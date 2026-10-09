// StackChan FW-A2 plan 2B-1 (design §3, §3.8): the link manager's ports on the ESP. The hub owns
// the manager task (link_mgr, priority 6), draws each pair's E (MessageStamp: boot_count * 65536 +
// conn_index, design §3.1), starts one connect worker per link and attempt (link_conn, priority
// 3), keeps the links of the current attempt and runs every output of the manager: hellos and
// ready into the send queues, the gate's bind / unbind / stop / LinkUp, LinkDown to UiController,
// the stop request, the destruction after every task exited, the restart. Plan 2B-1 builds it but
// nothing creates it yet (plan 2B-2 does, with the AudioService and the app).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "connect_plan.h"
#include "link_esp.h"
#include "link_manager.h"
#include "message_stamp.h"
#include "playback_gate.h"
#include "rx_link.h"
#include "send_queue.h"
#include "stat_report.h"
#include "ui_controller.h"

namespace stackchan::link {

struct HubDeps {
    NoticeQueue* notices = nullptr;
    net::SendQueue* audio_queue = nullptr;
    net::SendQueue* ctrl_queue = nullptr;
    gate::PlaybackGate* gate = nullptr;
    ui::UiController* ui = nullptr;
    uint32_t boot_count = 0;
    // The receive side's app ports (plan 2B-2): hello_reply, server_audio, app_json, stat. The hub
    // fills the rest (gate, UiController, the notices).
    RxPorts app;
    // NVS and Kconfig for one attempt, read on the worker
    std::function<ConnectInputs()> read_inputs;
    bool server_aec = false;
    int frame_duration_ms = 60;
};

class LinkHub : public ManagerPorts {
public:
    explicit LinkHub(HubDeps deps);
    // Start the manager task. False when it could not be created.
    bool Start();
    // Reboot / OTA (design §3.5): no reconnect; the pair ends.
    void RequestShutdown();
    // After RequestShutdown: the pair is over and none of its tasks runs (ShutdownComplete, written
    // by the manager task after each turn). Reboot and OTA wait for this, at most 1 s.
    bool Stopped() const { return stopped_.load(); }

    // Other tasks' view of the link (design §1.2; plan 2B-1 handoff 3, 4): atomics the manager task
    // writes after each turn. Never UiController's state.
    uint64_t BoundPair() const { return bound_e_.load(); }  // ready sent (kBound), else 0
    bool AudioOnly() const { return audio_only_.load(); }   // kAudioOnly: the audio link alone
    bool TransportConnected() const { return BoundPair() != 0 || AudioOnly(); }
    // The URL of the audio link of the view above (taken with it), else ""
    std::string ConnectedUrl();
    // The main task's mic audio (MAIN_EVENT_SEND_AUDIO): a kMic frame of the bound pair in the
    // audio send queue, in the audio link's Protocol-Version. Never waits.
    net::PushResult PushMic(uint32_t timestamp, const uint8_t* opus, size_t len);
    // stat: the hub's part (the pairs ended, restarts, the notice queue, the send queues, the link
    // totals, the stacks). The gate, the pipeline, the UI, the heap and the build are the caller's.
    void FillStat(StatInputs* in);

    // ManagerPorts (the manager task only)
    int64_t NowUs() override;
    int64_t CtrlLastRxUs() override;
    bool NextConnectionWraps() override;
    void ConnectAudio(uint32_t attempt) override;
    void SendAudioHello(uint32_t attempt, uint64_t e) override;
    void ConnectCtrl(uint32_t attempt, uint64_t e) override;
    void SendCtrlHello(uint64_t e) override;
    void BindGate(uint64_t e) override;
    void SendReady(uint64_t e) override;
    void PostLinkUp(uint64_t e) override;
    void StopForDeath(uint64_t e, EndReason reason) override;
    void UnbindGate(uint64_t e) override;
    void CloseQueues(uint64_t e, bool keep_ctrl) override;
    void PostLinkDown(uint64_t e) override;
    void RequestStop(uint64_t e) override;
    void Destroy(uint64_t e) override;
    void Restart(uint64_t e, const char* why) override;

private:
    struct Worker {
        LinkHub* hub;
        LinkSide side;
        uint32_t attempt;
        uint64_t e;
        std::string url;         // the control link: the audio link's URL
        std::string session_id;  // the control link: the audio hello reply's session
        ConnectInputs inputs;    // the control link: what the audio worker read (one read per
                                 // attempt; Claude review 149 Minor 4)
    };
    // What the worker posts after its C++ objects are gone (trivially destructible)
    struct WorkerOutcome {
        NoticeQueue* notices;
        Input result;
        Input end;
        Input exited;
        Link* link;  // the adopted link, or null
    };
    static WorkerOutcome RunWorker(Worker* w);
    static void WorkerMain(void* arg);
    static void ManagerMain(void* arg);
    void StartWorker(Worker* w);
    RxPorts RxPortsFor();
    // The worker hands over a started link. Under the leaf mutex; a stop requested for the
    // attempt meanwhile reaches the link at once.
    void Adopt(LinkSide side, uint32_t attempt, std::unique_ptr<Link> link);
    void Push(net::SendQueue* q, uint64_t e, const std::string& json, const char* what);
    void AddTotalsLocked(const Link& link);
    // The manager task, after each turn: the view above and its URL, together
    void Publish(uint64_t bound, bool audio_only);

    HubDeps d_;
    LinkManager manager_;
    MessageStamp stamp_;
    bool connected_once_ = false;
    std::atomic<uint32_t> stop_attempt_{0};
    std::mutex mutex_;  // a leaf: the links below
    std::unique_ptr<Link> audio_, ctrl_;
    uint32_t links_attempt_ = 0;
    LinkTotals audio_totals_, ctrl_totals_;  // links destroyed so far (under mutex_)
    StackMarks stack_marks_;                 // links destroyed so far (under mutex_)
    std::atomic<uint64_t> bound_e_{0};
    std::atomic<bool> audio_only_{false};
    std::string connected_url_;  // under mutex_, written with the two above (Publish)
    std::atomic<bool> stopped_{false};
    std::atomic<int> audio_version_{1};  // the audio link's Protocol-Version (SendAudioHello)
    std::atomic<uint32_t> mgr_stack_min_{0}, conn_stack_min_{0};
    uint32_t link_restarts_ = 0;  // NVS at construction (it only grows right before a restart)
};

}  // namespace stackchan::link
