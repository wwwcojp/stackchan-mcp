// StackChan FW-A2 §6.1: played_after_stop accounting.
#include "audio_origin.h"

#include <algorithm>

namespace stackchan::audio {

void PlayedAfterStop::OnStop(uint32_t serial) {
    for (uint32_t s = latest_ + 1; s <= serial; s++) counts_[s % kSlots] = 0;
    latest_ = std::max(latest_, serial);
}

void PlayedAfterStop::OnWriteEnd(const Origin& o, uint32_t serial_at_end) {
    if (!o.server) return;
    for (uint32_t s = o.accept_serial + 1; s <= serial_at_end; s++) {
        if (s + kSlots <= latest_) {
            overflow_++;
            continue;
        }
        uint32_t& c = counts_[s % kSlots];
        c++;
        max_ = std::max(max_, c);
    }
}

uint32_t PlayedAfterStop::CountFor(uint32_t serial) const {
    if (serial == 0 || serial > latest_ || serial + kSlots <= latest_) return 0;
    return counts_[serial % kSlots];
}

}  // namespace stackchan::audio
