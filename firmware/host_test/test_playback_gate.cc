// StackChan FW-A2 §2.1-2.2, plan 1 handoff 12-13: the playback gate shell. The contract vectors
// run through the shell (the done / device abort it queues must match "sent"), then what only
// the shell does: queue failures end the pair, the R5 pair, LinkUp under the lock, notices.
#include <gtest/gtest.h>

#include <cJSON.h>

#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "playback_gate.h"

namespace g = stackchan::gate;
namespace n = stackchan::net;
namespace w = stackchan::wire;
using stackchan::link::EndReason;

namespace {

struct FakeAudio : g::AudioSink {
    uint32_t stops = 0, clears = 0, last_serial = 0, queued = 3;
    uint32_t Stop(uint32_t serial) override {
        stops++;
        last_serial = serial;
        return queued;
    }
    void Clear() override { clears++; }
};

struct End {
    uint64_t e;
    EndReason reason;
    bool flush;
};
struct Notice {
    uint64_t e;
    bool spk;
    uint32_t rev;
};

struct Rig {
    FakeAudio audio;
    n::SendQueue audio_q{n::kAudioLimits};
    n::SendQueue ctrl_q{n::kCtrlLimits};
    std::vector<End> ends;
    std::vector<Notice> changed, link_ups;
    std::unique_ptr<g::PlaybackGate> gate;
    Rig() {
        g::GatePorts p;
        p.audio = &audio;
        p.audio_queue = &audio_q;
        p.ctrl_queue = &ctrl_q;
        p.now_us = [] { return int64_t{0}; };
        p.post_gate_changed = [this](uint64_t e, bool spk, uint32_t rev) { changed.push_back({e, spk, rev}); };
        p.post_link_up = [this](uint64_t e, bool spk, uint32_t rev) { link_ups.push_back({e, spk, rev}); };
        p.end_pair = [this](uint64_t e, EndReason r, bool flush) { ends.push_back({e, r, flush}); };
        gate = std::make_unique<g::PlaybackGate>(p);
    }
    void Bind(uint64_t e) {
        audio_q.Open(e);
        ctrl_q.Open(e);
        ASSERT_EQ(gate->Bind(e, "sid"), g::Outcome::kBound);
    }
};

std::vector<std::string> Drain(n::SendQueue& q) {
    std::vector<std::string> out;
    while (auto x = q.Pop(0)) out.push_back(x->payload);
    return out;
}

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json Parse(const std::string& s) { return Json(cJSON_Parse(s.c_str()), &cJSON_Delete); }

// key -> printed value, without the keys the vectors do not compare (README "sent")
std::map<std::string, std::string> Canon(const cJSON* m) {
    static const std::set<std::string> kDrop = {"req_id", "session_id", "fw_epoch", "seq", "dropped_ms",
                                                "reason", "diag"};
    std::map<std::string, std::string> out;
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, m) {
        if (kDrop.count(it->string)) continue;
        char* p = cJSON_PrintUnformatted(it);
        out[it->string] = p;
        cJSON_free(p);
    }
    return out;
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

uint32_t U(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? static_cast<uint32_t>(v->valuedouble) : 0;
}

}  // namespace

TEST(PlaybackGateShell, ContractVectorsThroughTheShell) {
    std::set<std::tuple<std::string, std::string, int>> manifest, ran;
    {
        std::istringstream in(ReadFile(std::string(CONTRACT_VECTORS_DIR) + "/MANIFEST"));
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            const auto t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
            manifest.insert({line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1), std::stoi(line.substr(t2 + 1))});
        }
    }
    ASSERT_FALSE(manifest.empty());
    std::set<std::string> files;
    for (const auto& k : manifest) files.insert(std::get<0>(k));
    for (const auto& file : files) {
        Json doc = Parse(ReadFile(std::string(CONTRACT_VECTORS_DIR) + "/" + file));
        ASSERT_NE(doc.get(), nullptr) << file;
        const cJSON* c = nullptr;
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(doc.get(), "cases")) {
            const std::string cname = cJSON_GetObjectItemCaseSensitive(c, "name")->valuestring;
            Rig rig;
            uint64_t e = 0, last = 0;
            int idx = 0;
            const cJSON* step = nullptr;
            cJSON_ArrayForEach(step, cJSON_GetObjectItemCaseSensitive(c, "steps")) {
                SCOPED_TRACE(file + " / " + cname + " / step " + std::to_string(idx++));
                const std::string ev = cJSON_GetObjectItemCaseSensitive(step, "event")->valuestring;
                const cJSON* exp = cJSON_GetObjectItemCaseSensitive(step, "expect");
                const uint32_t stops0 = rig.audio.stops;
                std::string outcome;
                if (ev == "bind") {
                    if (e == 0) {
                        e = last + 1;
                        rig.audio_q.Open(e);
                        rig.ctrl_q.Open(e);
                    }
                    outcome = g::OutcomeName(rig.gate->Bind(e, "sid"));
                } else if (ev == "tts_start") {
                    w::TtsStart t{U(step, "gen"), U(step, "aborted_gen"), U(step, "dev_abort_seen")};
                    outcome = g::OutcomeName(rig.gate->OnTtsStart(e, t));
                } else if (ev == "tts_stop") {
                    outcome = g::OutcomeName(rig.gate->OnTtsStop(e, U(step, "gen")));
                } else if (ev == "abort") {
                    w::AbortRequest req;
                    req.gen = U(step, "gen");
                    req.req_id = "r";
                    req.reason = "user";
                    req.session_id = "sid";
                    outcome = g::OutcomeName(rig.gate->OnAbort(e, req));
                } else if (ev == "server_audio") {
                    outcome = g::OutcomeName(rig.gate->OnServerAudio(e, [] { return true; }));
                } else if (ev == "touch") {
                    const g::TouchResult t = rig.gate->OnTouch(e, w::ListenMode::kManualStop, w::DeviceAbortReason::kTouch);
                    outcome = t.outcome == g::TouchOutcome::kR5 ? "touched" : "ignored";
                } else if (ev == "pair_end") {
                    outcome = g::OutcomeName(rig.gate->Unbind(e));
                } else {
                    FAIL() << "unknown event " << ev;
                }
                // what the shell queued in this step (the listen start after R5 is not "sent")
                std::vector<std::map<std::string, std::string>> sent;
                for (auto* q : {&rig.ctrl_q, &rig.audio_q}) {
                    for (const std::string& s : Drain(*q)) {
                        Json m = Parse(s);
                        ASSERT_NE(m.get(), nullptr);
                        if (w::TypeOf(m.get()) == "listen") continue;
                        sent.push_back(Canon(m.get()));
                    }
                }
                std::vector<std::map<std::string, std::string>> want;
                const cJSON* x = nullptr;
                cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(exp, "sent")) want.push_back(Canon(x));
                EXPECT_EQ(sent, want);
                EXPECT_EQ(outcome, cJSON_GetObjectItemCaseSensitive(exp, "outcome")->valuestring);
                EXPECT_EQ(rig.audio.stops > stops0, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(exp, "stopped")));
                if (outcome == "ended") {  // the manager ends the pair (K1): Unbind
                    ASSERT_EQ(rig.ends.size(), 1u);
                    EXPECT_EQ(rig.ends.back().reason, EndReason::kViolation);
                    rig.gate->Unbind(e);
                    rig.ends.clear();
                }
                if (outcome == "ended" || ev == "pair_end") {
                    last = e;
                    e = 0;
                }
                const g::State s = rig.gate->Snapshot();
                const cJSON* st = cJSON_GetObjectItemCaseSensitive(exp, "state");
                EXPECT_EQ(s.bound_e != 0, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "bound")));
                EXPECT_EQ(s.current_gen, U(st, "current_gen"));
                EXPECT_EQ(s.aborted_gen, U(st, "aborted_gen"));
                EXPECT_EQ(s.dev_abort_seq, U(st, "dev_abort_seq"));
                EXPECT_EQ(s.accepting, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "accepting")));
                EXPECT_EQ(s.speaking, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "speaking")));
                if (::testing::Test::HasFailure()) return;
            }
            ran.insert({file, cname, idx});
        }
    }
    EXPECT_EQ(ran, manifest);
}

TEST(PlaybackGateShell, DoneCarriesTheRequestAndTheClearedAudio) {
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {1, 0, 0});
    w::AbortRequest req;
    req.gen = 1;
    req.req_id = "q-9";
    req.session_id = "sid";
    req.reason = "hush";
    EXPECT_EQ(rig.gate->OnAbort(7, req), g::Outcome::kStopped);
    const auto done = Drain(rig.ctrl_q);
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done[0],
              R"({"type":"abort","state":"done","req_id":"q-9","reason":"hush","gen":1,"result":"stopped","dropped_ms":180})");
    EXPECT_EQ(rig.audio.last_serial, rig.gate->Snapshot().stop_serial);  // the sink got the gate's serial
}

TEST(PlaybackGateShell, ADoneThatDoesNotFitEndsThePair) {
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {1, 0, 0});
    for (size_t i = 0; i < n::kCtrlLimits.max_items; i++) rig.ctrl_q.Push(7, n::ElemKind::kJson, "x", 0);
    w::AbortRequest req;
    req.gen = 1;
    req.req_id = "q";
    req.session_id = "sid";
    rig.gate->OnAbort(7, req);
    ASSERT_EQ(rig.ends.size(), 1u);
    EXPECT_EQ(rig.ends[0].reason, EndReason::kQueueFull);
    EXPECT_FALSE(rig.ends[0].flush);
    EXPECT_TRUE(rig.gate->Snapshot().dead);
    EXPECT_EQ(rig.audio_q.Push(7, n::ElemKind::kJson, "y", 0), n::PushResult::kClosed);
    EXPECT_EQ(rig.gate->Stats().done_send_failed, 1u);
}

TEST(PlaybackGateShell, AViolationAbortFlushesItsDoneThenEndsThePair) {
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {3, 0, 0});
    w::AbortRequest req;
    req.gen = 2;  // aborted_gen < g < current_gen: R1.2
    req.req_id = "q";
    req.session_id = "sid";
    EXPECT_EQ(rig.gate->OnAbort(7, req), g::Outcome::kEnded);
    ASSERT_EQ(rig.ends.size(), 1u);
    EXPECT_EQ(rig.ends[0].reason, EndReason::kViolation);
    EXPECT_TRUE(rig.ends[0].flush);
    EXPECT_FALSE(rig.ctrl_q.Drained());  // the done waits to be flushed
    EXPECT_EQ(rig.ctrl_q.Push(7, n::ElemKind::kJson, "late", 0), n::PushResult::kClosed);
    EXPECT_EQ(rig.audio_q.Push(7, n::ElemKind::kJson, "late", 0), n::PushResult::kClosed);
    EXPECT_EQ(Drain(rig.ctrl_q).size(), 1u);
    // the pair is dead: a second end never comes from this gate
    rig.gate->StopForDeath(7, EndReason::kF1);
    EXPECT_EQ(rig.ends.size(), 1u);
}

TEST(PlaybackGateShell, ATtsStartViolationClosesBothQueuesWithoutFlush) {
    Rig rig;
    rig.Bind(7);
    EXPECT_EQ(rig.gate->OnTtsStart(7, {1, 0, 1}), g::Outcome::kEnded);  // K > dev_abort_seq: R2.2
    ASSERT_EQ(rig.ends.size(), 1u);
    EXPECT_FALSE(rig.ends[0].flush);
    EXPECT_TRUE(rig.ctrl_q.Drained());
}

TEST(PlaybackGateShell, R5QueuesTheAbortThenTheListenStartTogether) {
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {2, 0, 0});
    const g::TouchResult t = rig.gate->OnTouch(7, w::ListenMode::kAutoStop, w::DeviceAbortReason::kWakeWord);
    EXPECT_EQ(t.outcome, g::TouchOutcome::kR5);
    EXPECT_EQ(t.rev, rig.gate->Snapshot().rev);
    EXPECT_EQ(Drain(rig.audio_q),
              (std::vector<std::string>{
                  R"({"session_id":"sid","type":"abort","gen":2,"dev_abort_seq":1,"reason":"wake_word_detected"})",
                  R"({"session_id":"sid","type":"listen","state":"start","mode":"auto"})"}));
    EXPECT_EQ(rig.audio.stops, 1u);
}

TEST(PlaybackGateShell, AnR5ThatDoesNotFitStillStopsAndEndsThePair) {
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {2, 0, 0});
    for (size_t i = 0; i + 1 < n::kAudioLimits.max_items; i++) rig.audio_q.Push(7, n::ElemKind::kJson, "x", 0);
    const g::TouchResult t = rig.gate->OnTouch(7, w::ListenMode::kManualStop, w::DeviceAbortReason::kTouch);
    EXPECT_EQ(t.outcome, g::TouchOutcome::kSendFailed);
    EXPECT_GE(rig.audio.stops, 1u);  // the stop holds although nothing was sent (F3)
    EXPECT_FALSE(rig.gate->Snapshot().speaking);
    ASSERT_EQ(rig.ends.size(), 1u);
    EXPECT_EQ(rig.ends[0].reason, EndReason::kQueueFull);
    EXPECT_TRUE(rig.gate->Snapshot().dead);
}

TEST(PlaybackGateShell, TouchAnswersWithoutTheCoreWhenTheUiKnowsNoPair) {
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {1, 0, 0});
    EXPECT_EQ(rig.gate->OnTouch(0, w::ListenMode::kManualStop, w::DeviceAbortReason::kTouch).outcome,
              g::TouchOutcome::kUnbound);
    EXPECT_EQ(rig.gate->OnTouch(8, w::ListenMode::kManualStop, w::DeviceAbortReason::kTouch).outcome,
              g::TouchOutcome::kUnbound);
    EXPECT_EQ(rig.audio.stops, 0u);
    EXPECT_TRUE(rig.gate->Snapshot().speaking);
    rig.gate->OnTtsStop(7, 1);
    const g::TouchResult idle = rig.gate->OnTouch(7, w::ListenMode::kManualStop, w::DeviceAbortReason::kTouch);
    EXPECT_EQ(idle.outcome, g::TouchOutcome::kNotSpeaking);
    EXPECT_EQ(idle.rev, rig.gate->Snapshot().rev);
}

TEST(PlaybackGateShell, GateChangedOnlyWhenSpeakingChanges) {
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {1, 0, 0});
    rig.gate->OnTtsStart(7, {2, 0, 0});  // already speaking: no notice
    rig.gate->OnTtsStop(7, 2);
    ASSERT_EQ(rig.changed.size(), 2u);
    EXPECT_EQ(rig.changed[0].e, 7u);
    EXPECT_TRUE(rig.changed[0].spk);
    EXPECT_FALSE(rig.changed[1].spk);
    EXPECT_EQ(rig.changed[1].rev, rig.gate->Snapshot().rev);
    EXPECT_GT(rig.changed[1].rev, rig.changed[0].rev);
}

TEST(PlaybackGateShell, StartFromIdleClearsTheQueuesFirst) {
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {1, 0, 0});
    EXPECT_EQ(rig.audio.clears, 1u);
    rig.gate->OnTtsStart(7, {2, 0, 0});  // already speaking: no clear
    EXPECT_EQ(rig.audio.clears, 1u);
}

TEST(PlaybackGateShell, LinkUpOnlyForTheBoundLivePair) {
    Rig rig;
    EXPECT_FALSE(rig.gate->PostLinkUp(7));  // not bound yet
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {1, 0, 0});
    EXPECT_FALSE(rig.gate->PostLinkUp(8));
    EXPECT_TRUE(rig.gate->PostLinkUp(7));
    ASSERT_EQ(rig.link_ups.size(), 1u);
    EXPECT_TRUE(rig.link_ups[0].spk);
    EXPECT_EQ(rig.link_ups[0].rev, rig.gate->Snapshot().rev);
    rig.gate->StopForDeath(7, EndReason::kF1);
    EXPECT_FALSE(rig.gate->PostLinkUp(7));  // dead
    EXPECT_EQ(rig.link_ups.size(), 1u);
}

TEST(PlaybackGateShell, ServerAudioIsPushedOnlyWhenTheGateTakesIt) {
    Rig rig;
    rig.Bind(7);
    int pushes = 0;
    auto push = [&] {
        pushes++;
        return true;
    };
    EXPECT_EQ(rig.gate->OnServerAudio(7, push), g::Outcome::kDropped);  // not speaking
    rig.gate->OnTtsStart(7, {1, 0, 0});
    EXPECT_EQ(rig.gate->OnServerAudio(7, push), g::Outcome::kQueued);
    EXPECT_EQ(rig.gate->OnServerAudio(8, push), g::Outcome::kStale);
    EXPECT_EQ(pushes, 1);
    EXPECT_EQ(rig.gate->OnServerAudio(7, [] { return false; }), g::Outcome::kDropped);  // decode queue full
    EXPECT_EQ(rig.gate->Stats().dropped_server, 2u);
}

TEST(PlaybackGateShell, DeathStopsClosesAndEndsOnce) {
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {1, 0, 0});
    rig.gate->StopForDeath(8, EndReason::kF2);  // another pair: nothing
    EXPECT_TRUE(rig.ends.empty());
    rig.gate->StopForDeath(7, EndReason::kF2);
    rig.gate->StopForDeath(7, EndReason::kF1);
    ASSERT_EQ(rig.ends.size(), 1u);
    EXPECT_EQ(rig.ends[0].reason, EndReason::kF2);
    EXPECT_EQ(rig.audio.stops, 1u);
    EXPECT_FALSE(rig.gate->Snapshot().speaking);
    EXPECT_EQ(rig.ctrl_q.Push(7, n::ElemKind::kJson, "x", 0), n::PushResult::kClosed);
    EXPECT_EQ(rig.gate->OnTtsStart(7, {2, 1, 0}), g::Outcome::kStale);  // dead: nothing accepted
}

TEST(PlaybackGateShell, ClearForListeningOnlyWhileNotSpeaking) {
    Rig rig;
    rig.Bind(7);
    rig.gate->ClearForListening();
    EXPECT_EQ(rig.audio.clears, 1u);
    rig.gate->OnTtsStart(7, {1, 0, 0});
    const uint32_t c = rig.audio.clears;
    rig.gate->ClearForListening();
    EXPECT_EQ(rig.audio.clears, c);
}

TEST(PlaybackGateShell, UnbindStopsAndForgetsTheSession) {
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {1, 0, 0});
    EXPECT_EQ(rig.gate->Unbind(7), g::Outcome::kReset);
    EXPECT_EQ(rig.gate->session_id(), "");
    EXPECT_EQ(rig.audio.stops, 1u);
    ASSERT_FALSE(rig.changed.empty());
    EXPECT_FALSE(rig.changed.back().spk);
    EXPECT_TRUE(rig.ends.empty());  // the manager ends pairs; Unbind does not ask it to
}

TEST(PlaybackGateShell, AViolationWhoseDoneDoesNotFitEndsWithoutFlush) {
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {3, 0, 0});
    for (size_t i = 0; i < n::kCtrlLimits.max_items; i++) rig.ctrl_q.Push(7, n::ElemKind::kJson, "x", 0);
    w::AbortRequest req;
    req.gen = 2;
    req.req_id = "q";
    req.session_id = "sid";
    EXPECT_EQ(rig.gate->OnAbort(7, req), g::Outcome::kEnded);
    ASSERT_EQ(rig.ends.size(), 1u);
    EXPECT_EQ(rig.ends[0].reason, EndReason::kViolation);
    EXPECT_FALSE(rig.ends[0].flush);  // nothing of ours to flush: do not wait for it
    EXPECT_TRUE(rig.ctrl_q.Drained());
}

TEST(PlaybackGateShell, ATouchWithoutAnyPairIsUnbound) {
    Rig rig;  // the gate is not bound: the core would say "ignored" for e == 0
    const g::TouchResult t = rig.gate->OnTouch(0, w::ListenMode::kManualStop, w::DeviceAbortReason::kTouch);
    EXPECT_EQ(t.outcome, g::TouchOutcome::kUnbound);
}

TEST(PlaybackGateShell, AnAbortOfAnotherOrNoSessionDoesNothing) {  // Codex review 143 I2
    Rig rig;
    rig.Bind(7);
    rig.gate->OnTtsStart(7, {1, 0, 0});
    for (const char* sid : {"other", ""}) {
        w::AbortRequest req;
        req.gen = 1;
        req.req_id = "q";
        req.session_id = sid;
        EXPECT_EQ(rig.gate->OnAbort(7, req), g::Outcome::kStale) << sid;
    }
    EXPECT_EQ(rig.audio.stops, 0u);
    EXPECT_TRUE(rig.gate->Snapshot().speaking);
    EXPECT_TRUE(Drain(rig.ctrl_q).empty());  // no done either
    EXPECT_EQ(rig.gate->Stats().rejected_session, 2u);
}
