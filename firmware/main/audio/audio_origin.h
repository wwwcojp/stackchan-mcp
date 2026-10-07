// StackChan FW-A2 §2.2 and §6.1: the origin of queued audio, kept pure for host tests.
// OriginCounter decides whether a stop must clear the queues (only server audio accepted
// since the previous stop and not yet marked "write started"; local sounds are never a reason).
// PlayedAfterStop counts frames written after a stop that covers them (contract §5.1).
// AudioService (FW-A2 plan 2) calls these under audio_queue_mutex_.
#pragma once

#include <array>
#include <cstdint>

namespace stackchan::audio {

struct Origin {
    bool server = false;         // false: a local sound (popup etc.)
    uint32_t accept_serial = 0;  // the gate's stop_serial when the server audio was queued
};

class OriginCounter {
public:
    // A server packet was queued under the current stop serial.
    void OnServerQueued() { pending_++; }
    // The output task marked an item "write started" (the generation check passed): it no
    // longer counts. An item dropped by the generation check is NOT reported: the clear that
    // moved the generation already zeroed the count (reporting it would undercount the items
    // queued after the clear, and a later stop would leave them playing).
    void OnMarked(const Origin& o, uint32_t current_serial) {
        if (o.server && o.accept_serial == current_serial && pending_ > 0) pending_--;
    }
    // A queued item of the current generation was dropped before writing (decode failure,
    // no decoder): it no longer counts. Not for items dropped by the generation check.
    void OnDecodeDropped(const Origin& o, uint32_t current_serial) { OnMarked(o, current_serial); }
    // The queues were cleared (ResetDecoder): nothing pending.
    void OnCleared() { pending_ = 0; }
    // A stop is about to bump the serial. Returns whether the queues must be cleared.
    bool OnStop() {
        const bool clear = pending_ > 0;
        pending_ = 0;  // the next serial starts with nothing
        return clear;
    }
    uint32_t pending() const { return pending_; }

private:
    uint32_t pending_ = 0;
};

class PlayedAfterStop {
public:
    static constexpr uint32_t kSlots = 8;
    // A stop happened; the serial is now `serial` (>= 1).
    void OnStop(uint32_t serial);
    // A frame finished writing; `serial_at_end` was read after OutputData() returned.
    // Counted for every stop in (accept_serial, serial_at_end].
    void OnWriteEnd(const Origin& o, uint32_t serial_at_end);
    uint32_t CountFor(uint32_t serial) const;  // 0 when out of the window
    uint32_t last() const { return CountFor(latest_); }
    uint32_t max() const { return max_; }
    uint32_t overflow() const { return overflow_; }  // counts for stops already out of the window

private:
    std::array<uint32_t, kSlots> counts_{};
    uint32_t latest_ = 0;
    uint32_t max_ = 0;
    uint32_t overflow_ = 0;
};

}  // namespace stackchan::audio
