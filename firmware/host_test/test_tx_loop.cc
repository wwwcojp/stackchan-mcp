// StackChan FW-A2 plan 2B-1: a link's send task (design §4.1, §3.8) with a fake socket and clock.
// What was sent is read back with the frame decoder.
#include <gtest/gtest.h>

#include <cJSON.h>

#include <string>
#include <vector>

#include "tx_loop.h"
#include "ws_frame.h"

using namespace stackchan;
using namespace stackchan::link;

namespace {

constexpr uint64_t E = 65536ull * 65536 + 3;  // above 2^32 (design §3.1)

struct Fake {
    enum Mode { kAll, kBlock, kError } mode = kAll;
    int64_t now = 1'000'000;
    bool stop = false;
    int stop_after_slices = -1;  // kBlock: set stop after this many slices
    std::string wire;
    std::vector<Input> posted;
    std::vector<std::pair<uint64_t, EndReason>> deaths;
    std::vector<int64_t> idles;
    TxPorts Ports() {
        TxPorts p;
        p.now_us = [this] { return now; };
        p.send = [this](const uint8_t* d, size_t n, int64_t timeout) -> int {
            if (mode == kError) return -1;
            if (mode == kBlock) {
                now += timeout;
                if (stop_after_slices > 0 && --stop_after_slices == 0) stop = true;
                return 0;
            }
            wire.append(reinterpret_cast<const char*>(d), n);
            return static_cast<int>(n);
        };
        p.stop_requested = [this] { return stop; };
        p.mask = [](uint8_t m[4]) {
            m[0] = 1;
            m[1] = 2;
            m[2] = 3;
            m[3] = 4;
        };
        p.stop_for_death = [this](uint64_t e, EndReason r) { deaths.push_back({e, r}); };
        p.post = [this](const Input& in) {
            posted.push_back(in);
            return true;
        };
        p.idle = [this](int64_t us) {
            idles.push_back(us);
            now += us;
        };
        return p;
    }
    std::vector<wsframe::Message> Frames() const {
        wsframe::Decoder d;
        d.Feed(wire.data(), wire.size());
        std::vector<wsframe::Message> out;
        wsframe::Message m;
        while (d.Next(&m) == wsframe::Status::kMessage) out.push_back(m);
        return out;
    }
};

uint64_t Num(const std::string& json, const char* key) {
    cJSON* root = cJSON_Parse(json.c_str());
    const cJSON* v = cJSON_GetObjectItem(root, key);
    const uint64_t out = cJSON_IsNumber(v) ? static_cast<uint64_t>(v->valuedouble) : 0;
    cJSON_Delete(root);
    return out;
}

std::string Text(cJSON* root) {
    char* p = cJSON_PrintUnformatted(root);
    std::string s(p);
    cJSON_free(p);
    cJSON_Delete(root);
    return s;
}

}  // namespace

// fw_epoch / seq are stamped in send order with the pair's E: the link's first JSON (the hello)
// is seq 1 (contract S8, FW-A §2.2). Mic frames go out as they were queued (binary).
TEST(TxLoop, StampsJsonInSendOrder) {
    Fake f;
    LinkPhase phase;
    net::SendQueue q(net::kAudioLimits);
    q.Open(E);
    ASSERT_EQ(q.Push(E, net::ElemKind::kJson, R"({"type":"hello"})", f.now), net::PushResult::kQueued);
    ASSERT_EQ(q.Push(E, net::ElemKind::kJson, R"({"type":"listen","state":"start"})", f.now), net::PushResult::kQueued);
    ASSERT_EQ(q.Push(E, net::ElemKind::kMic, "abc", f.now), net::PushResult::kQueued);
    TxLoop tx(LinkSide::kAudio, E, &q, &phase, f.Ports());
    for (int i = 0; i < 3; i++) EXPECT_TRUE(tx.RunOnce());
    const auto frames = f.Frames();
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0].kind, wsframe::Kind::kText);
    EXPECT_EQ(Num(frames[0].payload, "fw_epoch"), E);
    EXPECT_EQ(Num(frames[0].payload, "seq"), 1u);
    EXPECT_EQ(Num(frames[1].payload, "seq"), 2u);
    EXPECT_EQ(frames[2].kind, wsframe::Kind::kBinary);
    EXPECT_EQ(frames[2].payload, "abc");
    EXPECT_EQ(tx.stats().sent, 3u);
    EXPECT_TRUE(f.posted.empty());
}

// The control link: hello seq 1, ready seq 2 (contract S8); ReadySent once the ready went out.
TEST(TxLoop, ReadyIsReportedAfterItIsSent) {
    Fake f;
    LinkPhase phase;
    net::SendQueue q(net::kCtrlLimits);
    q.Open(E);
    ASSERT_EQ(q.Push(E, net::ElemKind::kJson, Text(wire::BuildCtrlHello(E)), f.now), net::PushResult::kQueued);
    ASSERT_EQ(q.Push(E, net::ElemKind::kJson, Text(wire::BuildReady(E)), f.now), net::PushResult::kQueued);
    TxLoop tx(LinkSide::kCtrl, E, &q, &phase, f.Ports());
    EXPECT_TRUE(tx.RunOnce());
    EXPECT_TRUE(f.posted.empty());  // the hello is not the ready
    EXPECT_TRUE(tx.RunOnce());
    ASSERT_EQ(f.posted.size(), 1u);
    EXPECT_EQ(f.posted[0].kind, InKind::kReadySent);
    EXPECT_EQ(f.posted[0].e, E);
    const auto frames = f.Frames();
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(Num(frames[1].payload, "seq"), 2u);
    EXPECT_EQ(Num(frames[1].payload, "fw_epoch"), E);
    EXPECT_EQ(Num(frames[1].payload, "audio_epoch"), E);
}

// F2: the element's deadline passes while the socket takes nothing. The sound stops at once
// (the gate) and the pair ends (the end request), once; the task leaves.
TEST(TxLoop, AMissedDeadlineStopsTheSoundAndEndsThePairOnce) {
    Fake f;
    f.mode = Fake::kBlock;
    LinkPhase phase;
    ASSERT_FALSE(phase.OnResultPosted());  // after the connect result: this task posts its end
    net::SendQueue q(net::kAudioLimits);
    q.Open(E);
    ASSERT_EQ(q.Push(E, net::ElemKind::kJson, R"({"type":"listen"})", f.now), net::PushResult::kQueued);
    TxLoop tx(LinkSide::kAudio, E, &q, &phase, f.Ports());
    EXPECT_FALSE(tx.RunOnce());
    ASSERT_EQ(f.deaths.size(), 1u);
    EXPECT_EQ(f.deaths[0], std::make_pair(E, EndReason::kF2));
    ASSERT_EQ(f.posted.size(), 1u);
    EXPECT_EQ(f.posted[0].kind, InKind::kEndRequest);
    EXPECT_EQ(f.posted[0].e, E);
    EXPECT_EQ(f.posted[0].reason, EndReason::kF2);
    EXPECT_EQ(tx.stats().f2, 1u);
}

// Before the connect result the worker posts the end (design §3.8, v10): the send task only
// marks it, so the manager reads the result first.
TEST(TxLoop, BeforeTheConnectResultTheWorkerPostsTheEnd) {
    Fake f;
    f.mode = Fake::kBlock;
    LinkPhase phase;
    net::SendQueue q(net::kAudioLimits);
    q.Open(E);
    ASSERT_EQ(q.Push(E, net::ElemKind::kJson, R"({"type":"hello"})", f.now), net::PushResult::kQueued);
    TxLoop tx(LinkSide::kAudio, E, &q, &phase, f.Ports());
    EXPECT_FALSE(tx.RunOnce());
    EXPECT_EQ(f.deaths.size(), 1u);
    EXPECT_TRUE(f.posted.empty());
    EXPECT_TRUE(phase.OnResultPosted());  // the worker posts the end request after the result
}

TEST(TxLoop, ASendErrorEndsThePairByTheSide) {
    for (LinkSide side : {LinkSide::kAudio, LinkSide::kCtrl}) {
        Fake f;
        f.mode = Fake::kError;
        LinkPhase phase;
        phase.OnResultPosted();
        net::SendQueue q(side == LinkSide::kAudio ? net::kAudioLimits : net::kCtrlLimits);
        q.Open(E);
        ASSERT_EQ(q.Push(E, net::ElemKind::kJson, R"({"type":"x"})", f.now), net::PushResult::kQueued);
        TxLoop tx(side, E, &q, &phase, f.Ports());
        EXPECT_FALSE(tx.RunOnce());
        EXPECT_TRUE(f.deaths.empty());  // a closed peer is not F2: the receive side sees it too
        ASSERT_EQ(f.posted.size(), 1u);
        EXPECT_EQ(f.posted[0].kind, InKind::kEndRequest);
        EXPECT_EQ(f.posted[0].reason, side == LinkSide::kAudio ? EndReason::kAudioClosed : EndReason::kCtrlClosed);
        EXPECT_EQ(tx.stats().errors, 1u);
    }
}

TEST(TxLoop, TheStopRequestLeavesWithoutNotices) {
    Fake f;
    f.stop = true;
    LinkPhase phase;
    phase.OnResultPosted();
    net::SendQueue q(net::kAudioLimits);
    q.Open(E);
    ASSERT_EQ(q.Push(E, net::ElemKind::kJson, R"({"type":"x"})", f.now), net::PushResult::kQueued);
    TxLoop tx(LinkSide::kAudio, E, &q, &phase, f.Ports());
    EXPECT_FALSE(tx.RunOnce());
    EXPECT_TRUE(f.wire.empty());

    Fake g;  // the stop comes while a send waits for room
    g.mode = Fake::kBlock;
    g.stop_after_slices = 1;
    TxLoop tx2(LinkSide::kAudio, E, &q, &phase, g.Ports());
    EXPECT_FALSE(tx2.RunOnce());
    EXPECT_TRUE(g.deaths.empty());
    EXPECT_TRUE(g.posted.empty());
}

// A violation: the done is flushed, then kCtrlFlushed once (the manager stops waiting).
TEST(TxLoop, TheFlushIsReportedOnceWhenTheDoneIsSent) {
    Fake f;
    LinkPhase phase;
    phase.OnResultPosted();
    net::SendQueue q(net::kCtrlLimits);
    q.Open(E);
    ASSERT_EQ(q.Push(E, net::ElemKind::kJson, R"({"type":"abort","state":"done"})", f.now), net::PushResult::kQueued);
    q.CloseForFlush();
    TxLoop tx(LinkSide::kCtrl, E, &q, &phase, f.Ports());
    EXPECT_TRUE(tx.RunOnce());
    ASSERT_EQ(f.posted.size(), 1u);
    EXPECT_EQ(f.posted[0].kind, InKind::kCtrlFlushed);
    EXPECT_EQ(f.posted[0].e, E);
    EXPECT_TRUE(tx.RunOnce());  // nothing left: returns at once (follow-up 3), no second notice
    EXPECT_EQ(f.posted.size(), 1u);
    EXPECT_EQ(f.Frames().size(), 1u);
}

// A flush that cannot finish (the send fails) is reported as flushed too, with the end request.
TEST(TxLoop, AFailedFlushIsReportedWithTheEnd) {
    Fake f;
    f.mode = Fake::kError;
    LinkPhase phase;
    phase.OnResultPosted();
    net::SendQueue q(net::kCtrlLimits);
    q.Open(E);
    ASSERT_EQ(q.Push(E, net::ElemKind::kJson, R"({"type":"abort","state":"done"})", f.now), net::PushResult::kQueued);
    q.CloseForFlush();
    TxLoop tx(LinkSide::kCtrl, E, &q, &phase, f.Ports());
    EXPECT_FALSE(tx.RunOnce());
    ASSERT_EQ(f.posted.size(), 2u);
    EXPECT_EQ(f.posted[0].kind, InKind::kEndRequest);
    EXPECT_EQ(f.posted[1].kind, InKind::kCtrlFlushed);
}

// No flush: a plain close or a failure posts no kCtrlFlushed (it would be a stale input).
TEST(TxLoop, NoFlushNoticeWithoutAFlush) {
    Fake f;
    LinkPhase phase;
    phase.OnResultPosted();
    net::SendQueue q(net::kCtrlLimits);
    q.Open(E);
    q.Close();
    TxLoop tx(LinkSide::kCtrl, E, &q, &phase, f.Ports());
    EXPECT_TRUE(tx.RunOnce());
    EXPECT_TRUE(f.posted.empty());

    Fake g;
    g.mode = Fake::kError;
    net::SendQueue r(net::kCtrlLimits);
    r.Open(E);
    ASSERT_EQ(r.Push(E, net::ElemKind::kJson, R"({"type":"x"})", g.now), net::PushResult::kQueued);
    TxLoop tx2(LinkSide::kCtrl, E, &r, &phase, g.Ports());
    EXPECT_FALSE(tx2.RunOnce());
    ASSERT_EQ(g.posted.size(), 1u);
    EXPECT_EQ(g.posted[0].kind, InKind::kEndRequest);
}

TEST(TxLoop, PongCloseAndBrokenJson) {
    Fake f;
    LinkPhase phase;
    net::SendQueue q(net::kAudioLimits);
    q.Open(E);
    ASSERT_EQ(q.Push(E, net::ElemKind::kPong, "pp", f.now), net::PushResult::kQueued);
    ASSERT_EQ(q.Push(E, net::ElemKind::kJson, "not json", f.now), net::PushResult::kQueued);
    ASSERT_EQ(q.Push(E, net::ElemKind::kClose, "", f.now), net::PushResult::kQueued);
    TxLoop tx(LinkSide::kAudio, E, &q, &phase, f.Ports());
    for (int i = 0; i < 3; i++) EXPECT_TRUE(tx.RunOnce());
    const auto frames = f.Frames();
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0].kind, wsframe::Kind::kPong);
    EXPECT_EQ(frames[0].payload, "pp");
    EXPECT_EQ(frames[1].kind, wsframe::Kind::kClose);
    EXPECT_EQ(tx.stats().bad_json, 1u);
}

// A closed queue makes Pop return at once (follow-up 3); the task then rests a short while
// instead of spinning (Codex review 148 Minor 1): before its pair opens the queue, after the pair
// ended, and after the flush was reported.
TEST(TxLoop, AClosedQueueRestsInsteadOfSpinning) {
    Fake f;
    LinkPhase phase;
    net::SendQueue q(net::kAudioLimits);  // never opened: the link is up, its hello not queued yet
    TxLoop tx(LinkSide::kAudio, E, &q, &phase, f.Ports());
    EXPECT_TRUE(tx.RunOnce());
    EXPECT_EQ(f.idles, (std::vector<int64_t>{kIdleUs}));
    EXPECT_TRUE(f.posted.empty());

    Fake g;
    net::SendQueue r(net::kCtrlLimits);
    r.Open(E);
    ASSERT_EQ(r.Push(E, net::ElemKind::kJson, R"({"type":"abort","state":"done"})", g.now), net::PushResult::kQueued);
    r.CloseForFlush();
    TxLoop tx2(LinkSide::kCtrl, E, &r, &phase, g.Ports());
    EXPECT_TRUE(tx2.RunOnce());  // sends the done: no rest
    EXPECT_TRUE(g.idles.empty());
    EXPECT_TRUE(tx2.RunOnce());  // flushed: rests
    EXPECT_EQ(g.idles.size(), 1u);
    EXPECT_EQ(g.posted.size(), 1u);  // kCtrlFlushed once
}

// A JSON that must go out in order (a hello, the ready): a full queue ends the pair, a closed
// one (the pair already ended) is only reported (design §4.1, Codex review 148 Important 3).
TEST(TxLoop, AFullQueueEndsThePairAClosedOneDoesNot) {
    std::vector<std::pair<uint64_t, EndReason>> deaths;
    std::vector<Input> posted;
    auto stop = [&](uint64_t e, EndReason r) { deaths.push_back({e, r}); };
    auto post = [&](const Input& in) {
        posted.push_back(in);
        return true;
    };
    net::SendQueue q(net::kCtrlLimits);
    q.Open(E);
    for (size_t i = 0; i < net::kCtrlLimits.max_items; i++) {
        ASSERT_EQ(q.Push(E, net::ElemKind::kJson, "{}", 0), net::PushResult::kQueued);
    }
    EXPECT_EQ(QueueJsonOrEnd(q, E, R"({"type":"ready"})", 0, stop, post), net::PushResult::kFull);
    ASSERT_EQ(deaths.size(), 1u);
    EXPECT_EQ(deaths[0], std::make_pair(E, EndReason::kQueueFull));
    ASSERT_EQ(posted.size(), 1u);
    EXPECT_EQ(posted[0].kind, InKind::kEndRequest);
    EXPECT_EQ(posted[0].e, E);
    EXPECT_EQ(posted[0].reason, EndReason::kQueueFull);

    q.Close();
    EXPECT_EQ(QueueJsonOrEnd(q, E, R"({"type":"ready"})", 0, stop, post), net::PushResult::kClosed);
    q.Open(E + 1);
    EXPECT_EQ(QueueJsonOrEnd(q, E + 1, R"({"type":"hello"})", 0, stop, post), net::PushResult::kQueued);
    EXPECT_EQ(deaths.size(), 1u);
    EXPECT_EQ(posted.size(), 1u);
}

// A violation's control queue stays closed for the flush until the next pair opens it, and the
// next control link's send task starts before that (reviews 151/152 Important 1). The new link
// must not report the last pair's flush (its one notice would be spent) ...
TEST(TxLoop, TheLastPairsFlushIsNotReportedByTheNextLink) {
    constexpr uint64_t E2 = E + 1;
    Fake f;
    LinkPhase phase;
    phase.OnResultPosted();
    net::SendQueue q(net::kCtrlLimits);
    q.Open(E);
    q.CloseForFlush();  // the last pair: flushed and ended, never opened again
    TxLoop tx(LinkSide::kCtrl, E2, &q, &phase, f.Ports());
    EXPECT_TRUE(tx.RunOnce());
    EXPECT_TRUE(f.posted.empty());
    q.Open(E2);
    ASSERT_EQ(q.Push(E2, net::ElemKind::kJson, R"({"type":"abort","state":"done"})", f.now), net::PushResult::kQueued);
    q.CloseForFlush();
    EXPECT_TRUE(tx.RunOnce());
    EXPECT_TRUE(tx.RunOnce());
    ASSERT_EQ(f.posted.size(), 1u);
    EXPECT_EQ(f.posted[0].kind, InKind::kCtrlFlushed);
    EXPECT_EQ(f.posted[0].e, E2);
}

// ... nor send what the last pair left in it: the link's first JSON is its own hello, seq 1 with
// its own E (contract S8).
TEST(TxLoop, TheLastPairsJsonIsNotSentOnTheNextLink) {
    constexpr uint64_t E2 = E + 1;
    Fake f;
    LinkPhase phase;
    phase.OnResultPosted();
    net::SendQueue q(net::kCtrlLimits);
    q.Open(E);
    ASSERT_EQ(q.Push(E, net::ElemKind::kJson, R"({"type":"abort","state":"done","req_id":"x"})", f.now),
              net::PushResult::kQueued);
    q.CloseForFlush();  // the flush was cut short: the done is still in the queue
    TxLoop tx(LinkSide::kCtrl, E2, &q, &phase, f.Ports());
    EXPECT_TRUE(tx.RunOnce());
    EXPECT_TRUE(f.wire.empty());
    EXPECT_TRUE(f.posted.empty());
    EXPECT_EQ(tx.stats().stale, 1u);
    q.Open(E2);
    ASSERT_EQ(q.Push(E2, net::ElemKind::kJson, R"({"type":"hello"})", f.now), net::PushResult::kQueued);
    EXPECT_TRUE(tx.RunOnce());
    const auto frames = f.Frames();
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(Num(frames[0].payload, "fw_epoch"), E2);
    EXPECT_EQ(Num(frames[0].payload, "seq"), 1u);
}
