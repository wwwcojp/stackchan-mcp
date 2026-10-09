// StackChan FW-A2: the playback gate core. Mirrors tests/device_model.py of the
// stackchan-works repo (contract §2.2 + R2.2b) and adds pair epochs, dead, rev, stop_serial.
#include "playback_gate_core.h"

#include <algorithm>

namespace stackchan::gate {

namespace {

// The stop processing (contract "止める処理"): close, not speaking, stop_serial+1.
State Stop(State s) {
    s.accepting = false;
    if (s.speaking) {
        s.speaking = false;
        s.rev++;
    }
    s.stop_serial++;
    return s;
}

// Reset the per-pair fields (K2). bound_e, last_ended_e, stop_serial and rev are kept by the caller.
State ResetPair(State s) {
    s.current_gen = 0;
    s.aborted_gen = 0;
    s.dev_abort_seq = 0;
    s.max_seen_k = 0;
    s.accepting = false;
    s.speaking = false;
    s.dead = false;
    return s;
}

Result Make(const State& s, Outcome o, const char* rule, bool stopped = false) {
    Result r;
    r.state = s;
    r.outcome = o;
    r.rule = rule;
    r.stopped = stopped;
    return r;
}

Result Stale(const State& s) { return Make(s, Outcome::kStale, "stale"); }

// The entry guard. Returns true when the call is for a pair this gate does not serve now.
// e == 0 with no bound pair is the contract's "not bound yet" (handled by each entry).
bool IsStale(const State& s, uint64_t e) {
    if (s.dead) return true;
    if (s.bound_e == 0) return e != 0;
    return e != s.bound_e;
}

// The stop processing of a pair that ends sets aborted_gen = max(aborted_gen, current_gen)
// (contract §4 "死んだとみなしたとき", R1.2; plan 1 follow-up 1): stat reads it before the Unbind.
Result Ended(const State& s, const char* rule) {
    State d = Stop(s);
    d.aborted_gen = std::max(s.aborted_gen, s.current_gen);
    d.dead = true;
    return Make(d, Outcome::kEnded, rule, true);
}

Sent Done(uint32_t gen, bool already) {
    Sent m;
    m.kind = SentKind::kDone;
    m.gen = gen;
    m.already = already;
    return m;
}

}  // namespace

Result Bind(const State& s, uint64_t e) {
    if (s.dead) return Stale(s);
    if (s.bound_e != 0) return Make(s, Outcome::kIgnored, "S4.2");
    if (e == 0 || e <= s.last_ended_e) return Stale(s);
    State b = ResetPair(s);
    b.bound_e = e;
    return Make(b, Outcome::kBound, "S4.2");
}

Result Unbind(const State& s, uint64_t e) {
    if (e != s.bound_e) return Stale(s);
    State u = ResetPair(Stop(s));
    if (e != 0) u.last_ended_e = std::max(u.last_ended_e, e);
    u.bound_e = 0;
    return Make(u, Outcome::kReset, "K2", true);
}

Result OnTtsStart(const State& s, uint64_t e, uint32_t gen, uint32_t aborted_gen,
                  uint32_t dev_abort_seen) {
    if (IsStale(s, e)) return Stale(s);
    if (s.bound_e == 0) return Make(s, Outcome::kRejected, "R2.1");
    if (dev_abort_seen > s.dev_abort_seq) return Ended(s, "R2.2");
    if (dev_abort_seen < s.max_seen_k) return Ended(s, "R2.2b");
    State d = s;
    d.max_seen_k = dev_abort_seen;
    if (dev_abort_seen < d.dev_abort_seq) {
        d.aborted_gen = std::max(d.aborted_gen, aborted_gen);
        return Make(d, Outcome::kRejected, "R2.3");
    }
    bool stopped = false;
    if (aborted_gen > d.aborted_gen) {
        d = Stop(d);
        d.aborted_gen = aborted_gen;
        stopped = true;
    }
    if (gen > d.aborted_gen && gen >= d.current_gen) {
        Result r = Make(d, Outcome::kAccepted, "R2.5", stopped);
        r.start_from_idle = !d.speaking;
        r.state.current_gen = gen;
        r.state.accepting = true;
        if (!r.state.speaking) {
            r.state.speaking = true;
            r.state.rev++;
        }
        return r;
    }
    return Make(d, Outcome::kRejected, "R2.6", stopped);
}

Result OnTtsStop(const State& s, uint64_t e, uint32_t gen) {
    if (IsStale(s, e)) return Stale(s);
    if (gen == s.current_gen && gen > s.aborted_gen) {
        State d = s;
        if (d.speaking) {
            d.speaking = false;
            d.rev++;
        }
        return Make(d, Outcome::kNormalStop, "R3");
    }
    return Make(s, Outcome::kIgnored, "R3.ignore");
}

Result OnAbort(const State& s, uint64_t e, uint32_t gen) {
    if (IsStale(s, e)) return Stale(s);
    if (s.bound_e == 0) return Make(s, Outcome::kIgnored, "R1.unbound");
    if (gen <= s.aborted_gen) {
        Result r = Make(s, Outcome::kAlready, "R1.1");
        r.sent = Done(gen, true);
        return r;
    }
    if (gen < s.current_gen) {
        Result r = Ended(s, "R1.2");
        r.sent = Done(gen, false);
        return r;
    }
    State d = Stop(s);
    d.aborted_gen = gen;
    Result r = Make(d, Outcome::kStopped, "R1.3", true);
    r.sent = Done(gen, false);
    return r;
}

Result OnServerAudio(const State& s, uint64_t e) {
    if (IsStale(s, e)) return Stale(s);
    if (s.accepting && s.speaking) return Make(s, Outcome::kQueued, "R4.queue");
    return Make(s, Outcome::kDropped, "R4.drop");
}

Result OnTouch(const State& s, uint64_t e) {
    if (IsStale(s, e)) return Stale(s);
    if (!s.speaking) return Make(s, Outcome::kIgnored, "R5.ignore");
    State d = Stop(s);
    d.dev_abort_seq = s.dev_abort_seq + 1;
    d.aborted_gen = std::max(s.aborted_gen, s.current_gen);
    Result r = Make(d, Outcome::kTouched, "R5", true);
    r.sent.kind = SentKind::kDeviceAbort;
    r.sent.gen = s.current_gen;
    r.sent.dev_abort_seq = d.dev_abort_seq;
    return r;
}

Result StopForDeath(const State& s, uint64_t e) {
    if (s.bound_e == 0 || IsStale(s, e)) return Stale(s);
    State d = Stop(s);
    d.aborted_gen = std::max(s.aborted_gen, s.current_gen);
    d.dead = true;
    return Make(d, Outcome::kStopped, "F", true);
}

bool ShouldClearForListening(const State& s) { return !s.speaking; }

const char* OutcomeName(Outcome o) {
    switch (o) {
        case Outcome::kBound: return "bound";
        case Outcome::kAccepted: return "accepted";
        case Outcome::kRejected: return "rejected";
        case Outcome::kStopped: return "stopped";
        case Outcome::kAlready: return "already";
        case Outcome::kEnded: return "ended";
        case Outcome::kNormalStop: return "normal_stop";
        case Outcome::kIgnored: return "ignored";
        case Outcome::kQueued: return "queued";
        case Outcome::kDropped: return "dropped";
        case Outcome::kTouched: return "touched";
        case Outcome::kReset: return "reset";
        case Outcome::kStale: return "stale";
    }
    return "?";
}

}  // namespace stackchan::gate
