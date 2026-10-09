#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "listening_profile.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "app_link.h"
#include "link_hub.h"
#include "link_manager.h"
#include "outbound.h"
#include "playback_gate.h"
#include "send_queue.h"
#include "stat_report.h"
#include "ui_controller.h"
#include "ui_ports_esp.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
// StackChan FW-A2 (design §2.4): UiController has events (the one bit that wakes the main task)
#define MAIN_EVENT_UI                   (1 << 13)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    // StackChan FW-A2 (design §1.2): the link manager's view, never UiController's state
    std::string GetConnectedGatewayUrl() const {
        stackchan::link::LinkHub* hub = hub_view_.load();
        return hub ? hub->ConnectedUrl() : "";
    }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    // StackChan FW-A2 (design §2.4, plan 2B-2a handoff 19): the public entries other boards call.
    // Thread-safe: each sets its event bit; the main task routes it by the device state then
    // (ui::RouteEntry) into a UiController event. The profile is not used (a device-started
    // listening is voice; the gateway's listen carries its own).
    void ToggleChatState();
    void StartListening(ListeningProfile profile = kListeningProfileVoice);
    void StopListening();
    // The board's classified touch (ui::ClassifyTouch): Touch in a conversation state (any task),
    // the settings screens' audio test (scheduled on the main task)
    void Touch();
    void ToggleAudioTesting();
    // The main task only: leave the conversation for WifiConfiguring / Upgrading (design §2.4)
    void EnterNonConversation(DeviceState target);

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    // StackChan FW-A2 (design §4.1): what the device sends on its own goes into the audio send
    // queue at once (link::Outbound; never waits, any task). An MCP reply goes out on the pair
    // its request came on (e); the others on the pair bound at send time.
    void SendMcpMessage(uint64_t e, const std::string& payload);
    void SendStackChanEvent(const char* event_type, const char* subtype, uint64_t duration_ms);
    // Phase 4.5 avatar: a board notice such as avatar_set_loaded (a JSON object, sent as it is)
    void SendJsonString(const std::string& json_str);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    
    // StackChan FW-A2 (design §3.5): the link objects live until the restart; this only ends the
    // pair and stops reconnecting (thread-safe).
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    bool has_server_time_ = false;
    bool assets_version_checked_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;

    // StackChan FW-A2: the link side (design §1, §2, §3). Made once and never destroyed (a restart
    // ends them): the queues, the gate, UiController and Outbound in Initialize(); the app ports
    // and the link hub in the first InitializeProtocol() (design §3.5).
    std::unique_ptr<stackchan::link::NoticeQueue> notices_;
    std::unique_ptr<stackchan::net::SendQueue> audio_queue_;
    std::unique_ptr<stackchan::net::SendQueue> ctrl_queue_;
    std::unique_ptr<stackchan::gate::PlaybackGate> gate_;
    std::unique_ptr<stackchan::ui::UiPortsEsp> ui_ports_;
    std::unique_ptr<stackchan::ui::UiController> ui_;
    std::unique_ptr<stackchan::link::Outbound> outbound_;
    std::unique_ptr<stackchan::link::AppLink> app_link_;
    std::unique_ptr<stackchan::link::LinkHub> hub_;
    // hub_ is made on the activation task; the other tasks read it through this (set after Start)
    std::atomic<stackchan::link::LinkHub*> hub_view_{nullptr};
    bool protocol_initialized_ = false;  // the activation task, once per boot

    // Event handlers
    void HandleStateChangedEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    // StackChan FW-A2
    void CreateLinkSide();
    void HandleEntry(stackchan::ui::Entry entry);
    void PostUi(stackchan::ui::EvKind kind);
    stackchan::link::StatInputs CollectStat();
    void ShowAppMessage(const stackchan::link::AppMessage& message);
    // Reboot / OTA (design §3.5): no reconnect, end the pair, wait for it at most 1 s
    void EndLinks();

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
