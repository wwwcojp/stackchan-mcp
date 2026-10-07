// StackChan FW-A2 §3: the link manager's pure transition function. The manager task owns the
// State, feeds it inputs (results of the connect worker, notices from the receive tasks, a
// 100 ms tick with the monotonic time and the control link's last-receive time) and runs the
// outputs. Only this function decides stages, epochs and when a pair ends (once).
#pragma once

#include <cstdint>
#include <vector>

namespace stackchan::link {

enum class Stage {
    kWaiting,         // backoff before the next attempt
    kAudioConnect,    // connect worker: TCP + WebSocket handshake of the audio link
    kAudioHello,      // audio hello sent, waiting for the reply
    kAudioOnly,       // the server did not offer stackchan_ctrl: keep the link, no control link
    kCtrlConnect,     // connect worker: the control link
    kCtrlHello,       // control hello sent, waiting for the reply
    kReadySend,       // bound the gate, ready queued: waiting for ReadySent (no S6 any more)
    kBound,           // ready sent; LinkUp posted
    kEnding,          // ending E: flush (violation) -> stop -> wait exits -> destroy
    kStopped,         // shutdown requested (reboot, OTA): no reconnect
};

enum class EndReason {
    kNone,
    kAudioClosed,
    kCtrlClosed,
    kServerClose,   // K6: a WebSocket close from the gateway
    kF1,            // no receive on the control link for 5 s
    kF2,            // a send missed its deadline
    kS6,            // no control hello reply 5 s after the audio hello reply
    kHelloTimeout,  // no audio hello reply in 5 s
    kConnectFailed,
    kViolation,     // contract violation (gate): flush the control queue (done) first
    kQueueFull,
    kShutdown,
};

// Bits of the tasks whose exit must be confirmed before destroying the pair's objects
enum Task : uint32_t {
    kAudioRx = 1u << 0,
    kAudioTx = 1u << 1,
    kCtrlRx = 1u << 2,
    kCtrlTx = 1u << 3,
    kConnWorker = 1u << 4,
};

struct State {
    Stage stage = Stage::kWaiting;
    uint32_t attempt = 0;     // connect attempt id (results of older attempts are dropped)
    uint32_t e = 0;           // the pair being built / bound / ended
    int64_t audio_hello_reply_us = 0;  // S6 starts here (receive time, §3.2)
    int64_t deadline_us = 0;  // the current stage's deadline (hello, S6, flush, exits)
    uint32_t pending_exits = 0;  // Task bits still running while ending
    uint32_t exited = 0;         // Task bits of this attempt that exited before the end
    bool flush_ctrl = false;  // ending after a violation: wait for the control queue first
    bool flushing = false;
    EndReason reason = EndReason::kNone;
    uint32_t backoff_ms = 1000;
    int64_t retry_at_us = 0;
    bool shutdown = false;
    bool ctrl_link_up = false;  // a control link exists for e (its rx/tx tasks run)
    uint32_t ended_pairs = 0;
    uint32_t duplicate_ends = 0;  // a second end request for the same pair (counted only)
    uint32_t stale_inputs = 0;
};

enum class InKind {
    kTick,              // now_us, ctrl_last_rx_us
    kConnectResult,     // attempt, ok, which (kAudioRx: audio link / kCtrlRx: control link)
    kAudioHelloReply,   // attempt, e (fw_epoch of the hello), ctrl_offered, at_us
    kCtrlHelloReply,    // e
    kReadySent,         // e
    kEndRequest,        // e, reason, flush (violation)
    kCtrlFlushed,       // e: the control send task drained (or failed) its queue
    kTaskExited,        // e, task bit
    kShutdown,
};

struct Input {
    InKind kind = InKind::kTick;
    int64_t now_us = 0;
    int64_t ctrl_last_rx_us = 0;
    uint32_t attempt = 0;
    uint32_t e = 0;
    bool ok = false;
    bool ctrl_offered = false;
    uint32_t which = 0;
    int64_t at_us = 0;
    EndReason reason = EndReason::kNone;
    bool flush = false;
};

enum class OutKind {
    kConnectAudio,     // attempt
    kSendAudioHello,   // attempt (the hello carries fw_epoch; the reply tells e)
    kConnectCtrl,      // attempt, e
    kSendCtrlHello,    // e
    kBindGate,         // e
    kSendReady,        // e
    kPostLinkUp,       // e (the shell reads spk/rev under the gate lock)
    kStopForDeath,     // e, reason (gate: stop + dead + close the send queues)
    kUnbindGate,       // e
    kCloseQueues,      // e, keep_ctrl (keep the control queue to flush the done)
    kPostLinkDown,     // e
    kRequestStop,      // e (both links: receive/send tasks see the stop request)
    kDestroy,          // e
    kRestart,          // e (exits not confirmed: esp_restart, after counting in NVS)
};

struct Output {
    OutKind kind;
    uint32_t attempt = 0;
    uint32_t e = 0;
    EndReason reason = EndReason::kNone;
    bool keep_ctrl = false;
    bool operator==(const Output& o) const {
        return kind == o.kind && attempt == o.attempt && e == o.e && reason == o.reason && keep_ctrl == o.keep_ctrl;
    }
};

struct StepResult {
    State state;
    std::vector<Output> out;
};

constexpr int64_t kHelloTimeoutUs = 5'000'000;   // audio hello reply (§3.6)
constexpr int64_t kS6Us = 5'000'000;             // control hello reply after the audio hello reply
constexpr int64_t kF1Us = 5'000'000;             // no receive on the control link
constexpr int64_t kFlushUs = 2'000'000;          // the done's deadline (§4.1)
constexpr int64_t kExitWaitUs = 3'000'000;       // task exits (§3.4)
constexpr uint32_t kBackoffMaxMs = 15'000;

StepResult Step(const State& s, const Input& in);

}  // namespace stackchan::link
