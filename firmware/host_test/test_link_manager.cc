// StackChan FW-A2 §3, §3.8: the link manager shell (the notice queue, the tick, the ports) and
// the link phases around the connect result.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "link_manager.h"

using namespace stackchan::link;

namespace {

constexpr int64_t S = 1'000'000;
constexpr uint64_t E = 65536ull * 65536 + 2;

struct FakePorts : ManagerPorts {
    int64_t now = 0, ctrl_rx = 0;
    bool wraps = false;
    std::vector<std::string> log;
    void Add(const std::string& s) { log.push_back(s); }
    static std::string N(uint64_t v) { return std::to_string(v); }
    int64_t NowUs() override { return now; }
    int64_t CtrlLastRxUs() override { return ctrl_rx; }
    bool NextConnectionWraps() override { return wraps; }
    void ConnectAudio(uint32_t a) override { Add("connect_audio " + N(a)); }
    void SendAudioHello(uint32_t a, uint64_t e) override { Add("audio_hello " + N(a) + " " + N(e)); }
    void ConnectCtrl(uint32_t a, uint64_t e) override { Add("connect_ctrl " + N(a) + " " + N(e)); }
    void SendCtrlHello(uint64_t e) override { Add("ctrl_hello " + N(e)); }
    void BindGate(uint64_t e) override { Add("bind " + N(e)); }
    void SendReady(uint64_t e) override { Add("ready " + N(e)); }
    void PostLinkUp(uint64_t e) override { Add("link_up " + N(e)); }
    void StopForDeath(uint64_t e, EndReason r) override { Add("death " + N(e) + " " + N(static_cast<int>(r))); }
    void UnbindGate(uint64_t e) override { Add("unbind " + N(e)); }
    void CloseQueues(uint64_t e, bool keep) override { Add("close " + N(e) + (keep ? " keep" : "")); }
    void PostLinkDown(uint64_t e) override { Add("link_down " + N(e)); }
    void RequestStop(uint64_t e) override { Add("stop " + N(e)); }
    void Destroy(uint64_t e) override { Add("destroy " + N(e)); }
    void Restart(uint64_t e, const char* why) override { Add("restart " + N(e) + " " + why); }
};

Input In(InKind k, uint64_t e = 0, uint32_t attempt = 1) {
    Input i;
    i.kind = k;
    i.e = e;
    i.attempt = attempt;
    return i;
}
Input Result(uint32_t which, bool ok) {
    Input i = In(InKind::kConnectResult, E);
    i.which = which;
    i.ok = ok;
    return i;
}
Input Hello() {
    Input i = In(InKind::kAudioHelloReply, E);
    i.ctrl_offered = true;
    return i;
}

struct Rig {
    FakePorts ports;
    NoticeQueue q;
    LinkManager m{&ports, &q};
    void Post(const Input& in) { ASSERT_TRUE(q.Post(in)); }
    // post a notice and let the manager take it (the tick of `now` already ran)
    void Feed(const Input& in) {
        Post(in);
        m.RunOnce();
    }
    void Bound() {
        m.RunOnce();  // tick: connect
        Feed(Result(kAudioRx, true));
        Feed(Hello());
        Feed(Result(kCtrlRx, true));
        Feed(In(InKind::kCtrlHelloReply, E));
        Feed(In(InKind::kReadySent, E));
    }
};

}  // namespace

TEST(LinkManagerShell, BindsThroughTheQueueAndRunsTheOutputsInOrder) {
    Rig r;
    r.Bound();
    EXPECT_EQ(r.ports.log, (std::vector<std::string>{"connect_audio 1", "audio_hello 1 " + FakePorts::N(E),
                                                     "connect_ctrl 1 " + FakePorts::N(E), "ctrl_hello " + FakePorts::N(E),
                                                     "bind " + FakePorts::N(E), "ready " + FakePorts::N(E),
                                                     "link_up " + FakePorts::N(E)}));
    EXPECT_EQ(r.m.state().stage, Stage::kBound);
    EXPECT_EQ(r.m.ticks(), 1u);
}

TEST(LinkManagerShell, TheTickIsNotDelayedByAFloodOfNotices) {
    Rig r;
    r.Bound();
    r.ports.log.clear();
    for (int i = 0; i < 20; i++) r.Post(In(InKind::kTaskExited, 0, 99));  // stale notices
    r.ports.now = 6 * S;  // F1: no control receive for 5 s
    r.m.RunOnce();
    EXPECT_EQ(r.m.ticks(), 2u);  // the tick ran before any of the queued notices
    ASSERT_FALSE(r.ports.log.empty());
    EXPECT_EQ(r.ports.log.front(), "death " + FakePorts::N(E) + " " + std::to_string(static_cast<int>(EndReason::kF1)));
    EXPECT_EQ(r.m.state().stage, Stage::kEnding);
}

TEST(LinkManagerShell, OneNoticePerTurnBetweenTicks) {
    Rig r;
    r.m.RunOnce();  // tick at 0
    r.Post(Result(kAudioRx, true));
    r.Post(Hello());
    r.m.RunOnce();
    EXPECT_EQ(r.m.state().stage, Stage::kAudioHello);  // only the first one
    r.m.RunOnce();
    EXPECT_EQ(r.m.state().stage, Stage::kCtrlConnect);
}

TEST(LinkManagerShell, TheQueueHoldsThirtyTwoAndALostNoticeRestarts) {
    Rig r;
    for (size_t i = 0; i < kNoticeQueueLen; i++) EXPECT_TRUE(r.q.Post(In(InKind::kTaskExited, 0, 99)));
    EXPECT_EQ(r.q.min_free(), 0u);
    EXPECT_FALSE(r.q.lost());
    EXPECT_FALSE(r.q.Post(In(InKind::kTaskExited, 0, 99)));
    EXPECT_TRUE(r.q.lost());
    r.m.RunOnce();
    EXPECT_TRUE(r.m.restarted());
    EXPECT_EQ(r.ports.log, (std::vector<std::string>{"restart 0 lost notice"}));
    EXPECT_EQ(r.m.ticks(), 0u);  // not even the tick: the pair's lifetime is unknown
}

TEST(LinkManagerShell, AConnectionThatWouldWrapConnIndexRestartsInstead) {
    Rig r;
    r.ports.wraps = true;
    r.m.RunOnce();
    EXPECT_TRUE(r.m.restarted());
    EXPECT_EQ(r.ports.log, (std::vector<std::string>{"restart 0 conn_index wraps"}));
}

TEST(LinkManagerShell, ExitsNotConfirmedRestart) {
    Rig r;
    r.Bound();
    r.ports.now = S;
    r.m.RunOnce();  // tick
    r.Feed([] {
        Input i = In(InKind::kEndRequest, E);
        i.reason = EndReason::kAudioClosed;
        i.now_us = S;
        return i;
    }());
    r.ports.log.clear();
    r.ports.now = S + 3 * S;
    r.m.RunOnce();
    EXPECT_TRUE(r.m.restarted());
    EXPECT_EQ(r.ports.log, (std::vector<std::string>{"restart " + FakePorts::N(E) + " task exits not confirmed"}));
}

TEST(LinkManagerShell, ShutdownStopsReconnecting) {
    Rig r;
    r.m.RunOnce();
    Input fail = Result(kAudioRx, false);
    r.Feed(fail);
    Input worker = In(InKind::kTaskExited, 0, 1);
    worker.which = kAudioWorker;
    r.Feed(worker);
    r.Feed(In(InKind::kShutdown));
    r.ports.log.clear();
    for (int i = 1; i <= 50; i++) {
        r.ports.now = i * S;
        r.m.RunOnce();
    }
    EXPECT_TRUE(r.ports.log.empty());
    EXPECT_EQ(r.m.state().stage, Stage::kStopped);
}

TEST(LinkManagerShell, TakeWaitsUpToTheTimeoutAndWakesOnPost) {
    NoticeQueue q;
    EXPECT_FALSE(q.Take(1000).has_value());
    std::thread t([&q] { q.Post(In(InKind::kShutdown)); });
    const auto in = q.Take(5 * S);
    t.join();
    ASSERT_TRUE(in.has_value());
    EXPECT_EQ(in->kind, InKind::kShutdown);
}

TEST(LinkPhase, AnEndAfterTheResultIsPostedOncePerSide) {
    LinkPhase p;
    EXPECT_FALSE(p.OnResultPosted());
    EXPECT_TRUE(p.OnEnded(LinkPhase::Side::kRx));
    EXPECT_FALSE(p.OnEnded(LinkPhase::Side::kRx));
    EXPECT_TRUE(p.OnEnded(LinkPhase::Side::kTx));
    EXPECT_FALSE(p.OnEnded(LinkPhase::Side::kTx));
}

TEST(LinkPhase, AnEndBeforeTheResultIsLeftToTheWorker) {
    LinkPhase p;
    EXPECT_FALSE(p.OnEnded(LinkPhase::Side::kRx));
    EXPECT_FALSE(p.OnEnded(LinkPhase::Side::kTx));
    EXPECT_TRUE(p.OnResultPosted());  // the worker posts it, after the result
    EXPECT_FALSE(p.OnEnded(LinkPhase::Side::kRx));  // already posted for this side
    EXPECT_FALSE(p.OnEnded(LinkPhase::Side::kTx));
}

TEST(LinkPhase, TheTxSideAfterTheResultStillPostsWhenOnlyRxLeftItToTheWorker) {
    LinkPhase p;
    EXPECT_FALSE(p.OnEnded(LinkPhase::Side::kRx));
    EXPECT_TRUE(p.OnResultPosted());
    EXPECT_TRUE(p.OnEnded(LinkPhase::Side::kTx));  // a second end (allowed duplicate), once
}

TEST(LinkPhase, RacingWorkerAndTasksNeverLoseTheEndNorPostItBeforeTheResult) {
    for (int round = 0; round < 20000; round++) {
        LinkPhase p;
        std::atomic<bool> result_posted{false};
        std::atomic<int> ends{0}, early{0};
        auto side = [&](LinkPhase::Side s) {
            if (p.OnEnded(s)) {
                if (!result_posted.load()) early++;
                ends++;
            }
        };
        std::thread rx(side, LinkPhase::Side::kRx);
        std::thread tx(side, LinkPhase::Side::kTx);
        std::thread worker([&] {
            result_posted.store(true);  // the result is in the queue first
            if (p.OnResultPosted()) ends++;
        });
        rx.join();
        tx.join();
        worker.join();
        ASSERT_GE(ends.load(), 1) << "round " << round;
        ASSERT_LE(ends.load(), 2) << "round " << round;
        ASSERT_EQ(early.load(), 0) << "round " << round;
    }
}

TEST(LinkPhase, TheHelloReplyIsReportedOnce) {
    LinkPhase p;
    EXPECT_TRUE(p.TakeHelloReply());
    EXPECT_FALSE(p.TakeHelloReply());
}

TEST(LinkManagerShell, AViolationKeepsTheControlQueueToFlushTheDone) {
    Rig r;
    r.Bound();
    r.ports.log.clear();
    Input end = In(InKind::kEndRequest, E);
    end.reason = EndReason::kViolation;
    end.flush = true;
    r.Feed(end);
    EXPECT_NE(std::find(r.ports.log.begin(), r.ports.log.end(), "close " + FakePorts::N(E) + " keep"), r.ports.log.end());
}

// Claude review 146 Important 1: the shell stamps every notice it takes with its own clock, so a
// producer that leaves now_us at 0 cannot move the deadlines back to the epoch of the clock.
TEST(LinkManagerShell, NoticesAreStampedWithTheShellsClock) {
    Rig r;
    r.ports.now = 100 * S;
    r.m.RunOnce();  // tick: connect
    r.Feed(Result(kAudioRx, true));  // now_us left at 0 by the producer
    r.ports.now += kTickUs;
    r.m.RunOnce();  // tick: the hello deadline is 5 s after the result, not after 0
    EXPECT_EQ(r.ports.log, (std::vector<std::string>{"connect_audio 1", "audio_hello 1 " + FakePorts::N(E)}));

    Rig f;  // a failed connect: the next attempt waits for the worker's exit, then backs off
    f.ports.now = 100 * S;
    f.m.RunOnce();
    f.Feed(Result(kAudioRx, false));
    f.ports.now += kTickUs;
    f.m.RunOnce();  // a tick while the failed connect's worker still runs: wait, no restart
    Input exited = In(InKind::kTaskExited, 0, 1);
    exited.which = kAudioWorker;
    f.Feed(exited);
    EXPECT_EQ(f.ports.log, (std::vector<std::string>{"connect_audio 1"}));
    for (int i = 0; i < 300; i++) {  // 30 s of ticks
        f.ports.now += kTickUs;
        f.m.RunOnce();
    }
    EXPECT_EQ(std::count(f.ports.log.begin(), f.ports.log.end(), "connect_audio 2"), 1);
    EXPECT_EQ(std::count_if(f.ports.log.begin(), f.ports.log.end(),
                            [](const std::string& s) { return s.rfind("restart", 0) == 0; }),
              0);
}
