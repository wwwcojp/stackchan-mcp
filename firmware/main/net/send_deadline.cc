// StackChan FW-A2 §4.1: SendAll under one deadline.
#include "send_deadline.h"

#include <algorithm>

namespace stackchan::net {

int64_t NextSliceUs(int64_t deadline_us, int64_t now_us) {
    const int64_t remaining = deadline_us - now_us;
    if (remaining < 1000) return 0;
    return std::min(remaining, kSliceUs);
}

SendOutcome SendAll(const uint8_t* data, size_t len, int64_t deadline_us, const NowFn& now,
                    const SendFn& send, const StopFn& stop_requested) {
    SendOutcome o;
    while (o.sent < len) {
        if (stop_requested()) {
            o.result = SendResult::kStopped;
            return o;
        }
        const int64_t slice = NextSliceUs(deadline_us, now());
        if (slice == 0) {
            o.result = SendResult::kDeadline;
            return o;
        }
        const int n = send(data + o.sent, len - o.sent, slice);
        if (n > 0) {
            o.sent += static_cast<size_t>(n);
        } else if (n < 0) {
            o.result = SendResult::kError;
            return o;
        }
    }
    // the last send may return after the deadline: that is a miss, not a success (F2)
    o.result = now() > deadline_us ? SendResult::kDeadline : SendResult::kOk;
    return o;
}

}  // namespace stackchan::net
