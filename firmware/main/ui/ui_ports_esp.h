// StackChan FW-A2 plan 2B-2a (design §2.4, plan 2A handoff 10): UiController's ports on the ESP.
// The main task calls them (UiController::ProcessOne / EnterNonConversation), outside the gate
// lock; the listening timer's callback only posts ListenTimeout(req). Application makes them at
// start (plan 2B-2b).
#pragma once

#include <esp_timer.h>

#include <atomic>
#include <cstdint>
#include <functional>

#include "link_manager.h"
#include "playback_gate.h"
#include "send_queue.h"
#include "ui_controller.h"

class Application;
class AudioService;

namespace stackchan::ui {

constexpr int64_t kListenTimeoutUs = 30'000'000;  // today's LISTEN_TIMEOUT_MS (stackchan.cc)
constexpr int64_t kTimerSlackUs = 1'000'000;       // a firing this much before its due time is an earlier arm's

struct UiEspDeps {
    Application* app = nullptr;
    AudioService* audio = nullptr;
    gate::PlaybackGate* gate = nullptr;
    net::SendQueue* audio_queue = nullptr;
    link::NoticeQueue* notices = nullptr;
    UiController* ui = nullptr;  // the timer posts ListenTimeout here
    // The board's listening LED (K151: SetAllRgbLeds(0, 32, 0) and off). May be empty.
    std::function<void(bool on)> listening_led;
};

// The settings Step and the shell read once at start (design §2.4)
UiConfig MakeUiConfig(AudioService& audio);

class UiPortsEsp : public UiPorts {
public:
    explicit UiPortsEsp(UiEspDeps deps);
    ~UiPortsEsp() override;
    // The UiController is made with these ports, so it comes after them: set it before any
    // ArmTimer (the main task, at start).
    void BindUi(UiController* ui) { d_.ui = ui; }

    DeviceState CurrentState() override;
    bool SetDeviceState(DeviceState s) override;
    gate::TouchResult GateTouch(uint64_t e, wire::ListenMode mode, wire::DeviceAbortReason reason) override;
    void ClearForListening() override;
    void SendListenStart(uint64_t e, Mode mode) override;
    void SendListenStop(uint64_t e) override;
    void SendWakeWord(uint64_t e) override;
    void MicOn(Profile p) override;
    void MicOff() override;
    void PlayPopup() override;
    void RequestDrain() override;
    void WakeDetect(bool on) override;
    void ArmTimer(uint32_t req) override;
    void CancelTimer() override;
    void ShowWaitingForLink() override;
    void StopMouth() override;
    void StartMouth() override;
    void SetListeningLed(bool on) override;
    void SetPerformance(bool performance) override;

private:
    static void OnTimer(void* arg);
    void Queue(uint64_t e, cJSON* root, const char* what);

    UiEspDeps d_;
    esp_timer_handle_t timer_ = nullptr;
    // The main task writes the due time first, then req; the timer task reads req first, then the
    // due time. A firing of an earlier arm that reads the new req also sees the new due time, still
    // in the future, and is dropped (Claude review 156 Minor 7).
    std::atomic<int64_t> timer_due_us_{0};
    std::atomic<uint32_t> timer_req_{0};
};

}  // namespace stackchan::ui
