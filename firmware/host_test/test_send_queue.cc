// StackChan FW-A2 §4.1: the send queue.
#include <gtest/gtest.h>

#include <chrono>

#include <string>
#include <thread>

#include "send_queue.h"

using namespace stackchan::net;

namespace {
constexpr uint64_t E = 65536ull * 65536 + 1;  // above 2^32 (design §3.1)
// the next payload, or "<none>" (a test must fail, not crash, when the queue is empty)
std::string Next(SendQueue& q) {
    const auto x = q.Pop(0);
    return x.has_value() ? x->payload : "<none>";
}
}

TEST(SendQueue, ClosedUntilOpenedAndOnlyForItsPair) {
    SendQueue q(kAudioLimits);
    EXPECT_EQ(q.Push(E, ElemKind::kJson, "a", 0), PushResult::kClosed);
    q.Open(E);
    EXPECT_FALSE(q.Drained());  // open and empty is not "flushed"
    EXPECT_EQ(q.Push(E + 1, ElemKind::kJson, "a", 0), PushResult::kClosed);
    EXPECT_EQ(q.Push(E & 0xFFFFFFFFull, ElemKind::kJson, "a", 0), PushResult::kClosed);
    EXPECT_EQ(q.Push(0, ElemKind::kJson, "a", 0), PushResult::kClosed);
    EXPECT_EQ(q.Push(E, ElemKind::kJson, "a", 5), PushResult::kQueued);
    EXPECT_EQ(q.Stats().rejected_closed, 4u);
    const auto x = q.Pop(0);
    ASSERT_TRUE(x.has_value());
    EXPECT_EQ(x->e, E);
    EXPECT_EQ(x->payload, "a");
    EXPECT_EQ(x->deadline_us, 5 + kSendDeadlineUs);  // one deadline from the time it was queued
}

TEST(SendQueue, CloseDropsEverythingAndRejects) {
    SendQueue q(kAudioLimits);
    q.Open(E);
    q.Push(E, ElemKind::kJson, "a", 0);
    q.Close();
    EXPECT_FALSE(q.Pop(0).has_value());
    EXPECT_EQ(q.Push(E, ElemKind::kJson, "b", 0), PushResult::kClosed);
    EXPECT_TRUE(q.Drained());
    q.Open(E + 1);  // the next pair starts empty
    EXPECT_EQ(q.Push(E + 1, ElemKind::kJson, "c", 0), PushResult::kQueued);
    EXPECT_EQ(Next(q), "c");
}

TEST(SendQueue, FlushKeepsTheQueuedDoneButRejectsNewOnes) {
    SendQueue q(kCtrlLimits);
    q.Open(E);
    q.Push(E, ElemKind::kJson, "done", 0);
    q.CloseForFlush();
    EXPECT_FALSE(q.Drained());
    EXPECT_EQ(q.Push(E, ElemKind::kJson, "late", 0), PushResult::kClosed);
    EXPECT_EQ(Next(q), "done");
    EXPECT_TRUE(q.Drained());
    EXPECT_FALSE(q.Pop(0).has_value());
}

TEST(SendQueue, MicDropsTheOldestAndKeepsRoomForJson) {
    SendQueue q(kAudioLimits);
    q.Open(E);
    EXPECT_EQ(q.Push(E, ElemKind::kJson, "first", 0), PushResult::kQueued);
    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(q.Push(E, ElemKind::kMic, "m" + std::to_string(i), 0), PushResult::kQueued);
    }
    EXPECT_EQ(q.Push(E, ElemKind::kMic, "m16", 0), PushResult::kQueuedDroppedOldMic);
    EXPECT_EQ(q.Stats().mic, 16u);
    EXPECT_EQ(q.Stats().mic_dropped, 1u);
    // JSON still fits (48 items, 16 of them mic at most)
    for (int i = 0; i < 31; i++) EXPECT_EQ(q.Push(E, ElemKind::kJson, "j", 0), PushResult::kQueued);
    EXPECT_EQ(q.Push(E, ElemKind::kJson, "j", 0), PushResult::kFull);  // a full queue ends the pair
    EXPECT_EQ(Next(q), "first");  // a JSON is never dropped for a mic frame
    EXPECT_EQ(Next(q), "m1");     // m0 was dropped, the order is kept
}

TEST(SendQueue, MicMakesRoomByBytesAndIsDroppedWhenNoMicIsLeft) {
    SendQueue q(Limits{8, 100, 4});
    q.Open(E);
    EXPECT_EQ(q.Push(E, ElemKind::kMic, std::string(40, 'm'), 0), PushResult::kQueued);
    EXPECT_EQ(q.Push(E, ElemKind::kJson, std::string(50, 'j'), 0), PushResult::kQueued);
    EXPECT_EQ(q.Push(E, ElemKind::kMic, std::string(40, 'n'), 0), PushResult::kQueuedDroppedOldMic);
    EXPECT_EQ(q.Stats().bytes, 90u);
    // 60 bytes would not fit even without any mic frame: the queued one is kept
    EXPECT_EQ(q.Push(E, ElemKind::kMic, std::string(60, 'o'), 0), PushResult::kDroppedMic);
    EXPECT_EQ(q.Stats().mic, 1u);
    EXPECT_EQ(q.Stats().mic_dropped, 2u);
    EXPECT_EQ(q.Push(E, ElemKind::kJson, std::string(11, 'k'), 0), PushResult::kFull);
    EXPECT_EQ(q.Push(E, ElemKind::kJson, std::string(10, 'k'), 0), PushResult::kQueued);
}

TEST(SendQueue, ControlQueueTakesNoMic) {
    SendQueue q(kCtrlLimits);
    q.Open(E);
    EXPECT_EQ(q.Push(E, ElemKind::kMic, "m", 0), PushResult::kDroppedMic);
    EXPECT_EQ(q.Stats().items, 0u);
}

TEST(SendQueue, PairGoesInTogetherOrNotAtAll) {
    SendQueue q(Limits{3, 1000, 1});
    q.Open(E);
    q.Push(E, ElemKind::kJson, "x", 0);
    q.Push(E, ElemKind::kJson, "y", 0);
    EXPECT_EQ(q.PushPair(E, "abort", "listen", 0), PushResult::kFull);
    EXPECT_EQ(q.Stats().items, 2u);  // neither went in
    q.Pop(0);
    EXPECT_EQ(q.PushPair(E, "abort", "listen", 7), PushResult::kQueued);
    EXPECT_EQ(Next(q), "y");
    const auto a = q.Pop(0);
    const auto b = q.Pop(0);
    ASSERT_TRUE(a.has_value() && b.has_value());
    EXPECT_EQ(a->payload, "abort");
    EXPECT_EQ(b->payload, "listen");
    EXPECT_EQ(a->deadline_us, b->deadline_us);
    EXPECT_EQ(q.PushPair(E + 1, "abort", "listen", 0), PushResult::kClosed);
}

TEST(SendQueue, PopWaitsAndWakeUnblocksIt) {
    SendQueue q(kAudioLimits);
    q.Open(E);
    EXPECT_FALSE(q.Pop(1000).has_value());  // times out
    std::thread t([&q] { q.Wake(); });
    EXPECT_FALSE(q.Pop(5'000'000).has_value());  // woken long before the timeout
    t.join();
    std::thread p([&q] { q.Push(E, ElemKind::kJson, "late", 0); });
    const auto x = q.Pop(5'000'000);
    p.join();
    ASSERT_TRUE(x.has_value());
    EXPECT_EQ(x->payload, "late");
}

TEST(SendQueue, StatsTrackTheSmallestFreeRoom) {
    SendQueue q(Limits{4, 1000, 4});
    q.Open(E);
    for (int i = 0; i < 3; i++) q.Push(E, ElemKind::kJson, "j", 0);
    q.Pop(0);
    EXPECT_EQ(q.Stats().min_free_items, 1u);
}

// Follow-up 3 (Codex review 145 Minor 1): a closed queue never keeps Pop waiting. Empty after a
// flush, Pop returns at once; a Pop waiting on an empty open queue returns when the queue is
// closed (for a flush or for good), not at its timeout.
TEST(SendQueue, PopNeverWaitsOnAClosedQueue) {
    using namespace std::chrono;
    SendQueue q(kCtrlLimits);
    q.Open(7);
    q.CloseForFlush();
    auto t0 = steady_clock::now();
    EXPECT_FALSE(q.Pop(5'000'000).has_value());
    EXPECT_LT(steady_clock::now() - t0, milliseconds(500));

    for (bool flush : {true, false}) {
        SendQueue r(kCtrlLimits);
        r.Open(7);
        std::thread closer([&] {
            std::this_thread::sleep_for(milliseconds(100));
            if (flush) {
                r.CloseForFlush();
            } else {
                r.Close();
            }
        });
        t0 = steady_clock::now();
        EXPECT_FALSE(r.Pop(5'000'000).has_value()) << flush;
        EXPECT_LT(steady_clock::now() - t0, milliseconds(2000)) << flush;
        closer.join();
    }
}

// FlushDone: closed for a flush and nothing left (the control send task's kCtrlFlushed). A queue
// closed for good is drained but not flushed.
TEST(SendQueue, FlushDoneOnlyAfterAFlush) {
    SendQueue q(kCtrlLimits);
    q.Open(7);
    ASSERT_EQ(q.Push(7, ElemKind::kJson, "done", 0), PushResult::kQueued);
    q.CloseForFlush();
    EXPECT_FALSE(q.FlushDone());
    ASSERT_TRUE(q.Pop(0).has_value());
    EXPECT_TRUE(q.FlushDone());
    EXPECT_TRUE(q.Drained());

    SendQueue r(kCtrlLimits);
    r.Open(7);
    r.Close();
    EXPECT_TRUE(r.Drained());
    EXPECT_FALSE(r.FlushDone());
    r.Open(8);
    EXPECT_FALSE(r.FlushDone());
}
