#include "server_time.h"

#include <cJSON.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace stackchan {

namespace {
constexpr int32_t kMinOffsetMin = -12 * 60;
constexpr int32_t kMaxOffsetMin = 14 * 60;
constexpr double kTwoTo63 = 9223372036854775808.0;  // first double that does not fit int64

int SystemSetTz(const char* tz) {
    if (setenv("TZ", tz, 1) != 0) {
        return -1;
    }
    tzset();
    return 0;
}

int SystemSetClock(const struct timeval* tv) {
    return settimeofday(tv, nullptr);
}
}  // namespace

bool ParseServerTime(const cJSON* server_time, ServerTime* out) {
    if (server_time == nullptr || !cJSON_IsObject(server_time) || out == nullptr) {
        return false;
    }
    const cJSON* timestamp = cJSON_GetObjectItemCaseSensitive(server_time, "timestamp");
    if (!cJSON_IsNumber(timestamp) || !std::isfinite(timestamp->valuedouble) ||
        timestamp->valuedouble < 0 || timestamp->valuedouble >= kTwoTo63) {
        return false;
    }
    int32_t offset_min = 0;
    const cJSON* offset = cJSON_GetObjectItemCaseSensitive(server_time, "timezone_offset");
    if (offset != nullptr) {
        if (!cJSON_IsNumber(offset)) {
            return false;
        }
        const double value = offset->valuedouble;
        if (value != std::floor(value) || value < kMinOffsetMin || value > kMaxOffsetMin) {
            return false;
        }
        offset_min = static_cast<int32_t>(value);
    }
    out->utc_ms = static_cast<int64_t>(timestamp->valuedouble);  // truncates a fraction of a ms
    out->offset_min = offset_min;
    return true;
}

std::string PosixTzForOffset(int32_t offset_min) {
    if (offset_min == 0) {
        return "UTC0";
    }
    const char east = offset_min > 0 ? '+' : '-';
    const int32_t magnitude = offset_min > 0 ? offset_min : -offset_min;
    const int hours = static_cast<int>(magnitude / 60);
    const int minutes = static_cast<int>(magnitude % 60);
    // POSIX TZ gives the offset to add to local time to reach UTC, so east of UTC is negative.
    const char* posix_sign = offset_min > 0 ? "-" : "";
    char buf[32];
    if (minutes == 0) {
        std::snprintf(buf, sizeof(buf), "<%c%02d>%s%d", east, hours, posix_sign, hours);
    } else {
        std::snprintf(buf, sizeof(buf), "<%c%02d%02d>%s%d:%02d", east, hours, minutes, posix_sign,
                      hours, minutes);
    }
    return buf;
}

const TimeOps& SystemTimeOps() {
    static const TimeOps ops{SystemSetTz, SystemSetClock};
    return ops;
}

const char* ApplyResultText(ApplyResult result) {
    switch (result) {
        case ApplyResult::kOk:
            return "ok";
        case ApplyResult::kTzFailed:
            return "setting TZ failed (clock unchanged)";
        case ApplyResult::kClockFailed:
            return "settimeofday failed (TZ changed, clock unchanged)";
    }
    return "?";
}

ApplyResult ApplyServerTime(const ServerTime& time, const TimeOps& ops) {
    if (ops.set_tz(PosixTzForOffset(time.offset_min).c_str()) != 0) {
        return ApplyResult::kTzFailed;
    }
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(time.utc_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((time.utc_ms % 1000) * 1000);
    if (ops.set_clock(&tv) != 0) {
        return ApplyResult::kClockFailed;
    }
    return ApplyResult::kOk;
}

int64_t ReadClockUtcMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

}  // namespace stackchan
