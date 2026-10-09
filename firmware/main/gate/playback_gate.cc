// StackChan FW-A2 §2.1-2.2: the playback gate shell (see playback_gate.h).
#include "playback_gate.h"

#include <cJSON.h>

#include <cstring>

namespace stackchan::gate {

namespace {
constexpr uint32_t kFrameMs = 60;  // dropped_ms = cleared packets * frame duration (FW-A §2.1)
}

PlaybackGate::PlaybackGate(GatePorts ports) : p_(std::move(ports)) {}

std::string PlaybackGate::Take(cJSON* root) {
    char* s = cJSON_PrintUnformatted(root);
    std::string out = s != nullptr ? s : "";
    cJSON_free(s);
    cJSON_Delete(root);
    return out;
}

// Apply a core result under the lock: the GateChanged notice first, then the AudioService's stop
// / start. A clear can meet an AutoStop drain request and post PlaybackDrained; the UI must see the
// new speaking state before it (plan 2A follow-up 1, Claude review 146 Minor 1: otherwise a new
// playback's start opens the mic and plays the popup at its head).
void PlaybackGate::ApplyLocked(uint64_t e, const State& before, const Result& r, uint32_t* cleared) {
    s_ = r.state;
    if (s_.rev != before.rev) p_.post_gate_changed(e, s_.speaking, s_.rev);
    uint32_t n = 0;
    if (r.stopped) {
        n = p_.audio->Stop(r.state.stop_serial);
        stats_.stops++;
    }
    if (r.outcome == Outcome::kAccepted && r.start_from_idle) p_.audio->Clear();
    if (cleared != nullptr) *cleared = n;
}

// The counts by rule (stat). The stops by death are counted where the reason is known.
void PlaybackGate::CountLocked(const Result& r) {
    if (r.outcome == Outcome::kStale) {
        stats_.stale++;
        return;
    }
    const char* rule = r.rule != nullptr ? r.rule : "";
    if (std::strcmp(rule, "R1.1") == 0) stats_.already++;
    if (std::strcmp(rule, "R1.2") == 0) stats_.v_abort++;
    if (std::strcmp(rule, "R2.2") == 0) stats_.v_k_ahead++;
    if (std::strcmp(rule, "R2.2b") == 0) stats_.v_k_lowered++;
    if (std::strcmp(rule, "R3.ignore") == 0) stats_.ignored_stop++;
    if (r.outcome == Outcome::kRejected) stats_.rejected_start++;
    if (!r.stopped) return;
    if (r.outcome == Outcome::kEnded) {
        stats_.stop_violation++;
    } else if (std::strcmp(rule, "R1.3") == 0) {
        stats_.stop_abort++;
    } else if (std::strcmp(rule, "R5") == 0) {
        stats_.stop_touch++;
    } else if (std::strcmp(rule, "K2") == 0) {
        stats_.stop_unbind++;
    } else if (std::strncmp(rule, "R2.", 3) == 0) {
        stats_.stop_tts_start++;
    }
}

// A violation (R1.2, R2.2, R2.2b): the core stopped and marked the pair dead. Ask the manager to
// end the pair (once: every later entry of this pair is stale), then close the queues (keep the
// control one when the done is in it). The end request goes first: the control send task reports
// the flush only after the queue is closed for it, so that notice can never reach the manager
// before the end request (Claude review 146 Minor 2).
void PlaybackGate::EndViolationLocked(uint64_t e, bool flush) {
    stats_.violations++;
    p_.end_pair(e, link::EndReason::kViolation, flush);
    p_.audio_queue->Close();
    if (flush) {
        p_.ctrl_queue->CloseForFlush();
    } else {
        p_.ctrl_queue->Close();
    }
}

void PlaybackGate::StopForDeathLocked(uint64_t e, link::EndReason reason) {
    const State before = s_;
    const Result r = gate::StopForDeath(s_, e);
    if (r.outcome == Outcome::kStale) {  // not bound, another pair, or already dead
        stats_.stale++;
        return;
    }
    ApplyLocked(e, before, r, nullptr);
    switch (reason) {
        case link::EndReason::kF1: stats_.stop_f1++; break;
        case link::EndReason::kF2: stats_.stop_f2++; break;
        case link::EndReason::kAudioClosed:
        case link::EndReason::kCtrlClosed:
        case link::EndReason::kServerClose: stats_.stop_closed++; break;
        default: stats_.stop_other_death++; break;
    }
    p_.audio_queue->Close();
    p_.ctrl_queue->Close();
    p_.end_pair(e, reason, false);
}

Outcome PlaybackGate::Bind(uint64_t e, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Result r = gate::Bind(s_, e);
    if (r.outcome == Outcome::kStale) stats_.stale++;
    if (r.outcome == Outcome::kBound) {
        s_ = r.state;
        session_ = session_id;
    }
    return r.outcome;
}

Outcome PlaybackGate::Unbind(uint64_t e) {
    std::lock_guard<std::mutex> lock(mutex_);
    const State before = s_;
    const Result r = gate::Unbind(s_, e);
    CountLocked(r);
    if (r.outcome == Outcome::kStale) return r.outcome;
    ApplyLocked(e, before, r, nullptr);
    session_.clear();
    return r.outcome;
}

Outcome PlaybackGate::OnTtsStart(uint64_t e, const wire::TtsStart& t) {
    std::lock_guard<std::mutex> lock(mutex_);
    const State before = s_;
    const Result r = gate::OnTtsStart(s_, e, t.gen, t.aborted_gen, t.dev_abort_seen);
    CountLocked(r);
    if (r.outcome == Outcome::kStale) return r.outcome;
    ApplyLocked(e, before, r, nullptr);
    if (r.outcome == Outcome::kEnded) EndViolationLocked(e, false);
    return r.outcome;
}

Outcome PlaybackGate::OnTtsStop(uint64_t e, uint32_t gen) {
    std::lock_guard<std::mutex> lock(mutex_);
    const State before = s_;
    const Result r = gate::OnTtsStop(s_, e, gen);
    CountLocked(r);
    if (r.outcome == Outcome::kStale) return r.outcome;
    ApplyLocked(e, before, r, nullptr);
    return r.outcome;
}

Outcome PlaybackGate::OnAbort(uint64_t e, const wire::AbortRequest& req) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (s_.bound_e != 0 && req.session_id != session_) {  // Codex review 143 Important 2
        stats_.rejected_session++;
        return Outcome::kStale;
    }
    const State before = s_;
    const Result r = gate::OnAbort(s_, e, req.gen);
    CountLocked(r);
    if (r.outcome == Outcome::kStale) return r.outcome;
    uint32_t cleared = 0;
    ApplyLocked(e, before, r, &cleared);
    bool queued = false;
    if (r.sent.kind == SentKind::kDone) {
        // the core's gen (the request's) and result; req_id and reason are echoed (contract §2.1)
        wire::AbortRequest echo = req;
        echo.gen = r.sent.gen;
        const std::string done = Take(wire::BuildAbortDone(echo, r.sent.already, cleared * kFrameMs));
        queued = p_.ctrl_queue->Push(e, net::ElemKind::kJson, done, p_.now_us()) == net::PushResult::kQueued;
        if (!queued) stats_.done_send_failed++;
    }
    if (r.outcome == Outcome::kEnded) {
        EndViolationLocked(e, queued);  // flush the done first (contract R1.2: done, then end)
    } else if (r.sent.kind == SentKind::kDone && !queued) {
        StopForDeathLocked(e, link::EndReason::kQueueFull);  // §4.1: a JSON that does not fit
    }
    return r.outcome;
}

Outcome PlaybackGate::OnServerAudio(uint64_t e, const std::function<bool()>& push) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Result r = gate::OnServerAudio(s_, e);
    if (r.outcome == Outcome::kStale) stats_.stale++;
    if (r.outcome == Outcome::kQueued) {
        if (!push()) {
            stats_.dropped_server++;
            return Outcome::kDropped;
        }
    } else if (r.outcome == Outcome::kDropped) {
        stats_.dropped_server++;
    }
    return r.outcome;
}

TouchResult PlaybackGate::OnTouch(uint64_t e, wire::ListenMode mode, wire::DeviceAbortReason reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    TouchResult out;
    if (e == 0) {  // no pair the UI knows of (a gate without a pair says "ignored")
        stats_.stale++;
        return out;
    }
    const State before = s_;
    const Result r = gate::OnTouch(s_, e);
    CountLocked(r);
    out.rev = r.state.rev;
    if (r.outcome == Outcome::kStale) return out;
    if (r.outcome == Outcome::kIgnored) {
        out.outcome = TouchOutcome::kNotSpeaking;
        return out;
    }
    ApplyLocked(e, before, r, nullptr);
    out.rev = s_.rev;
    // R5: the stop already holds (F3); the abort and the listen start go together (§2.2)
    const std::string abort =
        Take(wire::BuildDeviceAbort(session_, r.sent.gen, r.sent.dev_abort_seq, reason));
    const std::string listen = Take(wire::BuildListenStart(session_, mode));
    if (p_.audio_queue->PushPair(e, abort, listen, p_.now_us()) == net::PushResult::kQueued) {
        out.outcome = TouchOutcome::kR5;
    } else {
        StopForDeathLocked(e, link::EndReason::kQueueFull);
        out.outcome = TouchOutcome::kSendFailed;
    }
    return out;
}

void PlaybackGate::StopForDeath(uint64_t e, link::EndReason reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    StopForDeathLocked(e, reason);
}

void PlaybackGate::ClearForListening() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ShouldClearForListening(s_)) p_.audio->Clear();
}

bool PlaybackGate::PostLinkUp(uint64_t e) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (e == 0 || s_.bound_e != e || s_.dead) {
        stats_.stale++;  // Codex review 157 Minor 1
        return false;
    }
    p_.post_link_up(e, s_.speaking, s_.rev);
    return true;
}

State PlaybackGate::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return s_;
}

GateStats PlaybackGate::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::string PlaybackGate::session_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_;
}

}  // namespace stackchan::gate
