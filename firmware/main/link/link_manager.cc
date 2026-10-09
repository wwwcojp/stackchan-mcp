// StackChan FW-A2 §3, §3.8: the link manager shell (see link_manager.h).
#include "link_manager.h"

#include <algorithm>
#include <chrono>

namespace stackchan::link {

bool NoticeQueue::Post(const Input& in) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.size() >= capacity_) {
        lost_.store(true);
        return false;
    }
    items_.push_back(in);
    min_free_ = std::min(min_free_, capacity_ - items_.size());
    cv_.notify_one();
    return true;
}

std::optional<Input> NoticeQueue::Take(int64_t timeout_us) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (timeout_us > 0) {
        cv_.wait_for(lock, std::chrono::microseconds(timeout_us), [this] { return !items_.empty(); });
    }
    if (items_.empty()) return std::nullopt;
    Input in = items_.front();
    items_.pop_front();
    return in;
}

size_t NoticeQueue::min_free() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return min_free_;
}

LinkManager::LinkManager(ManagerPorts* ports, NoticeQueue* queue) : ports_(ports), queue_(queue) {}

void LinkManager::RunOnce() {
    const int64_t now = ports_->NowUs();
    if (queue_->lost()) {  // a notice was not delivered: the pair's lifetime is unknown now
        ports_->Restart(s_.e, "lost notice");
        restarted_ = true;
        return;
    }
    if (now >= next_tick_us_) {  // the tick first: a flood of notices never delays it
        Input t;
        t.kind = InKind::kTick;
        t.now_us = now;
        t.ctrl_last_rx_us = ports_->CtrlLastRxUs();
        next_tick_us_ = now + kTickUs;
        ticks_++;
        Run(Step(s_, t));
        return;
    }
    if (auto in = queue_->Take(next_tick_us_ - now)) {
        in->now_us = ports_->NowUs();  // the shell's clock: a producer cannot move the deadlines
        Run(Step(s_, *in));
    }
}

void LinkManager::Run(const StepResult& r) {
    if (r.state.ended_pairs != s_.ended_pairs) ends_[static_cast<size_t>(r.state.reason)].fetch_add(1);
    s_ = r.state;
    stale_inputs_.store(s_.stale_inputs);
    duplicate_ends_.store(s_.duplicate_ends);
    for (const Output& o : r.out) {
        switch (o.kind) {
            case OutKind::kConnectAudio:
                if (ports_->NextConnectionWraps()) {  // E would go down (design §3.1)
                    ports_->Restart(o.e, "conn_index wraps");
                    restarted_ = true;
                    return;
                }
                ports_->ConnectAudio(o.attempt);
                break;
            case OutKind::kSendAudioHello: ports_->SendAudioHello(o.attempt, o.e); break;
            case OutKind::kConnectCtrl: ports_->ConnectCtrl(o.attempt, o.e); break;
            case OutKind::kSendCtrlHello: ports_->SendCtrlHello(o.e); break;
            case OutKind::kBindGate: ports_->BindGate(o.e); break;
            case OutKind::kSendReady: ports_->SendReady(o.e); break;
            case OutKind::kPostLinkUp: ports_->PostLinkUp(o.e); break;
            case OutKind::kStopForDeath: ports_->StopForDeath(o.e, o.reason); break;
            case OutKind::kUnbindGate: ports_->UnbindGate(o.e); break;
            case OutKind::kCloseQueues: ports_->CloseQueues(o.e, o.keep_ctrl); break;
            case OutKind::kPostLinkDown: ports_->PostLinkDown(o.e); break;
            case OutKind::kRequestStop: ports_->RequestStop(o.e); break;
            case OutKind::kDestroy: ports_->Destroy(o.e); break;
            case OutKind::kRestart:
                ports_->Restart(o.e, "task exits not confirmed");
                restarted_ = true;
                return;
        }
    }
}

bool LinkPhase::OnEnded(Side side, EndReason reason) {
    // a side that left its end to the worker counts it as posted: at most one end per side
    // (two per link) reaches the manager's queue, whoever posts it (§3.8)
    std::atomic<bool>& posted = side == Side::kRx ? rx_posted_ : tx_posted_;
    if (posted.exchange(true)) return false;
    EndReason none = EndReason::kNone;  // kept before the phase moves: the worker reads it after
    first_reason_.compare_exchange_strong(none, reason);
    uint8_t p = kBeforeResult;
    if (phase_.compare_exchange_strong(p, kEndedBeforeResult)) return false;  // left to the worker
    if (p == kEndedBeforeResult) return false;  // the other side already left it to the worker
    return true;  // after the result: this side posts its own
}

bool LinkPhase::OnResultPosted() { return phase_.exchange(kAfterResult) == kEndedBeforeResult; }

EndReason LinkPhase::EndReasonOr(EndReason fallback) const {
    const EndReason r = first_reason_.load();
    return r != EndReason::kNone ? r : fallback;
}

}  // namespace stackchan::link
