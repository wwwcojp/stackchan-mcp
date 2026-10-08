// StackChan FW-A2 §2.1-2.2: the playback gate shell (see playback_gate.h).
#include "playback_gate.h"

#include <cJSON.h>

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

// Apply a core result under the lock: the AudioService's stop / start, then the notices.
void PlaybackGate::ApplyLocked(uint64_t e, const State& before, const Result& r, uint32_t* cleared) {
    uint32_t n = 0;
    if (r.stopped) {
        n = p_.audio->Stop(r.state.stop_serial);
        stats_.stops++;
    }
    if (r.outcome == Outcome::kAccepted && r.start_from_idle) p_.audio->Clear();
    s_ = r.state;
    if (s_.rev != before.rev) p_.post_gate_changed(e, s_.speaking, s_.rev);
    if (cleared != nullptr) *cleared = n;
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
    if (r.outcome == Outcome::kStale) return;  // not bound, another pair, or already dead
    ApplyLocked(e, before, r, nullptr);
    p_.audio_queue->Close();
    p_.ctrl_queue->Close();
    p_.end_pair(e, reason, false);
}

Outcome PlaybackGate::Bind(uint64_t e, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Result r = gate::Bind(s_, e);
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
    if (r.outcome == Outcome::kStale) return r.outcome;
    ApplyLocked(e, before, r, nullptr);
    session_.clear();
    return r.outcome;
}

Outcome PlaybackGate::OnTtsStart(uint64_t e, const wire::TtsStart& t) {
    std::lock_guard<std::mutex> lock(mutex_);
    const State before = s_;
    const Result r = gate::OnTtsStart(s_, e, t.gen, t.aborted_gen, t.dev_abort_seen);
    if (r.outcome == Outcome::kStale) return r.outcome;
    ApplyLocked(e, before, r, nullptr);
    if (r.outcome == Outcome::kEnded) EndViolationLocked(e, false);
    return r.outcome;
}

Outcome PlaybackGate::OnTtsStop(uint64_t e, uint32_t gen) {
    std::lock_guard<std::mutex> lock(mutex_);
    const State before = s_;
    const Result r = gate::OnTtsStop(s_, e, gen);
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
    if (e == 0) return out;  // no pair the UI knows of (a gate without a pair says "ignored")
    const State before = s_;
    const Result r = gate::OnTouch(s_, e);
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
    if (e == 0 || s_.bound_e != e || s_.dead) return false;
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
