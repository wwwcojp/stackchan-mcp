// StackChan FW-A2 plan 2B-1 (design §1.1, §3.4, §3.6, §4.2): one link (the audio or the control
// WebSocket of one attempt) on the ESP. The connect worker makes it: TCP within 3 s and the
// handshake within 3 s, both cut into 200 ms slices that look at the attempt's stop request. Only
// after the handshake succeeded are the receive and send tasks started, so a failed connect
// leaves nothing running (design §3.4: the worker destroys what it made before reporting).
// Each task posts its own TaskExited as the very last thing; the manager destroys the link only
// after every task it started has exited.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "connect_plan.h"
#include "esp_tcp.h"
#include "link_manager.h"
#include "rx_link.h"
#include "send_queue.h"
#include "tx_loop.h"

namespace stackchan::link {

constexpr int kTcpConnectMs = 3000;  // design §3.6
constexpr int kHandshakeMs = 3000;

struct LinkTasks {
    EspTcpTaskOptions rx;
    const char* tx_name;
    UBaseType_t tx_priority;
    uint32_t tx_stack;
};
// design §1.1: audio receive 1, audio send 3, control receive and send 5
LinkTasks TasksFor(LinkSide side);

struct LinkParams {
    LinkSide side = LinkSide::kAudio;
    uint32_t attempt = 0;
    uint64_t e = 0;
    std::string session_id;  // the control link: the audio hello reply's session
    ConnectInputs inputs;    // NVS and Kconfig as read for the attempt (the control link reuses them)
    ConnectPlan plan;
    net::SendQueue* queue = nullptr;  // the side's send queue (it outlives every link)
    NoticeQueue* notices = nullptr;
    RxPorts rx_ports;
    // the gate port of both tasks (F2, a full queue); now / send / stop / mask / post / idle are
    // the link's own
    std::function<void(uint64_t e, EndReason reason)> stop_for_death;
};

class Link {
public:
    // The worker: connect and shake hands. nullptr when it failed (nothing is left running).
    static std::unique_ptr<Link> Connect(LinkParams params, const std::function<bool()>& stop);
    ~Link();

    enum class StartResult {
        kStarted,
        kFailed,  // nothing of this link runs: the worker may destroy it
        kStuck,   // the receive task runs but the send task did not start: never destroy it;
                  // the worker restarts the device (design §3.4: exits not confirmed)
    };
    // Start the receive and send tasks.
    StartResult Start();
    // The manager's stop request: both tasks leave within a slice.
    void RequestStop();

    LinkSide side() const { return p_.side; }
    uint64_t epoch() const { return p_.e; }
    LinkPhase& phase() { return phase_; }
    const RxLink& rx() const { return rx_; }
    const TxLoop& tx() const { return tx_; }  // read after the send task exited (stat totals)
    // The smallest free stack (bytes) each task measured on itself so far; 0 before (stat)
    uint32_t rx_stack_min() const { return rx_stack_min_.load(); }
    uint32_t tx_stack_min() const { return tx_stack_min_.load(); }
    int64_t last_rx_us() const { return last_rx_us_.load(); }
    const std::string& url() const { return p_.plan.url; }
    int protocol_version() const { return p_.plan.version; }
    const ConnectInputs& inputs() const { return p_.inputs; }

private:
    explicit Link(LinkParams params);
    bool Handshake(const std::function<bool()>& stop);
    static void TxMain(void* arg);
    bool Stopping() const { return stop_.load() || tcp_.stop_requested(); }

    LinkParams p_;
    EspTcp tcp_;
    LinkPhase phase_;
    std::atomic<int64_t> last_rx_us_{0};
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> rx_stack_min_{0}, tx_stack_min_{0};
    uint32_t rx_batches_ = 0;  // the receive task's own count: it measures its stack every 16th
    RxLink rx_;
    TxLoop tx_;
    std::string leftover_;  // bytes after the handshake response (frames): the receive task's first
};

}  // namespace stackchan::link
