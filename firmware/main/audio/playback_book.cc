// StackChan FW-A2: the AudioService's bookkeeping (see playback_book.h).
#include "playback_book.h"

namespace stackchan::audio {

bool PlaybackBook::Left() {
    if (in_flight_ > 0) in_flight_--;
    if (in_flight_ == 0 && drain_pending_) {
        drain_pending_ = false;
        return true;
    }
    return false;
}

Origin PlaybackBook::OnServerQueued() {
    counter_.OnServerQueued();
    in_flight_++;
    return Origin{true, serial_};
}

bool PlaybackBook::OnDecodeDropped(const Origin& o, uint32_t item_gen) {
    if (item_gen == generation_) counter_.OnDecodeDropped(o, serial_);
    return o.server && Left();
}

PlaybackBook::Mark PlaybackBook::MarkWriteStart(const Origin& o, uint32_t item_gen) {
    Mark m;
    if (item_gen != generation_) {  // doomed by a clear: not reported to the counter
        m.drained = o.server && Left();
        return m;
    }
    counter_.OnMarked(o, serial_);
    m.write = true;
    return m;
}

bool PlaybackBook::OnWriteEnd(const Origin& o) {
    played_.OnWriteEnd(o, serial_);
    return o.server && Left();
}

bool PlaybackBook::OnStop(uint32_t serial) {
    const bool clear = counter_.OnStop();
    serial_ = serial;
    played_.OnStop(serial);
    return clear;
}

bool PlaybackBook::OnCleared(uint32_t server_removed) {
    generation_++;
    counter_.OnCleared();
    bool met = false;
    for (uint32_t i = 0; i < server_removed; i++) met = Left() || met;
    return met;
}

bool PlaybackBook::RequestDrain() {
    if (in_flight_ == 0) {
        drain_pending_ = false;
        return true;
    }
    drain_pending_ = true;
    return false;
}

}  // namespace stackchan::audio
