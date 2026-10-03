// StackChan FW-A T4 (design §2.6): server_time from the gateway hello.
#include <gtest/gtest.h>

#include <cJSON.h>

#include <cstdlib>
#include <ctime>
#include <string>

#include "server_time.h"

using stackchan::ApplyResult;
using stackchan::ApplyServerTime;
using stackchan::ParseServerTime;
using stackchan::PosixTzForOffset;
using stackchan::ServerTime;
using stackchan::TimeOps;

namespace {

bool Parse(const char* json, ServerTime* out) {
    cJSON* root = cJSON_Parse(json);
    bool ok = ParseServerTime(root, out);
    cJSON_Delete(root);
    return ok;
}

}  // namespace

TEST(ParseServerTime, ReadsUtcMillisecondsAndOffset) {
    ServerTime t{};
    ASSERT_TRUE(Parse(R"({"timestamp": 1790990400123, "timezone_offset": 540})", &t));
    EXPECT_EQ(t.utc_ms, 1790990400123LL);
    EXPECT_EQ(t.offset_min, 540);
}

TEST(ParseServerTime, OffsetIsOptionalAndDefaultsToUtc) {
    ServerTime t{};
    ASSERT_TRUE(Parse(R"({"timestamp": 1790990400000})", &t));
    EXPECT_EQ(t.offset_min, 0);
}

TEST(ParseServerTime, AcceptsNegativeAndFractionalHourOffsets) {
    ServerTime t{};
    ASSERT_TRUE(Parse(R"({"timestamp": 0, "timezone_offset": -210})", &t));
    EXPECT_EQ(t.offset_min, -210);
    ASSERT_TRUE(Parse(R"({"timestamp": 0, "timezone_offset": 330})", &t));
    EXPECT_EQ(t.offset_min, 330);
}

TEST(ParseServerTime, RejectsMissingOrInvalidFields) {
    ServerTime t{};
    EXPECT_FALSE(ParseServerTime(nullptr, &t));
    EXPECT_FALSE(Parse(R"("not an object")", &t));
    EXPECT_FALSE(Parse(R"({"timezone_offset": 540})", &t));             // no timestamp
    EXPECT_FALSE(Parse(R"({"timestamp": "1790990400000"})", &t));       // string
    EXPECT_FALSE(Parse(R"({"timestamp": -1})", &t));                    // before 1970
    EXPECT_FALSE(Parse(R"({"timestamp": 0, "timezone_offset": "540"})", &t));
    EXPECT_FALSE(Parse(R"({"timestamp": 0, "timezone_offset": 541.5})", &t));  // not whole minutes
    EXPECT_FALSE(Parse(R"({"timestamp": 0, "timezone_offset": 900})", &t));    // beyond +14:00
    EXPECT_FALSE(Parse(R"({"timestamp": 0, "timezone_offset": -780})", &t));   // beyond -12:00
}

TEST(ParseServerTime, RejectsTimestampsThatDoNotFitInt64AndLeavesOutUntouched) {
    ServerTime t{};
    t.utc_ms = 7;
    t.offset_min = 7;
    EXPECT_FALSE(Parse(R"({"timestamp": 1e100})", &t));
    EXPECT_FALSE(Parse(R"({"timestamp": 9223372036854775808})", &t));  // 2^63
    EXPECT_FALSE(Parse(R"({"timestamp": 0, "timezone_offset": 900})", &t));
    EXPECT_EQ(t.utc_ms, 7);
    EXPECT_EQ(t.offset_min, 7);
    ASSERT_TRUE(Parse(R"({"timestamp": 9200000000000000000})", &t));  // below 2^63
    EXPECT_EQ(t.utc_ms, 9200000000000000000LL);
}

TEST(ParseServerTime, TruncatesAFractionOfAMillisecond) {
    ServerTime t{};
    ASSERT_TRUE(Parse(R"({"timestamp": 1500.7})", &t));
    EXPECT_EQ(t.utc_ms, 1500);
}

namespace {
// Fake OS calls: record what ApplyServerTime asked for and fail on demand.
int g_tz_result = 0;
int g_clock_result = 0;
int g_tz_calls = 0;
int g_clock_calls = 0;
std::string g_tz;
struct timeval g_tv {};

int FakeSetTz(const char* tz) {
    ++g_tz_calls;
    g_tz = tz;
    return g_tz_result;
}

int FakeSetClock(const struct timeval* tv) {
    ++g_clock_calls;
    g_tv = *tv;
    return g_clock_result;
}

const TimeOps kFake{FakeSetTz, FakeSetClock};

void ResetFake(int tz_result, int clock_result) {
    g_tz_result = tz_result;
    g_clock_result = clock_result;
    g_tz_calls = 0;
    g_clock_calls = 0;
    g_tz.clear();
    g_tv = {};
}
}  // namespace

TEST(ApplyServerTime, SetsTzThenTheClockInUtc) {
    ResetFake(0, 0);
    EXPECT_EQ(ApplyServerTime(ServerTime{1790990400123LL, 540}, kFake), ApplyResult::kOk);
    EXPECT_EQ(g_tz, "<+09>-9");
    EXPECT_EQ(g_tv.tv_sec, 1790990400);  // UTC: the offset is not added to the clock
    EXPECT_EQ(g_tv.tv_usec, 123000);
}

TEST(ApplyServerTime, TzFailureLeavesTheClockAlone) {
    ResetFake(-1, 0);
    EXPECT_EQ(ApplyServerTime(ServerTime{1000, 540}, kFake), ApplyResult::kTzFailed);
    EXPECT_EQ(g_tz_calls, 1);
    EXPECT_EQ(g_clock_calls, 0);
}

TEST(ApplyServerTime, ClockFailureIsReported) {
    ResetFake(0, -1);
    EXPECT_EQ(ApplyServerTime(ServerTime{1000, 0}, kFake), ApplyResult::kClockFailed);
    EXPECT_EQ(g_tz, "UTC0");
    EXPECT_EQ(g_clock_calls, 1);
}

TEST(PosixTzForOffset, InvertsTheSignAndKeepsMinutes) {
    EXPECT_EQ(PosixTzForOffset(540), "<+09>-9");
    EXPECT_EQ(PosixTzForOffset(330), "<+0530>-5:30");
    EXPECT_EQ(PosixTzForOffset(-210), "<-0330>3:30");
    EXPECT_EQ(PosixTzForOffset(-300), "<-05>5");
    EXPECT_EQ(PosixTzForOffset(0), "UTC0");
}

TEST(PosixTzForOffset, LocaltimeShowsTheOffsetWhileTheClockStaysUtc) {
    // 2026-10-03 00:00:00 UTC
    const time_t utc = 1790985600;
    setenv("TZ", PosixTzForOffset(540).c_str(), 1);
    tzset();
    struct tm local {};
    localtime_r(&utc, &local);
    EXPECT_EQ(local.tm_hour, 9);
    EXPECT_EQ(local.tm_mday, 3);

    setenv("TZ", PosixTzForOffset(-210).c_str(), 1);
    tzset();
    localtime_r(&utc, &local);
    EXPECT_EQ(local.tm_mday, 2);
    EXPECT_EQ(local.tm_hour, 20);
    EXPECT_EQ(local.tm_min, 30);
}
