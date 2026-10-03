#pragma once

// StackChan FW-A: wall-clock time from the gateway hello (and the OTA reply).
// See stackchan-works docs/superpowers/specs/2026-10-02-fw-a-design.md §2.6.
//
// Wire contract: {"timestamp": <UTC Unix time in ms>, "timezone_offset": <minutes east of UTC,
// optional, JST = +540>}. The system clock is set to UTC; the offset only becomes the POSIX TZ
// used by localtime() for display. (The upstream OTA code used to shift the clock itself.)

#include <cstdint>
#include <string>
#include <sys/time.h>

struct cJSON;

namespace stackchan {

struct ServerTime {
    int64_t utc_ms = 0;
    int32_t offset_min = 0;
};

// Pure. False (and *out untouched) unless server_time is an object with a numeric "timestamp" in
// [0, 2^63) ms (a fractional part is truncated) and, if present, an integral "timezone_offset"
// within [-720, +840] minutes.
bool ParseServerTime(const cJSON* server_time, ServerTime* out);

// Pure. Minutes east of UTC -> POSIX TZ (the sign is inverted in POSIX):
// +540 -> "<+09>-9", +330 -> "<+0530>-5:30", -210 -> "<-0330>3:30", 0 -> "UTC0".
std::string PosixTzForOffset(int32_t offset_min);

// The two OS calls, injectable for host tests. Both return 0 on success.
struct TimeOps {
    int (*set_tz)(const char* tz);               // setenv("TZ") + tzset()
    int (*set_clock)(const struct timeval* tv);  // settimeofday()
};
const TimeOps& SystemTimeOps();

enum class ApplyResult { kOk, kTzFailed, kClockFailed };
const char* ApplyResultText(ApplyResult result);

// TZ first, then the clock (UTC). kTzFailed: nothing changed (the clock is not touched).
// kClockFailed: TZ has changed but the clock keeps its previous value - an unset clock stays
// unset (the display keeps reporting "time not set"); a set clock keeps running and is shown in
// the new TZ.
ApplyResult ApplyServerTime(const ServerTime& time, const TimeOps& ops = SystemTimeOps());

// gettimeofday() in ms, for the log line that the acceptance compares with what was sent.
int64_t ReadClockUtcMs();

}  // namespace stackchan
