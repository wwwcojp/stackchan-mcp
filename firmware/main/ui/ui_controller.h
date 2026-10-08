// StackChan FW-A2 §2.4: the UiController shell. The main task owns it: one FIFO of events (any
// task posts, never waiting), one event at a time through the pure Step (ui_controller_core),
// then its actions in order, outside the gate lock. What Step does not do is derived here from
// the state before and after one event (design §2.4 v10): the LED, the mouth start, the power
// save level and the detector while speaking. Also here: the touch classification for the
// non-conversation states, the Resync guard (Codex review 141 Important 1) and the one request
// that leaves the conversation (EnterNonConversation, Codex review 140 Important 4).
// No ESP-IDF: the ports are the real world in plan 2B, fakes in the host tests.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

#include "device_state.h"
#include "playback_gate.h"
#include "ui_controller_core.h"

namespace stackchan::ui {

bool IsConversation(DeviceState s);  // Idle, Listening, Speaking

// What the board does with a short touch (design §2.4 "タッチの分類")
enum class TouchRoute {
    kEnterWifiConfig,     // Starting
    kToggleAudioTesting,  // WifiConfiguring, AudioTesting
    kTouch,               // Idle, Listening, Speaking: the Touch event
    kDrop,                // anything else (Activating, Upgrading, FatalError, ...): log and drop
};
TouchRoute ClassifyTouch(DeviceState s);
// ToggleAudioTesting() on the main task: where to go, or nothing when the state moved on while
// the request waited (a settings-screen touch never becomes a conversation Toggle).
std::optional<DeviceState> AudioTestingToggleTarget(DeviceState s);

TouchReply ToTouchReply(gate::TouchOutcome o);  // plan 1 handoff 13
wire::ListenMode ToWireMode(Mode m);
// The gateway's listen as an event (the audio receive task posts it): start carries the mode and
// the profile, stop only the pair.
Event GwListenEvent(uint64_t e, const wire::GwListen& listen);

struct UiConfig {
    // CONFIG_WAKE_WORD_DETECTION_IN_LISTENING && IsAfeWakeWord() (Step's wake_in_listening)
    bool wake_in_listening = false;
    // IsAfeWakeWord(): today's Speaking transition keeps the detector on so a wake word can
    // interrupt the playback (application.cc:1172)
    bool wake_in_speaking = false;
    // CONFIG_SEND_WAKE_WORD_DATA: off in FW-A2 (design §2.4 v10), kSendWakeWord does nothing
    bool send_wake_word_data = false;
};

class UiPorts {
public:
    virtual ~UiPorts() = default;
    virtual DeviceState CurrentState() = 0;
    virtual bool SetDeviceState(DeviceState s) = 0;  // false: the state machine refused
    // the gate's OnTouch (one lock section); the UI's e, the mode of the listening to start
    virtual gate::TouchResult GateTouch(uint64_t e, wire::ListenMode mode, wire::DeviceAbortReason reason) = 0;
    virtual void ClearForListening() = 0;  // the gate's
    virtual void SendListenStart(uint64_t e, Mode mode) = 0;  // the audio send queue, with E
    virtual void SendListenStop(uint64_t e) = 0;
    virtual void SendWakeWord(uint64_t e) = 0;
    virtual void MicOn(Profile p) = 0;
    virtual void MicOff() = 0;
    virtual void PlayPopup() = 0;  // never waits (design §2.4)
    virtual void RequestDrain() = 0;
    virtual void WakeDetect(bool on) = 0;
    virtual void ArmTimer(uint32_t req) = 0;  // one-shot ListenTimeout(req)
    virtual void CancelTimer() = 0;
    virtual void ShowWaitingForLink() = 0;
    virtual void StopMouth() = 0;
    virtual void StartMouth() = 0;
    virtual void SetListeningLed(bool on) = 0;  // green 0,32,0 / off
    virtual void SetPerformance(bool performance) = 0;  // SetPowerSaveLevel(PERFORMANCE / LOW_POWER)
};

struct UiStats {
    uint32_t dropped_resync = 0;  // a Resync that came while not in a conversation
    uint32_t gate_calls = 0;
    uint32_t stale = 0;
    uint32_t refused_transitions = 0;
    size_t max_depth = 0;
};

class UiController {
public:
    UiController(UiPorts* ports, UiConfig config);

    // Any task. Never waits. `wake` tells the main task (one event bit).
    void Post(const Event& ev);
    void SetWake(std::function<void()> wake) { wake_ = std::move(wake); }
    // The main task: one event. False when the list was empty.
    bool ProcessOne();
    // The main task: leave the conversation for `target` (WifiConfiguring, Upgrading).
    void EnterNonConversation(DeviceState target);

    State state() const { return s_; }
    UiStats stats() const;

private:
    void Run(const State& before, const Event& ev, const StepResult& r);
    void Do(const Action& a);

    UiPorts* ports_;
    const UiConfig config_;
    State s_;
    mutable std::mutex mutex_;  // the list only (a leaf: nothing is called under it)
    std::deque<Event> list_;
    std::function<void()> wake_;
    UiStats stats_;
};

}  // namespace stackchan::ui
