// StackChan FW-A2 §3, §3.8: the link manager shell around the pure transition function
// (link_manager_core). The manager task calls RunOnce() in a loop: the 100 ms tick first (never
// delayed by a flood of notices), then at most one notice, each output run through the ports.
// Notices come through a bounded queue that never waits for room; a notice that does not fit
// sets the "lost" flag and the manager restarts (it never drops one silently). Its mutex is a
// leaf held only to push, pop or read a count, so a poster may wait for that much at most. No ESP-IDF here: the
// ports are the real world in plan 2B, fakes in the host tests.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "link_manager_core.h"

namespace stackchan::link {

constexpr int64_t kTickUs = 100'000;
constexpr size_t kNoticeQueueLen = 32;  // one attempt's notices (17) + Shutdown + room (§3.8)
constexpr size_t kEndReasonCount = static_cast<size_t>(EndReason::kShutdown) + 1;

class NoticeQueue {
public:
    explicit NoticeQueue(size_t capacity = kNoticeQueueLen) : capacity_(capacity) {}
    // Never waits for room (only for the leaf mutex, held to push / pop / read a count).
    // False: no room; the lost flag is set (the manager restarts).
    bool Post(const Input& in);
    std::optional<Input> Take(int64_t timeout_us);
    bool lost() const { return lost_.load(); }
    size_t min_free() const;

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Input> items_;
    std::atomic<bool> lost_{false};
    size_t min_free_ = kNoticeQueueLen;
};

// What the outputs do (plan 2B). Called from the manager task only, never under its own lock;
// Restart also from a connect worker whose link's task cannot be seen to stop (it touches nothing
// of the manager's state).
class ManagerPorts {
public:
    virtual ~ManagerPorts() = default;
    virtual int64_t NowUs() = 0;
    virtual int64_t CtrlLastRxUs() = 0;  // the control link's last receive (atomic in 2B)
    // True when the next audio connection would move conn_index past 65535 (design §3.1)
    virtual bool NextConnectionWraps() = 0;
    virtual void ConnectAudio(uint32_t attempt) = 0;
    virtual void SendAudioHello(uint32_t attempt, uint64_t e) = 0;
    virtual void ConnectCtrl(uint32_t attempt, uint64_t e) = 0;
    virtual void SendCtrlHello(uint64_t e) = 0;
    virtual void BindGate(uint64_t e) = 0;
    virtual void SendReady(uint64_t e) = 0;
    virtual void PostLinkUp(uint64_t e) = 0;  // the gate makes LinkUp under its lock
    virtual void StopForDeath(uint64_t e, EndReason reason) = 0;
    virtual void UnbindGate(uint64_t e) = 0;
    virtual void CloseQueues(uint64_t e, bool keep_ctrl) = 0;
    virtual void PostLinkDown(uint64_t e) = 0;
    virtual void RequestStop(uint64_t e) = 0;
    virtual void Destroy(uint64_t e) = 0;
    // Count it in NVS (leave the Settings scope first: its destructor commits), log, esp_restart.
    virtual void Restart(uint64_t e, const char* why) = 0;
};

class LinkManager {
public:
    LinkManager(ManagerPorts* ports, NoticeQueue* queue);
    // One turn of the manager task (the task calls it forever).
    void RunOnce();
    State state() const { return s_; }
    // The pairs ended, by reason (contract §5.1 stat). Any task may read it.
    uint32_t ends(EndReason reason) const { return ends_[static_cast<size_t>(reason)].load(); }
    // The transition function's stale inputs and duplicate end requests (design §6.2), copied for stat
    uint32_t stale_inputs() const { return stale_inputs_.load(); }
    uint32_t duplicate_ends() const { return duplicate_ends_.load(); }
    uint32_t ticks() const { return ticks_; }
    bool restarted() const { return restarted_; }

private:
    void Run(const StepResult& r);

    ManagerPorts* ports_;
    NoticeQueue* queue_;
    State s_;
    int64_t next_tick_us_ = 0;
    uint32_t ticks_ = 0;
    bool restarted_ = false;  // a host test sees Restart return; ESP never comes back
    std::array<std::atomic<uint32_t>, kEndReasonCount> ends_{};
    std::atomic<uint32_t> stale_inputs_{0}, duplicate_ends_{0};
};

// One link's (audio or control) phases around its connect result (design §3.8, v10): the
// receive / send tasks run during the handshake, before the manager has read the result. An end
// they see before the result is left to the worker, which posts it right after the result, so
// the manager always reads the result first. Each side posts its own end once.
class LinkPhase {
public:
    enum class Side { kRx, kTx };
    // A receive / send task found the link ended. True: post the EndRequest now.
    // `reason` is kept when it is the first end of the link (the worker posts it after the result).
    // Every caller names its reason (no default: the first reason is never kNone by mistake).
    bool OnEnded(Side side, EndReason reason);
    // The worker posted the successful connect result. True: post the EndRequest for an end
    // seen before the result.
    bool OnResultPosted();
    // The hello reply of this link is reported once (§3.8); later hellos are logged and dropped.
    bool TakeHelloReply() { return !hello_.exchange(true); }
    // The worker's end request: the first reason a task saw before the result, or `fallback`
    // (Claude review 152 Minor 1: a close of the gateway, a full queue or F2 is not lost as "closed").
    EndReason EndReasonOr(EndReason fallback) const;

private:
    enum : uint8_t { kBeforeResult, kEndedBeforeResult, kAfterResult };
    std::atomic<uint8_t> phase_{kBeforeResult};
    std::atomic<bool> rx_posted_{false}, tx_posted_{false}, hello_{false};
    std::atomic<EndReason> first_reason_{EndReason::kNone};
};

}  // namespace stackchan::link
