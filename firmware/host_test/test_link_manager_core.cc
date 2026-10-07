// StackChan FW-A2 §3: the link manager's transition function with a fake clock.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "link_manager_core.h"

using namespace stackchan::link;

namespace {

constexpr int64_t S = 1'000'000;  // 1 s in us
constexpr uint32_t E1 = 0x10001;

Input Tick(int64_t now, int64_t ctrl_rx = 0) {
    Input i;
    i.kind = InKind::kTick;
    i.now_us = now;
    i.ctrl_last_rx_us = ctrl_rx;
    return i;
}
Input Connected(uint32_t attempt, uint32_t which, bool ok, uint32_t e = E1, int64_t now = 0) {
    Input i;
    i.kind = InKind::kConnectResult;
    i.attempt = attempt;
    i.which = which;
    i.ok = ok;
    i.e = e;
    i.now_us = now;
    return i;
}
Input HelloReply(uint32_t attempt, uint32_t e, bool ctrl, int64_t at) {
    Input i;
    i.kind = InKind::kAudioHelloReply;
    i.attempt = attempt;
    i.e = e;
    i.ctrl_offered = ctrl;
    i.at_us = at;
    i.now_us = at;
    return i;
}
Input WithE(InKind k, uint32_t e, int64_t now = 0) {
    Input i;
    i.kind = k;
    i.e = e;
    i.now_us = now;
    return i;
}
Input End(uint32_t e, EndReason r, int64_t now, bool flush = false) {
    Input i = WithE(InKind::kEndRequest, e, now);
    i.reason = r;
    i.flush = flush;
    return i;
}
Input Exited(uint32_t e, uint32_t task, int64_t now) {
    Input i = WithE(InKind::kTaskExited, e, now);
    i.which = task;
    return i;
}

Output O(OutKind k, uint32_t e = 0, uint32_t attempt = 0) {
    Output o{k};
    o.e = e;
    o.attempt = attempt;
    return o;
}

State Feed(State s, const std::vector<Input>& ins) {
    for (const auto& i : ins) s = Step(s, i).state;
    return s;
}

// Bound at t=1s with the control link received last at 1s
State Bound() {
    return Feed(State{}, {Tick(0), Connected(1, kAudioRx, true), HelloReply(1, E1, true, S / 2),
                          Connected(1, kCtrlRx, true, E1, S / 2), WithE(InKind::kCtrlHelloReply, E1, S),
                          WithE(InKind::kReadySent, E1, S)});
}

}  // namespace

TEST(LinkManagerCore, BindsStepByStep) {
    StepResult r = Step(State{}, Tick(0));
    EXPECT_EQ(r.state.stage, Stage::kAudioConnect);
    EXPECT_EQ(r.out, std::vector<Output>({O(OutKind::kConnectAudio, 0, 1)}));
    r = Step(r.state, Connected(1, kAudioRx, true));
    EXPECT_EQ(r.state.stage, Stage::kAudioHello);
    EXPECT_EQ(r.out, std::vector<Output>({O(OutKind::kSendAudioHello, E1, 1)}));
    r = Step(r.state, HelloReply(1, E1, true, 2 * S));
    EXPECT_EQ(r.state.stage, Stage::kCtrlConnect);
    EXPECT_EQ(r.out, std::vector<Output>({O(OutKind::kConnectCtrl, E1, 1)}));
    r = Step(r.state, Connected(1, kCtrlRx, true, E1, 2 * S));
    EXPECT_EQ(r.out, std::vector<Output>({O(OutKind::kSendCtrlHello, E1)}));
    r = Step(r.state, WithE(InKind::kCtrlHelloReply, E1, 3 * S));
    EXPECT_EQ(r.out, std::vector<Output>({O(OutKind::kBindGate, E1), O(OutKind::kSendReady, E1)}));
    r = Step(r.state, WithE(InKind::kReadySent, E1, 3 * S));
    EXPECT_EQ(r.state.stage, Stage::kBound);
    EXPECT_EQ(r.out, std::vector<Output>({O(OutKind::kPostLinkUp, E1)}));
}

TEST(LinkManagerCore, TickBetweenTheControlHelloReplyAndReadySentKeepsThePair) {
    // Codex review 131 Important 1: the 100 ms tick crosses the ready send
    State s = Feed(State{}, {Tick(0), Connected(1, kAudioRx, true), HelloReply(1, E1, true, 0),
                             Connected(1, kCtrlRx, true), WithE(InKind::kCtrlHelloReply, E1, S)});
    EXPECT_EQ(s.stage, Stage::kReadySend);
    StepResult r = Step(s, Tick(10 * S, S));
    EXPECT_TRUE(r.out.empty());
    EXPECT_EQ(r.state.stage, Stage::kReadySend);
    EXPECT_EQ(Step(r.state, WithE(InKind::kReadySent, E1, 10 * S)).state.stage, Stage::kBound);
}

TEST(LinkManagerCore, AudioHelloTimeoutEndsTheAttempt) {
    State s = Feed(State{}, {Tick(0), Connected(1, kAudioRx, true, E1, 0)});
    EXPECT_TRUE(Step(s, Tick(5 * S - 1)).out.empty());
    StepResult r = Step(s, Tick(5 * S));
    EXPECT_EQ(r.state.stage, Stage::kEnding);
    EXPECT_EQ(r.state.reason, EndReason::kHelloTimeout);
    EXPECT_EQ(r.state.pending_exits, uint32_t{kAudioRx | kAudioTx});
}

TEST(LinkManagerCore, S6CountsFromTheAudioHelloReplyAndIncludesTheConnectWorker) {
    State s = Feed(State{}, {Tick(0), Connected(1, kAudioRx, true), HelloReply(1, E1, true, 2 * S)});
    EXPECT_TRUE(Step(s, Tick(7 * S - 1)).out.empty());
    StepResult r = Step(s, Tick(7 * S));
    EXPECT_EQ(r.state.reason, EndReason::kS6);
    EXPECT_EQ(r.state.pending_exits, uint32_t{kAudioRx | kAudioTx | kConnWorker});
}

TEST(LinkManagerCore, F1EndsTheBoundPairAfterFiveSecondsWithoutReceive) {
    State s = Bound();
    EXPECT_EQ(Step(s, Tick(6 * S - 1, S)).state.stage, Stage::kBound);
    StepResult r = Step(s, Tick(6 * S, S));
    EXPECT_EQ(r.state.reason, EndReason::kF1);
    ASSERT_FALSE(r.out.empty());
    EXPECT_EQ(r.out[0].kind, OutKind::kStopForDeath);
    EXPECT_EQ(r.out[0].reason, EndReason::kF1);
}

TEST(LinkManagerCore, EndRunsTheStepsInOrderAndPostsLinkDownOnlyAfterLinkUp) {
    StepResult r = Step(Bound(), End(E1, EndReason::kCtrlClosed, 2 * S));
    std::vector<Output> want = {O(OutKind::kStopForDeath, E1), O(OutKind::kUnbindGate, E1),
                                O(OutKind::kCloseQueues, E1), O(OutKind::kPostLinkDown, E1),
                                O(OutKind::kRequestStop, E1)};
    want[0].reason = EndReason::kCtrlClosed;
    EXPECT_EQ(r.out, want);
    EXPECT_EQ(r.state.pending_exits, uint32_t{kAudioRx | kAudioTx | kCtrlRx | kCtrlTx});
    // before LinkUp: no LinkDown
    State s = Feed(State{}, {Tick(0), Connected(1, kAudioRx, true)});
    r = Step(s, End(E1, EndReason::kAudioClosed, S));
    for (const auto& o : r.out) EXPECT_NE(o.kind, OutKind::kPostLinkDown);
}

TEST(LinkManagerCore, EndsOncePerPair) {
    State s = Step(Bound(), End(E1, EndReason::kCtrlClosed, 2 * S)).state;
    StepResult r = Step(s, End(E1, EndReason::kF2, 2 * S));
    EXPECT_TRUE(r.out.empty());
    EXPECT_EQ(r.state.duplicate_ends, 1u);
    EXPECT_EQ(r.state.reason, EndReason::kCtrlClosed);
}

TEST(LinkManagerCore, StaleAttemptsAndPairsAreDropped) {
    State s = Feed(State{}, {Tick(0), Connected(1, kAudioRx, true)});
    StepResult r = Step(s, Connected(0, kAudioRx, true));  // an older attempt
    EXPECT_TRUE(r.out.empty());
    EXPECT_EQ(r.state.stale_inputs, 1u);
    r = Step(s, HelloReply(1, E1 + 1, true, S));  // a reply for another epoch
    EXPECT_TRUE(r.out.empty());
    r = Step(Bound(), End(E1 + 7, EndReason::kAudioClosed, 2 * S));  // an old pair's late notice
    EXPECT_TRUE(r.out.empty());
    EXPECT_EQ(r.state.stage, Stage::kBound);
    r = Step(Bound(), Exited(E1 + 7, kAudioRx, 2 * S));
    EXPECT_EQ(r.state.stage, Stage::kBound);
}

TEST(LinkManagerCore, DisconnectRightAfterTheHelloEndsThePair) {
    State s = Feed(State{}, {Tick(0), Connected(1, kAudioRx, true)});
    StepResult r = Step(s, End(E1, EndReason::kAudioClosed, S / 10));
    EXPECT_EQ(r.state.stage, Stage::kEnding);
    EXPECT_EQ(r.state.reason, EndReason::kAudioClosed);
}

TEST(LinkManagerCore, ViolationFlushesTheControlQueueBeforeStopping) {
    StepResult r = Step(Bound(), End(E1, EndReason::kViolation, 2 * S, true));
    Output keep = O(OutKind::kCloseQueues, E1);
    keep.keep_ctrl = true;
    for (const auto& o : r.out) EXPECT_NE(o.kind, OutKind::kRequestStop);
    EXPECT_NE(std::find(r.out.begin(), r.out.end(), keep), r.out.end());
    EXPECT_TRUE(r.state.flushing);
    StepResult f = Step(r.state, WithE(InKind::kCtrlFlushed, E1, 2 * S + S / 10));
    EXPECT_EQ(f.out, std::vector<Output>({O(OutKind::kRequestStop, E1)}));
    StepResult t = Step(r.state, Tick(4 * S));  // the done's 2 s deadline
    EXPECT_EQ(t.out, std::vector<Output>({O(OutKind::kRequestStop, E1)}));
    EXPECT_TRUE(Step(r.state, Tick(4 * S - 1)).out.empty());
}

TEST(LinkManagerCore, DestroyAfterAllExitsThenBackOffAndDoubleUpTo15s) {
    State s = Step(Bound(), End(E1, EndReason::kCtrlClosed, 2 * S)).state;
    for (uint32_t t : {kAudioRx, kAudioTx, kCtrlRx}) s = Step(s, Exited(E1, t, 2 * S)).state;
    StepResult r = Step(s, Exited(E1, kCtrlTx, 2 * S));
    EXPECT_EQ(r.out, std::vector<Output>({O(OutKind::kDestroy, E1)}));
    EXPECT_EQ(r.state.stage, Stage::kWaiting);
    EXPECT_EQ(r.state.retry_at_us, 3 * S);  // backoff 1 s
    EXPECT_EQ(r.state.backoff_ms, 2000u);
    EXPECT_TRUE(Step(r.state, Tick(3 * S - 1)).out.empty());
    EXPECT_EQ(Step(r.state, Tick(3 * S)).out, std::vector<Output>({O(OutKind::kConnectAudio, 0, 2)}));
    State b = r.state;
    for (int i = 0; i < 6; i++) {  // keep failing: 2, 4, 8, 15 (capped), 15, 15 s
        const int64_t t = b.retry_at_us;
        b = Step(b, Tick(t)).state;
        b = Step(b, Connected(b.attempt, kAudioRx, false, E1, t)).state;
    }
    EXPECT_EQ(b.backoff_ms, kBackoffMaxMs);
}

TEST(LinkManagerCore, ReadyResetsTheBackoff) {
    State s = State{};
    s.backoff_ms = 8000;
    s = Feed(s, {Tick(0), Connected(1, kAudioRx, true), HelloReply(1, E1, true, 0), Connected(1, kCtrlRx, true),
                 WithE(InKind::kCtrlHelloReply, E1), WithE(InKind::kReadySent, E1)});
    EXPECT_EQ(s.backoff_ms, 1000u);
}

TEST(LinkManagerCore, ExitsNotConfirmedInThreeSecondsRestart) {
    State s = Step(Bound(), End(E1, EndReason::kCtrlClosed, 2 * S)).state;
    EXPECT_TRUE(Step(s, Tick(5 * S - 1)).out.empty());
    EXPECT_EQ(Step(s, Tick(5 * S)).out, std::vector<Output>({O(OutKind::kRestart, E1)}));
}

TEST(LinkManagerCore, ConnectFailureGoesStraightToWaiting) {
    State s = Step(State{}, Tick(0)).state;
    StepResult r = Step(s, Connected(1, kAudioRx, false, E1, S));
    EXPECT_TRUE(r.out.empty());
    EXPECT_EQ(r.state.stage, Stage::kWaiting);
    EXPECT_EQ(r.state.retry_at_us, 2 * S);
}

TEST(LinkManagerCore, NoControlOfferedKeepsTheAudioLinkWithoutS6) {
    State s = Feed(State{}, {Tick(0), Connected(1, kAudioRx, true), HelloReply(1, E1, false, S)});
    EXPECT_EQ(s.stage, Stage::kAudioOnly);
    EXPECT_TRUE(Step(s, Tick(60 * S)).out.empty());
}

TEST(LinkManagerCore, WrongEpochControlHelloReplyIsAViolation) {
    State s = Feed(State{}, {Tick(0), Connected(1, kAudioRx, true), HelloReply(1, E1, true, 0),
                             Connected(1, kCtrlRx, true)});
    StepResult r = Step(s, WithE(InKind::kCtrlHelloReply, E1 + 1, S));
    EXPECT_EQ(r.state.reason, EndReason::kViolation);
}

TEST(LinkManagerCore, ShutdownStopsReconnecting) {
    State s = Step(Bound(), WithE(InKind::kShutdown, 0, 2 * S)).state;
    EXPECT_EQ(s.reason, EndReason::kShutdown);
    for (uint32_t t : {kAudioRx, kAudioTx, kCtrlRx, kCtrlTx}) s = Step(s, Exited(E1, t, 2 * S)).state;
    EXPECT_EQ(s.stage, Stage::kStopped);
    EXPECT_TRUE(Step(s, Tick(100 * S)).out.empty());
}

// Final reviews 135/136: the end waits for exactly the tasks that exist, whatever the order of
// the notices from the shell's tasks.
TEST(LinkManagerCore, AllExitsDuringTheFlushDestroyAtOnce) {
    // Codex 136 Important 2, Claude 135 A: nothing is left to flush once every task exited
    State s = Step(Bound(), End(E1, EndReason::kViolation, 2 * S, true)).state;
    for (uint32_t t : {kAudioRx, kAudioTx, kCtrlRx}) {
        StepResult r = Step(s, Exited(E1, t, 2 * S));
        EXPECT_TRUE(r.out.empty());
        s = r.state;
    }
    StepResult r = Step(s, Exited(E1, kCtrlTx, 2 * S));
    EXPECT_EQ(r.out, std::vector<Output>({O(OutKind::kDestroy, E1)}));
    EXPECT_EQ(r.state.stage, Stage::kWaiting);
    EXPECT_TRUE(Step(r.state, WithE(InKind::kCtrlFlushed, E1, 2 * S)).out.empty());  // late: stale
    for (const auto& o : Step(r.state, Tick(5 * S)).out) EXPECT_NE(o.kind, OutKind::kRestart);
}

TEST(LinkManagerCore, AnExitBeforeTheEndRequestIsNotWaitedForAgain) {
    // Claude 135 B: the receive task exits, then its EndRequest arrives
    StepResult early = Step(Bound(), Exited(E1, kAudioRx, 2 * S));
    EXPECT_TRUE(early.out.empty());
    EXPECT_EQ(early.state.stale_inputs, Bound().stale_inputs);
    State s = Step(early.state, End(E1, EndReason::kAudioClosed, 2 * S)).state;
    EXPECT_EQ(s.pending_exits, uint32_t{kAudioTx | kCtrlRx | kCtrlTx});
    for (uint32_t t : {kAudioTx, kCtrlRx}) s = Step(s, Exited(E1, t, 2 * S)).state;
    EXPECT_EQ(Step(s, Exited(E1, kCtrlTx, 2 * S)).out, std::vector<Output>({O(OutKind::kDestroy, E1)}));
}

TEST(LinkManagerCore, AConnectionMadeAfterTheEndIsStoppedAndWaitedFor) {
    // Codex 136 Important 1, Claude 135 C: S6 while connecting the control link, then the worker
    // reports success (its receive task already runs) before it exits
    State s = Feed(State{}, {Tick(0), Connected(1, kAudioRx, true), HelloReply(1, E1, true, 2 * S)});
    s = Step(s, Tick(7 * S)).state;
    ASSERT_EQ(s.stage, Stage::kEnding);
    StepResult r = Step(s, Connected(1, kCtrlRx, true, E1, 7 * S));
    EXPECT_EQ(r.out, std::vector<Output>({O(OutKind::kRequestStop, E1, 1)}));
    EXPECT_EQ(r.state.pending_exits, uint32_t{kAudioRx | kAudioTx | kConnWorker | kCtrlRx | kCtrlTx});
    EXPECT_EQ(r.state.stale_inputs, s.stale_inputs);
    s = r.state;
    for (uint32_t t : {kAudioRx, kAudioTx, kConnWorker, kCtrlRx}) {
        StepResult x = Step(s, Exited(E1, t, 7 * S));
        EXPECT_TRUE(x.out.empty());
        s = x.state;
    }
    EXPECT_EQ(Step(s, Exited(E1, kCtrlTx, 7 * S)).out, std::vector<Output>({O(OutKind::kDestroy, E1)}));
    // the audio link made after a shutdown during its connect, likewise
    State a = Step(Step(State{}, Tick(0)).state, WithE(InKind::kShutdown, 0, 0)).state;
    ASSERT_EQ(a.pending_exits, uint32_t{kConnWorker});
    StepResult ra = Step(a, Connected(1, kAudioRx, true, E1, 0));
    EXPECT_EQ(ra.state.pending_exits, uint32_t{kConnWorker | kAudioRx | kAudioTx});
    ASSERT_EQ(ra.out.size(), 1u);
    EXPECT_EQ(ra.out[0].kind, OutKind::kRequestStop);
    // a failure adds nothing; the worker's own exit ends the wait
    StepResult rf = Step(a, Connected(1, kAudioRx, false, E1, 0));
    EXPECT_EQ(rf.state.pending_exits, uint32_t{kConnWorker});
    EXPECT_EQ(Step(rf.state, Exited(0, kConnWorker, 0)).out, std::vector<Output>({O(OutKind::kDestroy, 0)}));
}
