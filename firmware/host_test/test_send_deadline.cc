// StackChan FW-A2 §4.1, §7.1-6: one deadline over partial sends and waits.
#include <gtest/gtest.h>

#include <vector>

#include "send_deadline.h"

using namespace stackchan::net;

namespace {

struct FakeSocket {
    int64_t clock = 0;
    std::vector<int> script;  // per call: >0 bytes sent (takes 10 ms), 0 = slice timed out, -1 error
    size_t calls = 0;
    std::vector<int64_t> timeouts;
    int Send(const uint8_t*, size_t len, int64_t timeout_us) {
        timeouts.push_back(timeout_us);
        const int r = calls < script.size() ? script[calls] : 0;
        calls++;
        if (r > 0) {
            clock += 10'000;
            return static_cast<int>(std::min<size_t>(static_cast<size_t>(r), len));
        }
        clock += timeout_us;  // a timed-out slice consumes its whole timeout
        return r;
    }
};

SendOutcome SendWith(FakeSocket& f, size_t len, int64_t deadline, bool* stop = nullptr) {
    std::vector<uint8_t> buf(len, 0xAB);
    return SendAll(
        buf.data(), buf.size(), deadline, [&] { return f.clock; },
        [&](const uint8_t* d, size_t n, int64_t t) { return f.Send(d, n, t); },
        [&] { return stop != nullptr && *stop; });
}

}  // namespace

TEST(SendDeadline, PartialSendsComplete) {
    FakeSocket f;
    f.script = {100, 0, 100, 56};
    SendOutcome o = SendWith(f, 256, 2'000'000);
    EXPECT_EQ(o.result, SendResult::kOk);
    EXPECT_EQ(o.sent, 256u);
}

TEST(SendDeadline, StuckSendMissesTheDeadlineWithoutOverrunningIt) {
    FakeSocket f;
    f.script = {100};  // then every slice times out
    SendOutcome o = SendWith(f, 256, 2'000'000);
    EXPECT_EQ(o.result, SendResult::kDeadline);
    EXPECT_EQ(o.sent, 100u);
    EXPECT_LE(f.clock, 2'000'000);  // never past the deadline
    for (int64_t t : f.timeouts) {
        EXPECT_GE(t, 1000);  // never 0 (lwIP: block forever)
        EXPECT_LE(t, kSliceUs);
    }
}

TEST(SendDeadline, TheLastSliceIsTheRemainingTime) {
    FakeSocket f;
    f.clock = 1'950'000;
    SendOutcome o = SendWith(f, 10, 2'000'000);
    EXPECT_EQ(o.result, SendResult::kDeadline);
    ASSERT_FALSE(f.timeouts.empty());
    EXPECT_EQ(f.timeouts.front(), 50'000);
}

TEST(SendDeadline, UnderOneMillisecondLeftDoesNotSend) {
    EXPECT_EQ(NextSliceUs(1'000'000, 999'500), 0);
    EXPECT_EQ(NextSliceUs(1'000'000, 999'000), 1'000);
    EXPECT_EQ(NextSliceUs(1'000'000, 0), kSliceUs);
    FakeSocket f;
    f.clock = 999'500;
    EXPECT_EQ(SendWith(f, 10, 1'000'000).result, SendResult::kDeadline);
    EXPECT_EQ(f.calls, 0u);
}

TEST(SendDeadline, StopRequestIsSeenBetweenSlices) {
    FakeSocket f;
    bool stop = false;
    f.script = {0, 0, 0};
    std::vector<uint8_t> buf(10);
    int calls = 0;
    SendOutcome o = SendAll(
        buf.data(), buf.size(), 2'000'000, [&] { return f.clock; },
        [&](const uint8_t* d, size_t n, int64_t t) {
            if (++calls == 2) stop = true;
            return f.Send(d, n, t);
        },
        [&] { return stop; });
    EXPECT_EQ(o.result, SendResult::kStopped);
    EXPECT_EQ(calls, 2);
}

TEST(SendDeadline, ALastSendThatReturnsAfterTheDeadlineIsAMiss) {
    // Codex review 131 Important 6
    FakeSocket f;
    f.clock = 1'999'000;
    f.script = {10};  // sends everything, but takes 10 ms
    SendOutcome o = SendWith(f, 10, 2'000'000);
    EXPECT_EQ(o.result, SendResult::kDeadline);
    EXPECT_EQ(o.sent, 10u);
}

TEST(SendDeadline, ErrorEndsAtOnce) {
    FakeSocket f;
    f.script = {50, -1};
    SendOutcome o = SendWith(f, 100, 2'000'000);
    EXPECT_EQ(o.result, SendResult::kError);
    EXPECT_EQ(o.sent, 50u);
}
