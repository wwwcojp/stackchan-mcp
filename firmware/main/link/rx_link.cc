// StackChan FW-A2 plan 2B-1: a link's receive task, pure (see rx_link.h).
#include "rx_link.h"

#include <cJSON.h>

namespace stackchan::link {

RxLink::RxLink(LinkSide side, uint32_t attempt, uint64_t e, std::string session_id, int audio_version,
               LinkPhase* phase, RxPorts ports)
    : side_(side),
      attempt_(attempt),
      e_(e),
      audio_version_(audio_version),
      phase_(phase),
      p_(std::move(ports)),
      session_id_(std::move(session_id)) {}

void RxLink::Drop(const char* why) {
    stats_.dropped++;
    stats_.last_drop = why;
}

const char* MissingAppPort(const RxPorts& p) {
    if (!p.hello_reply) return "hello_reply";
    if (!p.server_audio) return "server_audio";
    if (!p.app_json) return "app_json";
    if (!p.stat) return "stat";
    return nullptr;
}

// The link ended: the pair ends once (the phase leaves it to the worker before the result).
void RxLink::End(EndReason reason) {
    if (ended_) return;
    ended_ = true;
    if (phase_->OnEnded(LinkPhase::Side::kRx, reason)) {
        Input in;
        in.kind = InKind::kEndRequest;
        in.e = e_;
        in.reason = reason;
        p_.post(in);
    }
}

void RxLink::OnDisconnected() { End(side_ == LinkSide::kAudio ? EndReason::kAudioClosed : EndReason::kCtrlClosed); }

void RxLink::OnBytes(const char* data, size_t len) {
    if (ended_) return;
    const int64_t now = p_.now_us();  // the receive time of every frame in these bytes
    const uint64_t frames_before = decoder_.frames();
    bool marked = false;
    decoder_.Feed(data, len);
    wsframe::Message m;
    for (;;) {
        const wsframe::Status s = decoder_.Next(&m);
        if (!marked && decoder_.frames() != frames_before) {
            marked = true;
            p_.received(now);  // F1 counts frames, a fragment included (design §3.3)
        }
        if (s == wsframe::Status::kNeedMore) return;
        if (s == wsframe::Status::kError) {
            Drop(decoder_.error());
            OnDisconnected();  // a broken stream cannot go on: the link ends
            return;
        }
        OnMessage(m, now);
        if (ended_) return;
    }
}

void RxLink::OnMessage(const wsframe::Message& m, int64_t now) {
    stats_.frames++;
    switch (m.kind) {
        case wsframe::Kind::kPing: {
            const net::PushResult r = e_ == 0 ? net::PushResult::kClosed : p_.push_pong(e_, m.payload, now);
            if (r == net::PushResult::kFull) {  // never dropped: the pair ends (design §4.1)
                p_.stop_for_death(e_, EndReason::kQueueFull);
                End(EndReason::kQueueFull);
            } else if (r != net::PushResult::kQueued) {
                stats_.pongs_lost++;  // the pair ended meanwhile: nothing to answer for
            }
            return;
        }
        case wsframe::Kind::kPong:
            return;
        case wsframe::Kind::kClose:
            End(EndReason::kServerClose);  // K6
            return;
        case wsframe::Kind::kText:
            OnText(m.payload, now);
            return;
        case wsframe::Kind::kBinary: {
            if (side_ == LinkSide::kCtrl) {
                Drop("a binary frame on the control link");
                return;
            }
            AudioIn audio;
            if (!DecodeAudioFrame(audio_version_, reinterpret_cast<const uint8_t*>(m.payload.data()), m.payload.size(),
                                  &audio)) {
                stats_.bad_audio++;
                return;
            }
            p_.server_audio(e_, std::move(audio));
            return;
        }
    }
}

void RxLink::OnText(const std::string& text, int64_t now) {
    cJSON* root = cJSON_Parse(text.c_str());
    const uint64_t e = e_;
    if (side_ == LinkSide::kCtrl) {
        const CtrlRouted r = RouteCtrlText(root, session_id_);
        switch (r.route) {
            case CtrlRoute::kHelloReply:
                if (phase_->TakeHelloReply()) {
                    Input in;
                    in.kind = InKind::kCtrlHelloReply;
                    in.e = r.audio_epoch;  // the manager compares it with the pair (S4.1)
                    p_.post(in);
                } else {
                    stats_.extra_hellos++;
                }
                break;
            case CtrlRoute::kAbort:
                p_.abort(e, r.abort);
                break;
            case CtrlRoute::kStat:
                p_.stat(e, r.req_id);
                break;
            case CtrlRoute::kDrop:
                Drop(r.why);
                break;
        }
        cJSON_Delete(root);
        return;
    }
    const AudioRouted r = RouteAudioText(root, session_id_);
    switch (r.route) {
        case AudioRoute::kHelloReply: {
            AudioHelloReply reply;
            if (!ParseAudioHelloReply(root, &reply)) {
                // Not reported: the manager's 5 s hello deadline ends the pair (plan 2B-1 decision)
                Drop("audio hello reply without the websocket transport or a session");
                break;
            }
            if (!phase_->TakeHelloReply()) {
                stats_.extra_hellos++;
                break;
            }
            session_id_ = reply.session_id;  // before the notice: the manager reads it after
            p_.hello_reply(reply, root);
            Input in;
            in.kind = InKind::kAudioHelloReply;
            in.attempt = attempt_;
            in.e = e;
            in.ctrl_offered = reply.ctrl_offered;
            in.at_us = now;  // S6 counts from the receive, not from after the app's callback (§3.2)
            p_.post(in);
            break;
        }
        case AudioRoute::kTtsStart:
            p_.tts_start(e, r.tts_start);
            break;
        case AudioRoute::kTtsStop:
            p_.tts_stop(e, r.tts_stop_gen);
            break;
        case AudioRoute::kListen:
            p_.gw_listen(e, r.listen);
            break;
        case AudioRoute::kTtsOther:
        case AudioRoute::kApp:
            p_.app_json(e, root);
            break;
        case AudioRoute::kDrop:
            Drop(r.why);
            break;
    }
    cJSON_Delete(root);
}

}  // namespace stackchan::link
