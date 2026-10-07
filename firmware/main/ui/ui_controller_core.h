// StackChan FW-A2 §2.4: the UiController's pure transition function Step.
// The main task owns the State. Events come through one FIFO; Step returns the new State
// and the effects, which the shell runs in order outside the gate lock. For Touch / WakeWord /
// Toggle the shell first calls the gate's OnTouch(e, mode, state.req + 1) (one lock section)
// and puts the reply into the Event. Model: spikes/spike9_fw_a2_model/UiCtrl.tla (one Step =
// one model step).
#pragma once

#include <cstdint>
#include <vector>

namespace stackchan::ui {

enum class Disp { kOther, kIdle, kListening, kSpeaking };  // kOther: a non-conversation state
enum class Mode { kManualStop, kAutoStop, kRealtime };
enum class Profile { kVoice, kRaw };

struct State {
    Disp disp = Disp::kOther;  // starts suspended (Starting/Activating) until Resync
    bool suspended = true;
    uint32_t e = 0;       // bound pair (0 = none)
    uint32_t chan_e = 0;  // logical channel's pair (0 = closed)
    bool gspk = false;    // what we know of the gate's speaking
    uint32_t grev = 0;    // the gate rev that knowledge is from
    uint32_t req = 0;     // the current listening request (+1 per start)
    Mode mode = Mode::kManualStop;
    Profile profile = Profile::kVoice;
    bool mic = false;
    bool wait = false;    // AutoStop: waiting for PlaybackDrained before opening the mic
    uint32_t timer_req = 0;  // armed ListenTimeout (0 = none)
    // A setting, not state: the shell sets it once from CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // && IsAfeWakeWord() (the current FW's Listening transition). The K151 build has it off,
    // so the detector stops while listening (Claude review 133 Important 1).
    bool wake_in_listening = false;
};

enum class TouchReply { kR5, kSendFailed, kUnbound, kNotSpeaking };

enum class EvKind {
    kTouch,
    kWakeWord,
    kToggle,
    kGateChanged,
    kLinkUp,
    kLinkDown,
    kGwListenStart,
    kGwListenStop,
    kListenTimeout,
    kPlaybackDrained,
    kResync,
    kSuspend,
};

struct Event {
    EvKind kind = EvKind::kTouch;
    uint32_t e = 0;        // GateChanged / LinkUp / LinkDown / GwListen*
    bool spk = false;      // GateChanged / LinkUp
    uint32_t rev = 0;      // GateChanged / LinkUp / the OnTouch reply
    uint32_t req = 0;      // ListenTimeout
    Mode mode = Mode::kManualStop;      // GwListenStart; WakeWord (the default mode)
    Profile profile = Profile::kVoice;  // GwListenStart
    TouchReply reply = TouchReply::kNotSpeaking;  // Touch / WakeWord / Toggle
};

enum class ActKind {
    kSetDisplay,       // disp
    kSendListenStart,  // e, mode
    kSendListenStop,   // e
    kSendWakeWord,     // e (encoded wake word audio and the detected notice)
    kClearForListening,
    kMicOn,            // profile
    kMicOff,
    kPlayPopup,
    kArmTimer,         // req
    kCancelTimer,
    kRequestDrain,     // AudioService posts PlaybackDrained when no server audio is queued
    kWakeDetect,       // on
    kShowWaitingForLink,
    kStopMouth,
};

struct Action {
    ActKind kind;
    Disp disp = Disp::kIdle;
    uint32_t e = 0;
    uint32_t req = 0;
    Mode mode = Mode::kManualStop;
    Profile profile = Profile::kVoice;
    bool on = false;
    bool operator==(const Action& o) const {
        return kind == o.kind && disp == o.disp && e == o.e && req == o.req && mode == o.mode &&
               profile == o.profile && on == o.on;
    }
};

struct StepResult {
    State state;
    std::vector<Action> actions;
    bool stale = false;  // the event was for an old pair / rev / req: nothing changed
};

StepResult Step(const State& s, const Event& ev);

// Whether the shell must call the gate's OnTouch(e, mode, req + 1) before Step for this event.
// False while suspended (Step drops the event: the gate must not stop or send anything) and for
// a wake word during raw listening. Codex review 131 Important 7.
bool NeedsGateDecision(const State& s, const Event& ev);

// Action builders (also used by tests)
Action SetDisplay(Disp d);
Action SendListenStart(uint32_t e, Mode m);
Action SendListenStop(uint32_t e);
Action SendWakeWord(uint32_t e);
Action MicOn(Profile p);
Action ArmTimer(uint32_t req);
Action WakeDetect(bool on);
Action Simple(ActKind k);

}  // namespace stackchan::ui
