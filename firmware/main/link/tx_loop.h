// StackChan FW-A2 plan 2B-1 (design §4.1, §3.4, §3.8): what a link's send task does, pure (no
// ESP-IDF): take the next element of the link's send queue, stamp a JSON with fw_epoch / seq in
// send order (wire::CtrlStamp: the pair's E and the link's own seq; the hello is 1), encode one
// WebSocket frame and send it under the element's one deadline (net::SendAll, F2). The ESP send
// task calls RunOnce() until it returns false, then posts its own TaskExited.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "link_manager.h"
#include "send_deadline.h"
#include "send_queue.h"
#include "wire.h"

namespace stackchan::link {

enum class LinkSide { kAudio, kCtrl };

// A closed queue makes Pop return at once: the task rests this long instead of spinning (before
// its pair opens the queue, after the pair ended, after the flush was reported).
constexpr int64_t kIdleUs = 20'000;

struct TxPorts {
    std::function<int64_t()> now_us;
    net::SendFn send;                       // one slice: EspTcp::SendSlice
    std::function<bool()> stop_requested;   // the link's stop request (the manager's RequestStop)
    std::function<void(uint8_t mask[4])> mask;  // a random frame mask
    // F2: stop the bound pair's sound (the gate does nothing for a pair it has not bound)
    std::function<void(uint64_t e, EndReason reason)> stop_for_death;
    std::function<bool(const Input&)> post;  // the manager's notice queue (never waits)
    std::function<void(int64_t us)> idle;    // rest (vTaskDelay on the ESP)
};

struct TxStats {
    uint32_t sent = 0;
    uint32_t f2 = 0;          // deadlines missed
    uint32_t errors = 0;      // send errors (the peer went away)
    uint32_t bad_json = 0;    // a queued JSON that did not parse (dropped)
    uint32_t stale = 0;       // left in the queue by an earlier pair (dropped, not sent)
};

// Queue a JSON that must go out in order (a hello, the ready). A full queue ends the pair: the
// gate stops the sound and the manager is asked to end E (design §4.1: JSON, pong and close are
// never dropped). A closed queue (the pair already ended) is only reported.
net::PushResult QueueJsonOrEnd(net::SendQueue& q, uint64_t e, std::string json, int64_t now_us,
                               const std::function<void(uint64_t e, EndReason reason)>& stop_for_death,
                               const std::function<bool(const Input&)>& post);

class TxLoop {
public:
    // e: the link's pair E. What another E left in the queue (an earlier pair's control queue
    // stays closed for its flush until this pair opens it, and this task starts before that)
    // is not this link's: it is neither sent nor reported (reviews 151/152 Important 1).
    TxLoop(LinkSide side, uint64_t e, net::SendQueue* queue, LinkPhase* phase, TxPorts ports);
    // One turn: wait up to 200 ms for an element and send it. False: leave the task (the stop
    // request, a failed send, a missed deadline). A failure ends the pair once: the gate stops the
    // sound (F2) and the link's phase decides whether this task posts the end request (after the
    // connect result) or the worker does (before it).
    bool RunOnce();
    const TxStats& stats() const { return stats_; }

private:
    void End(uint64_t e, EndReason reason);
    void PostFlushedOnce();

    const LinkSide side_;
    const uint64_t e_;
    net::SendQueue* queue_;
    LinkPhase* phase_;
    TxPorts p_;
    std::optional<wire::CtrlStamp> stamp_;
    uint64_t stamp_e_ = 0;
    bool flushed_posted_ = false;
    TxStats stats_;
};

}  // namespace stackchan::link
