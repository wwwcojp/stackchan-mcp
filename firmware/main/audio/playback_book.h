// StackChan FW-A2 §2.2, §2.4 (AutoStop), §6.1: the AudioService's bookkeeping of queued audio,
// kept pure for host tests. AudioService (plan 2B) owns one and calls it under
// audio_queue_mutex_ only. It joins:
// - OriginCounter: whether a stop must clear the queues (server audio accepted since the
//   previous stop, not yet marked "write started"; local sounds never are a reason);
// - PlayedAfterStop: frames written after a stop that covers them (contract §5.1);
// - the copy of the gate's stop_serial and the playback generation (bumped by every clear);
// - the server audio in flight: decode queue, decoding, playback queue and output, until
//   OutputData() returns. AutoStop waits until none is left (design §2.4, v10).
// Every server packet leaves the pipeline exactly once: cleared from a queue, dropped by the
// decoder, dropped by the generation check, or written. Each of those entries tells whether a
// pending drain request is met now ("drained": post PlaybackDrained in the same lock section).
// MarkWriteStart answers two things, so it returns both by name (Codex review 143 Important 1).
#pragma once

#include <cstdint>

#include "audio_origin.h"

namespace stackchan::audio {

class PlaybackBook {
public:
    // A server packet really entered the decode queue (call after the push succeeded; a full
    // queue drops it and must not count, inventory §6.13). Returns its origin.
    // OnDecodeDropped, OnWriteEnd and OnCleared return "drained"; OnStop returns whether to
    // clear; RequestDrain whether to post PlaybackDrained right away.
    Origin OnServerQueued();
    static Origin Local() { return Origin{false, 0}; }

    // The decoder dropped an item taken from the decode queue at generation `item_gen`: a
    // decode failure (no decoder included) or the generation check. Only a decode failure of
    // the current generation uncounts it in the OriginCounter (plan 1 decision 11).
    bool OnDecodeDropped(const Origin& o, uint32_t item_gen);

    // The output task, under the lock right before OutputData(): the generation check and the
    // write-start mark in one section. `write`: call OutputData(). Not `write`: doomed by a
    // clear (dropped). `drained`: a pending drain request is met by that drop (never on `write`:
    // the item is still in flight until OnWriteEnd).
    struct Mark {
        bool write = false;
        bool drained = false;
    };
    Mark MarkWriteStart(const Origin& o, uint32_t item_gen);

    // OutputData() returned; under the lock again. Counts played_after_stop with the stop
    // serial read here.
    bool OnWriteEnd(const Origin& o);

    // A stop: the gate's stop_serial is now `serial`. Returns whether the queues must be
    // cleared (then call OnCleared after emptying them).
    bool OnStop(uint32_t serial);
    // The queues were emptied (a stop that must clear, or ClearForListening / the start from
    // Idle): bump the generation; `server_removed` server items left the pipeline.
    bool OnCleared(uint32_t server_removed);

    // AutoStop asks to be told when no server audio is in flight. True: post PlaybackDrained
    // now. Otherwise a later entry returns true once.
    bool RequestDrain();

    uint32_t generation() const { return generation_; }
    uint32_t stop_serial() const { return serial_; }
    uint32_t server_in_flight() const { return in_flight_; }
    bool drain_pending() const { return drain_pending_; }
    const OriginCounter& counter() const { return counter_; }
    const PlayedAfterStop& played() const { return played_; }

private:
    bool Left();  // a server item left the pipeline

    OriginCounter counter_;
    PlayedAfterStop played_;
    uint32_t serial_ = 0;
    uint32_t generation_ = 0;
    uint32_t in_flight_ = 0;
    bool drain_pending_ = false;
};

}  // namespace stackchan::audio
