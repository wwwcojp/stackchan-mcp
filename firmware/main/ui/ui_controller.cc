// StackChan FW-A2 §2.4: the UiController shell (see ui_controller.h).
#include "ui_controller.h"

#include <algorithm>

namespace stackchan::ui {

bool IsConversation(DeviceState s) {
    return s == kDeviceStateIdle || s == kDeviceStateListening || s == kDeviceStateSpeaking;
}

TouchRoute ClassifyTouch(DeviceState s) {
    switch (s) {
        case kDeviceStateStarting: return TouchRoute::kEnterWifiConfig;
        case kDeviceStateWifiConfiguring:
        case kDeviceStateAudioTesting: return TouchRoute::kToggleAudioTesting;
        case kDeviceStateIdle:
        case kDeviceStateListening:
        case kDeviceStateSpeaking: return TouchRoute::kTouch;
        default: return TouchRoute::kDrop;
    }
}

std::optional<DeviceState> AudioTestingToggleTarget(DeviceState s) {
    if (s == kDeviceStateWifiConfiguring) return kDeviceStateAudioTesting;
    if (s == kDeviceStateAudioTesting) return kDeviceStateWifiConfiguring;
    return std::nullopt;
}

TouchReply ToTouchReply(gate::TouchOutcome o) {
    switch (o) {
        case gate::TouchOutcome::kR5: return TouchReply::kR5;
        case gate::TouchOutcome::kSendFailed: return TouchReply::kSendFailed;
        case gate::TouchOutcome::kNotSpeaking: return TouchReply::kNotSpeaking;
        case gate::TouchOutcome::kUnbound: break;
    }
    return TouchReply::kUnbound;
}

wire::ListenMode ToWireMode(Mode m) {
    switch (m) {
        case Mode::kAutoStop: return wire::ListenMode::kAutoStop;
        case Mode::kRealtime: return wire::ListenMode::kRealtime;
        case Mode::kManualStop: break;
    }
    return wire::ListenMode::kManualStop;
}

namespace {

DeviceState ToDeviceState(Disp d) {
    switch (d) {
        case Disp::kListening: return kDeviceStateListening;
        case Disp::kSpeaking: return kDeviceStateSpeaking;
        default: return kDeviceStateIdle;
    }
}

}  // namespace

UiController::UiController(UiPorts* ports, UiConfig config) : ports_(ports), config_(config) {
    s_.wake_in_listening = config.wake_in_listening;
}

void UiController::Post(const Event& ev) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        list_.push_back(ev);
        stats_.max_depth = std::max(stats_.max_depth, list_.size());
    }
    if (wake_) wake_();
}

UiStats UiController::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool UiController::ProcessOne() {
    Event ev;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (list_.empty()) return false;
        ev = list_.front();
        list_.pop_front();
    }
    // The outside screen belongs to a non-conversation state: a Resync posted before
    // EnterNonConversation must not resume the conversation behind it (Codex review 141 I1).
    // Only the main task changes the state machine, and this runs on it.
    if (ev.kind == EvKind::kResync && !IsConversation(ports_->CurrentState())) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.dropped_resync++;
        return true;
    }
    if (NeedsGateDecision(s_, ev)) {  // the gate decides first, in one lock section
        const bool wake = ev.kind == EvKind::kWakeWord;
        const Mode mode = wake ? ev.mode : Mode::kManualStop;
        const gate::TouchResult t = ports_->GateTouch(
            s_.e, ToWireMode(mode), wake ? wire::DeviceAbortReason::kWakeWord : wire::DeviceAbortReason::kTouch);
        ev.reply = s_.e == 0 ? TouchReply::kUnbound : ToTouchReply(t.outcome);
        ev.rev = t.rev;
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.gate_calls++;
    }
    const State before = s_;
    Run(before, ev, Step(s_, ev));
    return true;
}

void UiController::Run(const State& before, const Event& ev, const StepResult& r) {
    s_ = r.state;
    if (r.stale) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.stale++;
    }
    for (const Action& a : r.actions) Do(a);
    const State& after = s_;
    // what Step does not do, from the state before and after this one event (design §2.4 v10)
    if (before.disp != Disp::kListening && after.disp == Disp::kListening) ports_->SetListeningLed(true);
    if (before.disp == Disp::kListening && after.disp != Disp::kListening) ports_->SetListeningLed(false);
    if (before.disp != Disp::kSpeaking && after.disp == Disp::kSpeaking) {
        ports_->StartMouth();
        ports_->WakeDetect(config_.wake_in_speaking);  // a wake word may interrupt the playback
    }
    // a wake word that changed nothing while speaking stopped the detector (it stops itself on
    // a detection): the shell owns the detector while speaking (plan 1 handoff 5)
    if (ev.kind == EvKind::kWakeWord && after.disp == Disp::kSpeaking &&
        (ev.reply == TouchReply::kSendFailed || ev.reply == TouchReply::kUnbound)) {
        ports_->WakeDetect(config_.wake_in_speaking);
    }
    if ((before.e == 0 && after.e != 0) || (before.chan_e == 0 && after.chan_e != 0)) {
        ports_->SetPerformance(true);  // also wakes the power save timer (dims, powers off)
    } else if (before.chan_e != 0 && after.chan_e == 0) {
        ports_->SetPerformance(false);
    }
}

void UiController::Do(const Action& a) {
    switch (a.kind) {
        case ActKind::kSetDisplay:
            if (!ports_->SetDeviceState(ToDeviceState(a.disp))) {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.refused_transitions++;
            }
            break;
        case ActKind::kSendListenStart: ports_->SendListenStart(a.e, a.mode); break;
        case ActKind::kSendListenStop: ports_->SendListenStop(a.e); break;
        case ActKind::kSendWakeWord:
            if (config_.send_wake_word_data) ports_->SendWakeWord(a.e);
            break;
        case ActKind::kClearForListening: ports_->ClearForListening(); break;
        case ActKind::kMicOn: ports_->MicOn(a.profile); break;
        case ActKind::kMicOff: ports_->MicOff(); break;
        case ActKind::kPlayPopup: ports_->PlayPopup(); break;
        case ActKind::kArmTimer: ports_->ArmTimer(a.req); break;
        case ActKind::kCancelTimer: ports_->CancelTimer(); break;
        case ActKind::kRequestDrain: ports_->RequestDrain(); break;
        case ActKind::kWakeDetect: ports_->WakeDetect(a.on); break;
        case ActKind::kShowWaitingForLink: ports_->ShowWaitingForLink(); break;
        case ActKind::kStopMouth: ports_->StopMouth(); break;
    }
}

void UiController::EnterNonConversation(DeviceState target) {
    const DeviceState cur = ports_->CurrentState();
    if (IsConversation(cur)) {
        Event suspend;
        suspend.kind = EvKind::kSuspend;
        const State before = s_;
        Run(before, suspend, Step(s_, suspend));  // stop listening, mic off, timer, mouth
        // the state machine cannot go from Listening / Speaking to a non-conversation state
        if (cur != kDeviceStateIdle) ports_->SetDeviceState(kDeviceStateIdle);
    }
    if (!ports_->SetDeviceState(target)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.refused_transitions++;
        }
        Event resync;
        resync.kind = EvKind::kResync;
        Post(resync);  // back to the conversation (the state machine is still Idle)
    }
}

}  // namespace stackchan::ui
