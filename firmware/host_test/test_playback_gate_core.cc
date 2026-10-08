// StackChan FW-A2 §2.1-2.3: what the gate core adds on top of the contract vectors
// (pair epochs, dead, rev, stop_serial, start_from_idle, ClearForListening).
#include <gtest/gtest.h>

#include "playback_gate_core.h"

namespace g = stackchan::gate;

namespace {

g::State Bound(uint64_t e = 1) { return g::Bind(g::State{}, e).state; }
g::State Playing(uint64_t e = 1, uint32_t gen = 1) { return g::OnTtsStart(Bound(e), e, gen, 0, 0).state; }

bool SameContractState(const g::State& a, const g::State& b) {
    return a.bound_e == b.bound_e && a.last_ended_e == b.last_ended_e && a.dead == b.dead &&
           a.current_gen == b.current_gen && a.aborted_gen == b.aborted_gen &&
           a.dev_abort_seq == b.dev_abort_seq && a.max_seen_k == b.max_seen_k &&
           a.accepting == b.accepting && a.speaking == b.speaking &&
           a.stop_serial == b.stop_serial && a.rev == b.rev;
}

}  // namespace

TEST(PlaybackGateCore, StaleEpochChangesNothing) {
    const g::State s = Playing(2);
    for (const g::Result& r : {g::OnTtsStart(s, 1, 2, 0, 0), g::OnTtsStop(s, 1, 1), g::OnAbort(s, 1, 1),
                               g::OnServerAudio(s, 1), g::OnTouch(s, 1), g::StopForDeath(s, 1),
                               g::Unbind(s, 1)}) {
        EXPECT_EQ(r.outcome, g::Outcome::kStale);
        EXPECT_FALSE(r.stopped);
        EXPECT_EQ(r.sent.kind, g::SentKind::kNone);
        EXPECT_TRUE(SameContractState(r.state, s));
    }
}

TEST(PlaybackGateCore, BindNeedsANewerEpochAndNoBoundPair) {
    g::State s = Bound(1);
    EXPECT_EQ(g::Bind(s, 2).outcome, g::Outcome::kIgnored);  // already bound
    s = g::Unbind(s, 1).state;
    EXPECT_EQ(s.last_ended_e, 1u);
    EXPECT_EQ(g::Bind(s, 1).outcome, g::Outcome::kStale);    // a late Bind of the ended pair
    const g::Result r = g::Bind(s, 2);
    EXPECT_EQ(r.outcome, g::Outcome::kBound);
    EXPECT_EQ(r.state.bound_e, 2u);
}

TEST(PlaybackGateCore, BindResetsThePairStateButKeepsSerials) {
    g::State s = g::OnTouch(Playing(1, 3), 1).state;  // stop_serial 1, rev 2
    s = g::Unbind(s, 1).state;                      // stop again: stop_serial 2
    const g::State b = g::Bind(s, 2).state;
    EXPECT_EQ(b.current_gen + b.aborted_gen + b.dev_abort_seq + b.max_seen_k, 0u);
    EXPECT_FALSE(b.accepting || b.speaking || b.dead);
    EXPECT_EQ(b.stop_serial, 2u);
    EXPECT_EQ(b.rev, 2u);
}

TEST(PlaybackGateCore, DeadIgnoresEverythingButUnbind) {
    const g::Result d = g::StopForDeath(Playing(1), 1);
    EXPECT_EQ(d.outcome, g::Outcome::kStopped);
    EXPECT_TRUE(d.stopped);
    EXPECT_TRUE(d.state.dead);
    EXPECT_FALSE(d.state.accepting || d.state.speaking);
    const g::State s = d.state;
    for (const g::Result& r : {g::OnTtsStart(s, 1, 2, 0, 0), g::OnAbort(s, 1, 2), g::OnServerAudio(s, 1),
                               g::OnTouch(s, 1), g::Bind(s, 1), g::StopForDeath(s, 1)}) {
        EXPECT_EQ(r.outcome, g::Outcome::kStale);
        EXPECT_TRUE(SameContractState(r.state, s));
    }
    const g::Result u = g::Unbind(s, 1);
    EXPECT_EQ(u.outcome, g::Outcome::kReset);
    EXPECT_FALSE(u.state.dead);
    EXPECT_EQ(u.state.bound_e, 0u);
}

TEST(PlaybackGateCore, ViolationLeavesTheGateDeadUntilUnbind) {
    // R1.2: abort between aborted_gen and current_gen
    g::State s = g::OnTtsStart(Playing(1, 1), 1, 2, 0, 0).state;
    const g::Result r = g::OnAbort(s, 1, 1);
    EXPECT_EQ(r.outcome, g::Outcome::kEnded);
    EXPECT_TRUE(r.state.dead);
    EXPECT_EQ(r.state.bound_e, 1u);  // the shell ends the pair; Unbind clears it
    EXPECT_EQ(g::OnTtsStart(r.state, 1, 3, 2, 0).outcome, g::Outcome::kStale);
}

TEST(PlaybackGateCore, RevCountsSpeakingChanges) {
    g::State s = Bound(1);
    EXPECT_EQ(s.rev, 0u);
    s = g::OnTtsStart(s, 1, 1, 0, 0).state;  // false -> true
    EXPECT_EQ(s.rev, 1u);
    s = g::OnTtsStart(s, 1, 1, 0, 0).state;  // already speaking: same gen restart
    EXPECT_EQ(s.rev, 1u);
    s = g::OnTtsStop(s, 1, 1).state;  // true -> false
    EXPECT_EQ(s.rev, 2u);
    s = g::OnAbort(s, 1, 1).state;  // not speaking: no change
    EXPECT_EQ(s.rev, 2u);
}

TEST(PlaybackGateCore, StopSerialCountsEveryStop) {
    g::State s = Playing(1, 1);
    s = g::OnAbort(s, 1, 1).state;
    EXPECT_EQ(s.stop_serial, 1u);
    s = g::OnAbort(s, 1, 1).state;  // already: no stop
    EXPECT_EQ(s.stop_serial, 1u);
    s = g::OnTtsStart(s, 1, 2, 1, 0).state;
    s = g::OnTouch(s, 1).state;
    EXPECT_EQ(s.stop_serial, 2u);
    s = g::Unbind(s, 1).state;
    EXPECT_EQ(s.stop_serial, 3u);
}

TEST(PlaybackGateCore, StartFromIdleOnlyWhenSpeakingWasFalse) {
    const g::Result first = g::OnTtsStart(Bound(1), 1, 1, 0, 0);
    EXPECT_TRUE(first.start_from_idle);
    EXPECT_FALSE(g::OnTtsStart(first.state, 1, 2, 0, 0).start_from_idle);  // already speaking
    const g::State stopped = g::OnTtsStop(first.state, 1, 1).state;
    EXPECT_TRUE(g::OnTtsStart(stopped, 1, 2, 0, 0).start_from_idle);  // normal next utterance
}

TEST(PlaybackGateCore, TouchWhileSpeakingSendsTheDeviceAbort) {
    const g::Result r = g::OnTouch(Playing(1, 4), 1);
    EXPECT_EQ(r.outcome, g::Outcome::kTouched);
    EXPECT_EQ(r.sent.kind, g::SentKind::kDeviceAbort);
    EXPECT_EQ(r.sent.gen, 4u);
    EXPECT_EQ(r.sent.dev_abort_seq, 1u);
    EXPECT_EQ(r.state.aborted_gen, 4u);
    EXPECT_FALSE(r.state.speaking);
    EXPECT_EQ(g::OnTouch(r.state, 1).outcome, g::Outcome::kIgnored);  // not speaking now
}

TEST(PlaybackGateCore, ClearForListeningOnlyWhileNotSpeaking) {
    EXPECT_TRUE(g::ShouldClearForListening(Bound(1)));
    EXPECT_FALSE(g::ShouldClearForListening(Playing(1)));
    EXPECT_TRUE(g::ShouldClearForListening(g::OnTtsStop(Playing(1), 1, 1).state));
}

// FW-A2 design §3.1 (v10): E is the 64-bit fw_epoch (boot_count * 65536 + conn_index). From
// boot_count 65536 on it exceeds 2^32; a 32-bit E would wrap and the next Bind would be refused
// (Codex review 140 Important 2).
TEST(PlaybackGateCore, EpochAbove32BitsIsKeptWhole) {
    const uint64_t e1 = 65535ull * 65536 + 5;  // boot_count 65535, conn_index 5 (0xFFFF0005)
    const uint64_t e2 = 65536ull * 65536 + 3;  // boot_count 65536, conn_index 3 (0x1'0000'0003)
    g::State s = g::Unbind(Bound(e1), e1).state;
    const g::Result r = g::Bind(s, e2);
    EXPECT_EQ(r.outcome, g::Outcome::kBound);
    EXPECT_EQ(r.state.bound_e, e2);
    // a message for the low 32 bits of E is another pair
    EXPECT_EQ(g::OnTouch(r.state, e2 & 0xFFFFFFFFull).outcome, g::Outcome::kStale);
    EXPECT_EQ(g::Unbind(r.state, e2).state.last_ended_e, e2);
}
