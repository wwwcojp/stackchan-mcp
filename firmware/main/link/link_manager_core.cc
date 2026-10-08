// StackChan FW-A2 §3: the link manager's transition function.
#include "link_manager_core.h"

#include <algorithm>

namespace stackchan::link {

namespace {

using Outs = std::vector<Output>;

Output Out(OutKind k, uint64_t e = 0, uint32_t attempt = 0) {
    Output o{k};
    o.e = e;
    o.attempt = attempt;
    return o;
}

bool Building(Stage st) {
    return st == Stage::kAudioConnect || st == Stage::kAudioHello || st == Stage::kAudioOnly ||
           st == Stage::kCtrlConnect || st == Stage::kCtrlHello || st == Stage::kReadySend ||
           st == Stage::kBound;
}

State ToWaiting(State s, int64_t now) {
    s.stage = s.shutdown ? Stage::kStopped : Stage::kWaiting;
    s.retry_at_us = now + static_cast<int64_t>(s.backoff_ms) * 1000;
    s.backoff_ms = std::min(s.backoff_ms * 2, kBackoffMaxMs);
    s.deadline_us = 0;
    s.started = 0;
    s.exited = 0;
    s.pending_exits = 0;
    s.flushing = false;
    s.flush_ctrl = false;
    s.ctrl_link_up = false;
    return s;
}

// §3.4 step 2: ask both links to stop, then wait for the task exits (3 s)
State RequestStop(State s, int64_t now, Outs* out) {
    s.flushing = false;
    out->push_back(Out(OutKind::kRequestStop, s.e));
    s.deadline_us = now + kExitWaitUs;
    return s;
}

// End the pair once (K1). §3.4 step 1, then flush (violation) or stop.
State EndPair(State s, EndReason reason, bool flush, int64_t now, Outs* out) {
    const bool was_bound = s.stage == Stage::kBound;
    Output stop = Out(OutKind::kStopForDeath, s.e);
    stop.reason = reason;
    out->push_back(stop);
    out->push_back(Out(OutKind::kUnbindGate, s.e));
    Output close = Out(OutKind::kCloseQueues, s.e);
    close.keep_ctrl = flush && s.ctrl_link_up;
    out->push_back(close);
    if (was_bound) out->push_back(Out(OutKind::kPostLinkDown, s.e));
    s.stage = Stage::kEnding;
    s.reason = reason;
    s.ended_pairs++;
    // only the tasks that run: an exit noticed before the end is not waited for again
    s.pending_exits = s.started & ~s.exited;
    s.flush_ctrl = close.keep_ctrl;
    if (s.pending_exits == 0) {  // nothing runs: nothing to flush or stop
        out->push_back(Out(OutKind::kDestroy, s.e));
        return ToWaiting(s, now);
    }
    if (s.flush_ctrl) {
        s.flushing = true;
        s.deadline_us = now + kFlushUs;
        return s;
    }
    return RequestStop(s, now, out);
}

StepResult Stale(State s) {
    s.stale_inputs++;
    return {s, {}};
}

}  // namespace

StepResult Step(const State& s0, const Input& in) {
    StepResult r{s0, {}};
    State& s = r.state;
    Outs* out = &r.out;
    switch (in.kind) {
        case InKind::kTick: {
            const int64_t now = in.now_us;
            switch (s.stage) {
                case Stage::kWaiting:
                    if (!s.shutdown && now >= s.retry_at_us) {
                        s.attempt++;
                        s.stage = Stage::kAudioConnect;
                        s.e = 0;
                        s.started = kAudioWorker;  // exited was cleared by ToWaiting
                        s.reason = EndReason::kNone;
                        out->push_back(Out(OutKind::kConnectAudio, 0, s.attempt));
                    }
                    break;
                case Stage::kAudioHello:
                    if (now >= s.deadline_us) s = EndPair(s, EndReason::kHelloTimeout, false, now, out);
                    break;
                case Stage::kCtrlConnect:
                case Stage::kCtrlHello:
                    if (now >= s.deadline_us) s = EndPair(s, EndReason::kS6, false, now, out);
                    break;
                case Stage::kBound:
                    if (now - in.ctrl_last_rx_us >= kF1Us) s = EndPair(s, EndReason::kF1, false, now, out);
                    break;
                case Stage::kEnding:
                    if (s.flushing && now >= s.deadline_us) {
                        s = RequestStop(s, now, out);
                    } else if (!s.flushing && now >= s.deadline_us) {
                        out->push_back(Out(OutKind::kRestart, s.e));
                    }
                    break;
                default:
                    break;
            }
            return r;
        }
        case InKind::kConnectResult: {
            if (in.attempt != s.attempt) return Stale(s);
            // A result of this attempt's worker after the end (S6, Shutdown, ...). The worker
            // reports before it exits, so the attempt is still ending; a link it made already runs
            // its tasks: count them, stop them too, and wait for them (final reviews 135-137).
            if (s.stage == Stage::kEnding) {
                if (in.ok) {
                    s.started |= in.which == kAudioRx ? (kAudioRx | kAudioTx) : (kCtrlRx | kCtrlTx);
                    if (in.which == kAudioRx && s.e == 0) s.e = in.e;  // ended before its epoch
                    if (in.which == kCtrlRx) s.ctrl_link_up = true;
                    s.pending_exits = s.started & ~s.exited;
                    if (s.pending_exits != 0 && !s.flushing) out->push_back(Out(OutKind::kRequestStop, s.e));
                }
                return r;
            }
            if (in.which == kAudioRx && s.stage == Stage::kAudioConnect) {
                if (!in.ok) return {ToWaiting(s, in.now_us), {}};
                s.started |= kAudioRx | kAudioTx;
                s.e = in.e;
                s.stage = Stage::kAudioHello;
                s.deadline_us = in.now_us + kHelloTimeoutUs;
                out->push_back(Out(OutKind::kSendAudioHello, s.e, s.attempt));
                return r;
            }
            if (in.which == kCtrlRx && s.stage == Stage::kCtrlConnect) {
                if (!in.ok) {
                    s = EndPair(s, EndReason::kConnectFailed, false, in.now_us, out);
                    return r;
                }
                s.started |= kCtrlRx | kCtrlTx;
                s.ctrl_link_up = true;
                s.stage = Stage::kCtrlHello;
                out->push_back(Out(OutKind::kSendCtrlHello, s.e));
                return r;
            }
            return Stale(s);
        }
        case InKind::kAudioHelloReply: {
            if (in.attempt != s.attempt || in.e != s.e || s.stage != Stage::kAudioHello) return Stale(s);
            if (!in.ctrl_offered) {
                s.stage = Stage::kAudioOnly;
                s.deadline_us = 0;
                return r;
            }
            s.audio_hello_reply_us = in.at_us;
            s.deadline_us = in.at_us + kS6Us;
            s.stage = Stage::kCtrlConnect;
            s.started |= kCtrlWorker;
            out->push_back(Out(OutKind::kConnectCtrl, s.e, s.attempt));
            return r;
        }
        case InKind::kCtrlHelloReply: {
            if (s.stage != Stage::kCtrlHello) return Stale(s);
            if (in.e != s.e) {
                s = EndPair(s, EndReason::kViolation, false, in.now_us, out);
                return r;
            }
            s.deadline_us = 0;  // S6 met
            s.stage = Stage::kReadySend;
            out->push_back(Out(OutKind::kBindGate, s.e));
            out->push_back(Out(OutKind::kSendReady, s.e));
            return r;
        }
        case InKind::kReadySent: {
            if (in.e != s.e || s.stage != Stage::kReadySend) return Stale(s);
            s.stage = Stage::kBound;
            s.backoff_ms = 1000;
            out->push_back(Out(OutKind::kPostLinkUp, s.e));
            return r;
        }
        case InKind::kEndRequest: {
            if (in.e != s.e || s.e == 0) return Stale(s);
            if (!Building(s.stage)) {
                s.duplicate_ends++;
                return r;
            }
            s = EndPair(s, in.reason, in.flush, in.now_us, out);
            return r;
        }
        case InKind::kCtrlFlushed: {
            if (in.e != s.e || s.stage != Stage::kEnding || !s.flushing) return Stale(s);
            s = RequestStop(s, in.now_us, out);
            return r;
        }
        case InKind::kTaskExited: {
            // by the attempt: an attempt may end before it knows its epoch. Remembered in any
            // stage and order (before the EndRequest, before the late connect result).
            if (in.attempt != s.attempt || !(Building(s.stage) || s.stage == Stage::kEnding)) return Stale(s);
            s.exited |= in.which;
            if (s.stage != Stage::kEnding) return r;  // the EndRequest follows
            s.pending_exits = s.started & ~s.exited;
            // all gone: nothing is left to flush either (the control send task exited)
            if (s.pending_exits == 0) {
                out->push_back(Out(OutKind::kDestroy, s.e));
                s = ToWaiting(s, in.now_us);
            }
            return r;
        }
        case InKind::kShutdown: {
            s.shutdown = true;
            if (Building(s.stage)) {
                s = EndPair(s, EndReason::kShutdown, false, in.now_us, out);
            } else if (s.stage == Stage::kWaiting) {
                s.stage = Stage::kStopped;
            }
            return r;
        }
    }
    return r;
}

}  // namespace stackchan::link
