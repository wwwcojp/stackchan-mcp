// StackChan FW-A2: the playback gate core (design 2026-10-07-fw-a2-design.md §2.1-2.3).
// Pure functions over a value State. No ESP-IDF, no locks, no queues: the shell
// (PlaybackGate) takes gate_mutex_, calls these, and applies the result.
// Contract rules: K151-DeviceLink contract §2.2 (R1-R5), S4.2, K2, plus R2.2b.
#pragma once

#include <cstdint>

namespace stackchan::gate {

struct State {
    uint64_t bound_e = 0;       // pair epoch E this gate is bound to (0 = none)
    uint64_t last_ended_e = 0;  // last pair ended by Unbind (Bind needs E > this)
    bool dead = false;          // the pair is being ended; every entry but Unbind is ignored
    uint32_t current_gen = 0;
    uint32_t aborted_gen = 0;
    uint32_t dev_abort_seq = 0;
    uint32_t max_seen_k = 0;  // highest dev_abort_seen seen in this pair (R2.2b)
    bool accepting = false;
    bool speaking = false;
    uint32_t stop_serial = 0;  // +1 on every stop; never reset (played_after_stop)
    uint32_t rev = 0;          // +1 whenever speaking changes; never reset (GateChanged)
};

enum class Outcome {
    kBound,
    kAccepted,
    kRejected,
    kStopped,
    kAlready,
    kEnded,  // contract violation: stopped and dead; the shell ends the pair
    kNormalStop,
    kIgnored,
    kQueued,
    kDropped,
    kTouched,
    kReset,
    kStale,  // wrong E, not bound, or dead: nothing changed
};

enum class SentKind { kNone, kDone, kDeviceAbort };

struct Sent {
    SentKind kind = SentKind::kNone;
    uint32_t gen = 0;
    bool already = false;        // kDone: result "already" (else "stopped")
    uint32_t dev_abort_seq = 0;  // kDeviceAbort
};

struct Result {
    State state;
    Outcome outcome = Outcome::kIgnored;
    const char* rule = "";       // contract rule branch, e.g. "R2.5" (last one when several)
    bool stopped = false;        // the stop processing ran (close, not speaking, stop_serial+1)
    bool start_from_idle = false;  // R2.5 turned speaking false->true: clear the queue first
    Sent sent;
};

// S4.2 / K2 (design §2.2 "組の切り替え")
Result Bind(const State& s, uint64_t e);
Result Unbind(const State& s, uint64_t e);
// R2 (+R2.2b), R3, R1, R4 on the given pair
Result OnTtsStart(const State& s, uint64_t e, uint32_t gen, uint32_t aborted_gen,
                  uint32_t dev_abort_seen);
Result OnTtsStop(const State& s, uint64_t e, uint32_t gen);
Result OnAbort(const State& s, uint64_t e, uint32_t gen);
Result OnServerAudio(const State& s, uint64_t e);
// R5 (touch / wake word while speaking). Not speaking: kIgnored. Wrong pair: kStale.
Result OnTouch(const State& s, uint64_t e);
// F1/F2/full queue: stop and mark dead (the shell ends the pair)
Result StopForDeath(const State& s, uint64_t e);
// ClearForListening: clear the queue only while not speaking (design §2.4)
bool ShouldClearForListening(const State& s);

const char* OutcomeName(Outcome o);

}  // namespace stackchan::gate
