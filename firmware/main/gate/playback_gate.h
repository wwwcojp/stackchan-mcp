// StackChan FW-A2 §2.1-2.2: the playback gate shell. One lock (gate_mutex_) around the pure core
// (playback_gate_core); every entry takes it once and applies the core's result in the same
// section: the AudioService's stop / clear, the done and the R5 pair into the send queues, the
// GateChanged / LinkUp notices into the UI list and the end request to the link manager.
// Lock order (design §1.2): gate_mutex_ -> audio_queue_mutex_ (AudioSink), gate_mutex_ ->
// send_queue_mutex_ (one at a time), gate_mutex_ -> ui_queue_mutex_; the manager's queue is
// posted to without waiting. Nothing here is called back from under those locks.
// No ESP-IDF: the ports are given by the shell's owner (plan 2B), fakes in the host tests.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "link_manager_core.h"
#include "playback_gate_core.h"
#include "send_queue.h"
#include "wire.h"

struct cJSON;

namespace stackchan::gate {

// The AudioService side of the stop / start processing (plan 2B), called under gate_mutex_.
class AudioSink {
public:
    virtual ~AudioSink() = default;
    // A stop: the gate's stop_serial is now `serial`. Clears the queues when its book says so
    // (server audio accepted since the previous stop and not yet written). Returns the packets
    // cleared (all of them, local sounds included: dropped_ms, contract §2.1).
    virtual uint32_t Stop(uint32_t serial) = 0;
    // Empty the queues: the start from Idle (R2.5) and ClearForListening (§2.4).
    virtual void Clear() = 0;
};

struct GatePorts {
    AudioSink* audio = nullptr;
    net::SendQueue* audio_queue = nullptr;
    net::SendQueue* ctrl_queue = nullptr;
    std::function<int64_t()> now_us;
    // The UI list (never waits)
    std::function<void(uint64_t e, bool spk, uint32_t rev)> post_gate_changed;
    std::function<void(uint64_t e, bool spk, uint32_t rev)> post_link_up;
    // The link manager's queue (never waits). Posted once per pair: the core marks it dead.
    std::function<void(uint64_t e, link::EndReason reason, bool flush)> end_pair;
};

// The gate's answer to a touch / wake word / Toggle; the UiController shell maps it to its
// TouchReply 1:1 (plan 1 handoff 13).
enum class TouchOutcome {
    kUnbound,      // no pair, another pair, or a dead one
    kNotSpeaking,  // the gate does not play: rev tells what the UI now knows
    kR5,           // stopped; the device abort and the listen start are queued together
    kSendFailed,   // stopped, but the pair could not be queued: the pair ends (F2's kind)
};
struct TouchResult {
    TouchOutcome outcome = TouchOutcome::kUnbound;
    uint32_t rev = 0;
};

// The gate's counts for stat (contract §5.1, design §2.2, §6.2; plan 2B-2a)
struct GateStats {
    uint32_t stops = 0;              // the stop processing ran (any trigger)
    uint32_t stop_abort = 0;         // R1.3
    uint32_t stop_tts_start = 0;     // R2.4: a tts start with a newer aborted_gen
    uint32_t stop_touch = 0;         // R5
    uint32_t stop_violation = 0;     // R1.2, R2.2, R2.2b (the pair ends)
    uint32_t stop_f1 = 0;            // StopForDeath(F1)
    uint32_t stop_f2 = 0;            // StopForDeath(F2)
    uint32_t stop_closed = 0;        // StopForDeath: a link closed (either side, or the gateway's close)
    uint32_t stop_other_death = 0;   // StopForDeath: a full queue, a hello / S6 deadline, ...
    uint32_t stop_unbind = 0;        // K2
    uint32_t already = 0;            // R1.1
    uint32_t rejected_start = 0;     // R2.1, R2.3, R2.6
    uint32_t ignored_stop = 0;       // R3: not the current generation
    uint32_t v_abort = 0;            // R1.2: aborted_gen < g < current_gen
    uint32_t v_k_ahead = 0;          // R2.2: K above dev_abort_seq
    uint32_t v_k_lowered = 0;        // R2.2b: K below the largest K seen
    uint32_t dropped_server = 0;     // server audio the gate refused (R4) or the queue was full for
    uint32_t done_send_failed = 0;   // a done that did not fit the control queue
    uint32_t violations = 0;         // the pair ended by a contract violation
    uint32_t rejected_session = 0;   // an abort of another (or no) session: no stop, no done
    uint32_t stale = 0;              // an entry for another pair, before any pair or a dead one (§2.2)
};

class PlaybackGate {
public:
    explicit PlaybackGate(GatePorts ports);

    // S4.2: the manager binds the pair once the control hello reply came (session_id of the
    // audio hello reply, the same for both links: contract S8).
    Outcome Bind(uint64_t e, const std::string& session_id);
    // K2: the manager ends the pair
    Outcome Unbind(uint64_t e);
    Outcome OnTtsStart(uint64_t e, const wire::TtsStart& t);
    Outcome OnTtsStop(uint64_t e, uint32_t gen);
    // R1: the done goes to the control queue; a violation ends the pair after the done. An
    // abort whose session_id is not the pair's (or is missing) does nothing (contract R1, S8)
    Outcome OnAbort(uint64_t e, const wire::AbortRequest& req);
    // R4: `push` runs under the lock only when the gate takes the packet; false: the decode
    // queue was full (dropped and counted here, never by the book)
    Outcome OnServerAudio(uint64_t e, const std::function<bool()>& push);
    // R5 for a touch (ManualStop) or a wake word (the default mode, reason wake_word_detected)
    TouchResult OnTouch(uint64_t e, wire::ListenMode mode, wire::DeviceAbortReason reason);
    // F1 / F2 / a full queue (design §2.2): stop, dead, close both queues, end the pair once
    void StopForDeath(uint64_t e, link::EndReason reason);
    // §2.4: empty the queues only while the gate does not play
    void ClearForListening();
    // Plan 1 handoff 12: LinkUp is made under the gate lock, only for the bound, live pair
    bool PostLinkUp(uint64_t e);

    State Snapshot() const;
    GateStats Stats() const;
    std::string session_id() const;

private:
    void ApplyLocked(uint64_t e, const State& before, const Result& r, uint32_t* cleared);
    void CountLocked(const Result& r);
    void StopForDeathLocked(uint64_t e, link::EndReason reason);
    void EndViolationLocked(uint64_t e, bool flush);
    static std::string Take(cJSON* root);

    GatePorts p_;
    mutable std::mutex mutex_;
    State s_;
    std::string session_;
    GateStats stats_;
};

}  // namespace stackchan::gate
