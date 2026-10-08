// StackChan FW-A2 plan 2B-1: a link's send task, pure (see tx_loop.h).
#include "tx_loop.h"

#include <cJSON.h>

#include <string>

#include "ws_frame.h"

namespace stackchan::link {

namespace {

Input Notice(InKind kind, uint64_t e) {
    Input in;
    in.kind = kind;
    in.e = e;
    return in;
}

}  // namespace

net::PushResult QueueJsonOrEnd(net::SendQueue& q, uint64_t e, std::string json, int64_t now_us,
                               const std::function<void(uint64_t e, EndReason reason)>& stop_for_death,
                               const std::function<bool(const Input&)>& post) {
    const net::PushResult r = q.Push(e, net::ElemKind::kJson, std::move(json), now_us);
    if (r == net::PushResult::kFull) {
        stop_for_death(e, EndReason::kQueueFull);
        Input in = Notice(InKind::kEndRequest, e);
        in.reason = EndReason::kQueueFull;
        post(in);
    }
    return r;
}

TxLoop::TxLoop(LinkSide side, uint64_t e, net::SendQueue* queue, LinkPhase* phase, TxPorts ports)
    : side_(side), e_(e), queue_(queue), phase_(phase), p_(std::move(ports)) {}

void TxLoop::End(uint64_t e, EndReason reason) {
    if (phase_->OnEnded(LinkPhase::Side::kTx)) {
        Input in = Notice(InKind::kEndRequest, e);
        in.reason = reason;
        p_.post(in);
    }
}

// The control link only: the manager waits for the flushed control queue after a violation
// (the done). Posted once, when the queue is closed for a flush and empty, or when this task
// can send no more of it.
void TxLoop::PostFlushedOnce() {
    if (side_ != LinkSide::kCtrl || flushed_posted_) return;
    if (queue_->e() != e_) return;  // an earlier pair's flush: not this link's one notice
    flushed_posted_ = true;
    p_.post(Notice(InKind::kCtrlFlushed, queue_->e()));
}

bool TxLoop::RunOnce() {
    if (p_.stop_requested()) return false;
    std::optional<net::Elem> elem = queue_->Pop(net::kSliceUs);
    if (!elem) {
        if (queue_->FlushDone()) PostFlushedOnce();
        if (queue_->Drained()) p_.idle(kIdleUs);  // closed and empty: Pop would not wait
        return true;
    }
    if (elem->e != e_) {  // left by an earlier pair: never sent ahead of this link's hello
        stats_.stale++;
        return true;
    }

    wsframe::Opcode op = wsframe::Opcode::kBinary;
    std::string payload;
    bool ready = false;
    switch (elem->kind) {
        case net::ElemKind::kJson: {
            cJSON* root = cJSON_Parse(elem->payload.c_str());
            if (!cJSON_IsObject(root)) {
                cJSON_Delete(root);
                stats_.bad_json++;
                return true;
            }
            if (!stamp_ || stamp_e_ != elem->e) {  // the link's first JSON (the hello) is seq 1
                stamp_.emplace(elem->e);
                stamp_e_ = elem->e;
            }
            stamp_->Stamp(root);
            ready = wire::TypeOf(root) == "ready";
            char* text = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            if (text == nullptr) {
                stats_.bad_json++;
                return true;
            }
            payload = text;
            cJSON_free(text);
            op = wsframe::Opcode::kText;
            break;
        }
        case net::ElemKind::kMic:
            payload = std::move(elem->payload);
            op = wsframe::Opcode::kBinary;
            break;
        case net::ElemKind::kPong:
            payload = std::move(elem->payload);
            op = wsframe::Opcode::kPong;
            break;
        case net::ElemKind::kClose:
            op = wsframe::Opcode::kClose;
            break;
    }
    uint8_t mask[4];
    p_.mask(mask);
    const std::string frame = wsframe::Encode(op, payload.data(), payload.size(), mask);
    const net::SendOutcome out = net::SendAll(reinterpret_cast<const uint8_t*>(frame.data()), frame.size(),
                                              elem->deadline_us, p_.now_us, p_.send, p_.stop_requested);
    switch (out.result) {
        case net::SendResult::kOk:
            stats_.sent++;
            if (ready && side_ == LinkSide::kCtrl) p_.post(Notice(InKind::kReadySent, elem->e));
            if (queue_->FlushDone()) PostFlushedOnce();
            return true;
        case net::SendResult::kDeadline:
            stats_.f2++;
            p_.stop_for_death(elem->e, EndReason::kF2);  // the sound stops now (design §4.1)
            End(elem->e, EndReason::kF2);                // and the pair ends even if not bound
            if (queue_->Flushing()) PostFlushedOnce();
            return false;
        case net::SendResult::kError:
            stats_.errors++;
            End(elem->e, side_ == LinkSide::kAudio ? EndReason::kAudioClosed : EndReason::kCtrlClosed);
            if (queue_->Flushing()) PostFlushedOnce();
            return false;
        case net::SendResult::kStopped:
            return false;
    }
    return false;
}

}  // namespace stackchan::link
