// StackChan FW-A2 §2.4: UiController Step (rule table of the design, one branch per row).
#include "ui_controller_core.h"

#include <algorithm>

namespace stackchan::ui {

Action SetDisplay(Disp d) { Action a{ActKind::kSetDisplay}; a.disp = d; return a; }
Action SendListenStart(uint64_t e, Mode m) { Action a{ActKind::kSendListenStart}; a.e = e; a.mode = m; return a; }
Action SendListenStop(uint64_t e) { Action a{ActKind::kSendListenStop}; a.e = e; return a; }
Action SendWakeWord(uint64_t e) { Action a{ActKind::kSendWakeWord}; a.e = e; return a; }
Action MicOn(Profile p) { Action a{ActKind::kMicOn}; a.profile = p; return a; }
Action ArmTimer(uint32_t req) { Action a{ActKind::kArmTimer}; a.req = req; return a; }
Action WakeDetect(bool on) { Action a{ActKind::kWakeDetect}; a.on = on; return a; }
Action Simple(ActKind k) { return Action{k}; }

namespace {

using Acts = std::vector<Action>;

bool UserEvent(EvKind k) {
    return k == EvKind::kTouch || k == EvKind::kWakeWord || k == EvKind::kToggle ||
           k == EvKind::kGwListenStart || k == EvKind::kGwListenStop || k == EvKind::kListenTimeout ||
           k == EvKind::kPlaybackDrained;
}

void LeaveSpeaking(const State& s, Acts* a) {
    if (s.disp == Disp::kSpeaking) a->push_back(Simple(ActKind::kStopMouth));
}

// listen stop, mic off, timer off (the display is left to the caller)
State EndListening(State s, Acts* a) {
    a->push_back(SendListenStop(s.e));
    a->push_back(Simple(ActKind::kMicOff));
    a->push_back(Simple(ActKind::kCancelTimer));
    s.mic = false;
    s.wait = false;
    s.timer_req = 0;
    s.profile = Profile::kVoice;  // ListeningProfileAfterStop
    return s;
}

// 止める: EndListening, Idle (+ wake word detection)
State StopListening(State s, Acts* a) {
    s = EndListening(s, a);
    a->push_back(SetDisplay(Disp::kIdle));
    a->push_back(WakeDetect(true));
    s.disp = Disp::kIdle;
    return s;
}

// Where the display wants the wake word detector: Idle, and Listening on the voice path when
// the setting says so. Speaking is left to the shell (§2.5).
bool ListeningWantsWake(const State& s, Profile profile) {
    return s.wake_in_listening && profile == Profile::kVoice;
}

// The wake word detector stops itself when it fires. After a wake word that changes nothing,
// turn it back on where the display wants it.
void RestoreWakeDetect(const State& s, Acts* a) {
    if (s.disp == Disp::kIdle || (s.disp == Disp::kListening && ListeningWantsWake(s, s.profile))) {
        a->push_back(WakeDetect(true));
    }
}

// 始める (not after R5)
State StartListening(State s, Mode mode, Profile profile, bool wake_word, Acts* a) {
    if (s.e == 0) {
        a->push_back(Simple(ActKind::kShowWaitingForLink));
        a->push_back(WakeDetect(true));
        return s;
    }
    LeaveSpeaking(s, a);
    s.chan_e = s.e;
    s.req++;
    s.mode = mode;
    s.profile = profile;
    s.disp = Disp::kListening;
    a->push_back(SetDisplay(Disp::kListening));
    a->push_back(SendListenStart(s.e, mode));
    if (wake_word) a->push_back(SendWakeWord(s.e));
    if (mode == Mode::kAutoStop) {
        s.wait = true;  // AudioService posts PlaybackDrained (now, when nothing is queued)
        s.mic = false;
        a->push_back(Simple(ActKind::kRequestDrain));
    } else {
        a->push_back(Simple(ActKind::kClearForListening));
        a->push_back(MicOn(profile));
        a->push_back(Simple(ActKind::kPlayPopup));
        s.wait = false;
        s.mic = true;
    }
    s.timer_req = s.req;
    a->push_back(ArmTimer(s.req));
    a->push_back(WakeDetect(ListeningWantsWake(s, profile)));
    return s;
}

// R5: the gate stopped the playback and sent abort + listen start on this pair
State AfterR5(State s, Mode mode, Acts* a) {
    const bool was_listening = s.disp == Disp::kListening;
    const bool s0_mic_voice = s.mic && s.profile == Profile::kVoice;
    LeaveSpeaking(s, a);
    s.chan_e = s.e;
    s.req++;
    s.mode = mode;
    s.profile = Profile::kVoice;
    s.disp = Disp::kListening;
    s.mic = true;
    s.wait = false;
    s.timer_req = s.req;
    const bool mic_was_voice = s0_mic_voice;
    if (!was_listening) a->push_back(SetDisplay(Disp::kListening));
    // open (or switch to) the voice mic unless it is already open on the voice path
    // (AutoStop waiting: listening with the mic closed; raw listening: another path)
    if (!was_listening || !mic_was_voice) a->push_back(MicOn(Profile::kVoice));
    a->push_back(Simple(ActKind::kPlayPopup));
    a->push_back(ArmTimer(s.req));
    // always set it: voice listening per the setting, from whatever raw listening, the shell's
    // Speaking or the wake word that caused this R5 left (Codex review 132 Important 3)
    a->push_back(WakeDetect(ListeningWantsWake(s, Profile::kVoice)));
    return s;
}

State ToSpeaking(State s, Acts* a) {
    if (s.disp == Disp::kListening) {
        a->push_back(SendListenStop(s.e));
        a->push_back(Simple(ActKind::kMicOff));
        a->push_back(Simple(ActKind::kCancelTimer));
    }
    a->push_back(SetDisplay(Disp::kSpeaking));
    s.disp = Disp::kSpeaking;
    s.mic = false;
    s.wait = false;
    s.timer_req = 0;
    return s;
}

State SpeakingToIdle(State s, Acts* a) {
    a->push_back(Simple(ActKind::kStopMouth));
    a->push_back(SetDisplay(Disp::kIdle));
    a->push_back(WakeDetect(true));
    s.disp = Disp::kIdle;
    return s;
}

StepResult Stale(const State& s) {
    StepResult r;
    r.state = s;
    r.stale = true;
    return r;
}

StepResult OnTouchLike(const State& s, const Event& ev) {
    StepResult r;
    r.state = s;
    Acts* a = &r.actions;
    const bool wake = ev.kind == EvKind::kWakeWord;
    const Mode mode = wake ? ev.mode : Mode::kManualStop;
    if (wake && s.disp == Disp::kListening && s.profile == Profile::kRaw) {
        // raw listening: the shell must not call OnTouch; the wake word is dropped
        a->push_back(WakeDetect(false));
        return r;
    }
    switch (ev.reply) {
        case TouchReply::kSendFailed:
            if (wake) RestoreWakeDetect(s, a);
            return r;  // the gate ends the pair; LinkDown follows
        case TouchReply::kUnbound:
            // also while the UI still knows a pair: the gate was unbound before LinkDown arrived
            a->push_back(Simple(ActKind::kShowWaitingForLink));
            RestoreWakeDetect(s, a);
            return r;
        case TouchReply::kR5: {
            State n = s;
            n.grev = std::max(n.grev, ev.rev);
            n.gspk = false;
            r.state = AfterR5(n, mode, a);
            return r;
        }
        case TouchReply::kNotSpeaking: {
            State n = s;
            n.grev = std::max(n.grev, ev.rev);
            n.gspk = false;
            if (n.disp == Disp::kListening) {
                if (ev.kind == EvKind::kTouch) {
                    r.state = StopListening(n, a);
                } else if (ev.kind == EvKind::kToggle) {
                    r.state = StopListening(n, a);
                    r.state.chan_e = 0;
                } else {  // wake word while listening: re-send listen start
                    a->push_back(SendListenStart(n.e, n.mode));
                    a->push_back(Simple(ActKind::kPlayPopup));
                    a->push_back(WakeDetect(ListeningWantsWake(n, n.profile)));
                    r.state = n;
                }
            } else {
                r.state = StartListening(n, mode, Profile::kVoice, wake, a);
            }
            return r;
        }
    }
    return r;
}

}  // namespace

StepResult Step(const State& s, const Event& ev) {
    StepResult r;
    r.state = s;
    Acts& a = r.actions;
    if (s.suspended && UserEvent(ev.kind)) return r;  // dropped (recorded by the shell)
    const bool show = !s.suspended;
    switch (ev.kind) {
        case EvKind::kLinkUp: {
            State n = s;
            n.e = ev.e;
            n.gspk = ev.spk;
            n.grev = std::max(n.grev, ev.rev);
            if (show && ev.spk && (n.disp == Disp::kIdle || n.disp == Disp::kListening)) n = ToSpeaking(n, &a);
            r.state = n;
            return r;
        }
        case EvKind::kLinkDown: {
            if (ev.e == 0 || ev.e != s.e) return Stale(s);
            State n = s;
            n.e = 0;
            n.gspk = false;
            if (n.chan_e == ev.e) n.chan_e = 0;
            if (show && n.disp == Disp::kListening) {
                a.push_back(Simple(ActKind::kMicOff));
                a.push_back(Simple(ActKind::kCancelTimer));
                a.push_back(SetDisplay(Disp::kIdle));
                a.push_back(WakeDetect(true));
                n.disp = Disp::kIdle;
                n.mic = false;
                n.wait = false;
                n.timer_req = 0;
                n.profile = Profile::kVoice;
            } else if (show && n.disp == Disp::kSpeaking) {
                n = SpeakingToIdle(n, &a);
            }
            r.state = n;
            return r;
        }
        case EvKind::kGateChanged: {
            if (ev.e == 0 || ev.e != s.e || ev.rev <= s.grev) return Stale(s);
            State n = s;
            n.grev = ev.rev;
            n.gspk = ev.spk;
            if (show && ev.spk && (n.disp == Disp::kIdle || n.disp == Disp::kListening)) {
                n = ToSpeaking(n, &a);
            } else if (show && !ev.spk && n.disp == Disp::kSpeaking) {
                n = SpeakingToIdle(n, &a);
            }
            r.state = n;
            return r;
        }
        case EvKind::kTouch:
        case EvKind::kWakeWord:
        case EvKind::kToggle:
            return OnTouchLike(s, ev);
        case EvKind::kGwListenStart:
            if (ev.e == 0 || ev.e != s.e) return Stale(s);
            // dropped unless Idle. While the gate speaks the display is Speaking (Step keeps
            // gspk => kSpeaking outside suspension), so this also drops it during playback.
            if (s.disp != Disp::kIdle) return r;
            r.state = StartListening(s, ev.mode, ev.profile, false, &a);
            return r;
        case EvKind::kGwListenStop:
            if (ev.e == 0 || ev.e != s.e) return Stale(s);
            if (s.disp == Disp::kListening) r.state = StopListening(s, &a);
            return r;
        case EvKind::kListenTimeout:
            if (s.disp != Disp::kListening || ev.req != s.req) return Stale(s);
            r.state = StopListening(s, &a);
            return r;
        case EvKind::kPlaybackDrained:
            if (s.disp == Disp::kListening && s.wait) {
                a.push_back(MicOn(s.profile));
                a.push_back(Simple(ActKind::kPlayPopup));
                r.state.wait = false;
                r.state.mic = true;
            }
            return r;
        case EvKind::kResync: {
            // after a non-conversation state, or after an error from a conversation state
            // (MAIN_EVENT_ERROR -> Idle -> Resync). The display outside is not ours any more:
            // end any listening, then always show the target display (Codex review 131
            // Important 3, 132 Important 2)
            State n = s;
            const bool conversation = !n.suspended;
            if (conversation && n.disp == Disp::kListening) n = EndListening(n, &a);
            n.suspended = false;
            if (n.gspk && n.e != 0) {
                a.push_back(SetDisplay(Disp::kSpeaking));
                n.disp = Disp::kSpeaking;
            } else {
                // no StopMouth: in a conversation, Speaking implies gspk && e (the enumeration
                // checks it), so the display here is Idle or another state's
                a.push_back(SetDisplay(Disp::kIdle));
                a.push_back(WakeDetect(true));
                n.disp = Disp::kIdle;
            }
            r.state = n;
            return r;
        }
        case EvKind::kSuspend: {
            State n = s;
            if (n.disp == Disp::kListening) {
                a.push_back(SendListenStop(n.e));
                a.push_back(Simple(ActKind::kMicOff));
                a.push_back(Simple(ActKind::kCancelTimer));
            } else if (n.disp == Disp::kSpeaking) {
                a.push_back(Simple(ActKind::kStopMouth));
            }
            n.disp = Disp::kOther;
            n.suspended = true;
            n.profile = Profile::kVoice;  // like 止める (Claude review 133 Minor 5)
            n.mic = false;
            n.wait = false;
            n.timer_req = 0;
            r.state = n;
            return r;
        }
    }
    return r;
}

bool NeedsGateDecision(const State& s, const Event& ev) {
    if (ev.kind != EvKind::kTouch && ev.kind != EvKind::kWakeWord && ev.kind != EvKind::kToggle) return false;
    if (s.suspended) return false;  // dropped by Step: never touch the gate
    if (ev.kind == EvKind::kWakeWord && s.disp == Disp::kListening && s.profile == Profile::kRaw) return false;
    return true;
}

}  // namespace stackchan::ui
