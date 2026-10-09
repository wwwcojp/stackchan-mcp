// StackChan FW-A2 plan 2B-2a (plan 2B-1 handoff 2, design §2.4, §4.1, §6.2): the receive side's
// app ports (HubDeps::app): the audio hello reply, the server audio, the app's JSON and stat. The
// receive tasks call them, so nothing here waits for the main task: what the main task does goes
// through `post_app` (Application::Schedule in plan 2B-2b). Pure: the application's work comes in
// as std::function ports (fakes in the host tests).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "audio_items.h"
#include "link_manager.h"
#include "playback_gate.h"
#include "rx_link.h"
#include "send_queue.h"
#include "stat_report.h"

struct cJSON;

namespace stackchan::link {

// What the app JSON asks of the main task (today's OnIncomingJson, minus the gate's and the UI's)
struct AppMessage {
    enum class Kind {
        kAssistantText,  // tts sentence_start: text
        kUserText,       // stt: text
        kEmotion,        // llm: emotion
        kAlert,          // alert: status, message, emotion
        kReboot,         // system reboot
        kCustom,         // custom (CONFIG_RECEIVE_CUSTOM_MESSAGE): text is the payload printed
    };
    Kind kind = Kind::kAssistantText;
    std::string text;
    std::string status;
    std::string emotion;
};

struct AppDeps {
    gate::PlaybackGate* gate = nullptr;
    net::SendQueue* ctrl_queue = nullptr;
    NoticeQueue* notices = nullptr;
    std::function<int64_t()> now_us;
    // The hello reply's server_time (today's ParseServerTime / ApplyServerTime), on the receive task
    std::function<void(const cJSON* server_time)> apply_server_time;
    // The server offered no control link (kAudioOnly): no LinkUp will come, the main task wakes
    // the power save timer anyway (design §2.4, plan 2A handoff 12)
    std::function<void()> on_audio_only;
    // AudioService::PushServerAudio: false when the decode queue is full (the packet stays here)
    std::function<bool(std::unique_ptr<AudioStreamPacket>& packet)> push_server_audio;
    // Application::Schedule of the display / alert / reboot work. Never waits.
    std::function<void(AppMessage message)> post_app;
    // avatar_set_fetch: today the board starts its fetch from the receive task
    std::function<void(const cJSON* root)> avatar_set_fetch;
    // mcp on the bound pair only, with that pair's E (plan 2B-2b carries it to the reply)
    std::function<void(uint64_t e, const cJSON* payload)> mcp;
    // stat: each input from where it is safe to read (locks, atomics)
    std::function<StatInputs()> stat_inputs;
    bool receive_custom = false;  // CONFIG_RECEIVE_CUSTOM_MESSAGE
};

// AppStats: stat_report.h (it goes into the stat reply)

class AppLink {
public:
    explicit AppLink(AppDeps deps);
    // The name of the first empty dependency, or nullptr. An empty std::function would abort on the
    // first message (Claude review 156 Minor 5): plan 2B-2b refuses to start the link with one.
    // avatar_set_fetch may be empty only if the board has no avatar (it is then not called).
    const char* Missing() const;
    // hello_reply, server_audio, app_json and stat; LinkHub fills the rest
    RxPorts Ports();

    int sample_rate() const { return sample_rate_.load(); }
    int frame_duration() const { return frame_duration_.load(); }
    AppStats stats() const;

    void OnHelloReply(const AudioHelloReply& reply, const cJSON* root);
    void OnServerAudio(uint64_t e, AudioIn audio);
    void OnAppJson(uint64_t e, const cJSON* root);
    void OnStat(uint64_t e, const std::string& req_id);

private:
    AppDeps d_;
    std::atomic<int> sample_rate_{24000};  // today's defaults (protocol.h)
    std::atomic<int> frame_duration_{60};
    std::atomic<uint32_t> mcp_unbound_{0}, unknown_{0}, stat_full_{0}, stat_closed_{0};
};

}  // namespace stackchan::link
