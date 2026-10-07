// StackChan FW-A2 §4.1-4.2: send a whole buffer under one deadline shared by every partial
// send (F2). The shell sets SO_SNDTIMEO to each slice before calling `send`. lwIP treats a
// SO_SNDTIMEO of 0 as "block forever", so a slice is never 0: under 1 ms left is a deadline miss.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace stackchan::net {

constexpr int64_t kSliceUs = 200'000;  // check the stop request at least every 200 ms

// Returns bytes sent (> 0), 0 when the slice timed out (EAGAIN), or -1 on an error.
using SendFn = std::function<int(const uint8_t* data, size_t len, int64_t timeout_us)>;
using NowFn = std::function<int64_t()>;
using StopFn = std::function<bool()>;

enum class SendResult { kOk, kDeadline, kStopped, kError };

struct SendOutcome {
    SendResult result = SendResult::kOk;
    size_t sent = 0;  // bytes sent before returning (a partial frame ends the pair)
};

// The timeout for the next send: min(remaining, slice), or 0 when under 1 ms remains
// (the caller must treat 0 as a deadline miss and not call send).
int64_t NextSliceUs(int64_t deadline_us, int64_t now_us);

SendOutcome SendAll(const uint8_t* data, size_t len, int64_t deadline_us, const NowFn& now,
                    const SendFn& send, const StopFn& stop_requested);

}  // namespace stackchan::net
