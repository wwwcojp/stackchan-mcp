// StackChan FW-A2 plan 2B-2a (design §2.2, §2.4, §6.1; plan 2A handoff 1-3, follow-up 4): the
// AudioService's two playback queues (decode and playback) and its PlaybackBook, apart from the
// ESP so the wiring is host-tested. The AudioService keeps its audio_queue_mutex_, its condition
// variable, the opus decoder and the I2S output; every entry here runs under that lock (the
// caller's lock is passed as proof) and the caller notifies its condition variable afterwards.
// Every server packet leaves exactly once (cleared, a decode failure, decoded after a clear,
// doomed at the write-start mark, or written), and the generation is the book's alone: a clear
// of any kind bumps it (follow-up 4). An entry that meets a pending AutoStop drain request posts
// PlaybackDrained in the same lock section (lock order audio_queue_mutex_ -> ui_queue_mutex_,
// design §1.2). Every clear also calls `on_cleared` under the lock (the AudioService resets the opus
// decoder and its server-AEC timestamps there, as ResetDecoder does today: audio_queue_mutex_ ->
// decoder_mutex_). Neither port may call back into the AudioService's queues or the gate.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "audio_items.h"
#include "playback_book.h"
#include "playback_gate.h"

namespace stackchan::audio {

using QueueLock = std::unique_lock<std::mutex>;

struct PipelineLimits {
    size_t decode = 40;   // MAX_DECODE_PACKETS_IN_QUEUE (2.4 s of 60 ms frames)
    size_t playback = 2;  // MAX_PLAYBACK_TASKS_IN_QUEUE
};

struct PipelineStats {
    uint32_t server_full = 0;    // server packets a full decode queue refused (never in the book)
    uint32_t local_full = 0;     // local packets a full decode queue refused (the popup that never waits)
    uint32_t decode_failed = 0;  // decode failures, a missing decoder included
    uint32_t stale_decoded = 0;  // decoded after a clear: dropped
    uint32_t stale_output = 0;   // taken by the output task before a clear: dropped at the mark
    uint32_t clears = 0;         // the queues were emptied (any reason)
};

class AudioPipeline {
public:
    AudioPipeline(PipelineLimits limits, std::function<void()> post_drained,
                  std::function<void()> on_cleared = nullptr);

    // ---- the decode queue ----
    // Server audio (the gate's push): stamped with its origin and counted by the book only when it
    // entered (inventory §6.13). False: the queue was full; the packet stays with the caller.
    bool PushServer(const QueueLock& lk, std::unique_ptr<AudioStreamPacket>& packet);
    // A local sound: the local origin. False: full; the packet stays with the caller.
    bool PushLocal(const QueueLock& lk, std::unique_ptr<AudioStreamPacket>& packet);
    bool DecodeQueueFull(const QueueLock& lk) const;

    // ---- the codec task ----
    bool CanDecode(const QueueLock& lk) const;  // a packet waits and the playback queue has room
    struct ToDecode {
        std::unique_ptr<AudioStreamPacket> packet;
        uint32_t gen = 0;  // the generation when it was taken
    };
    ToDecode TakeForDecode(const QueueLock& lk);
    // The codec task's one packet (Claude review 156 Important 2): `decode` runs without the lock
    // and returns the PCM, or nullptr when the decoder failed (no decoder included); then, under the
    // lock again, the PCM is queued (unless a clear came meanwhile) or the failure is told. `lk` is
    // held on entry and on return. True: queued for playback.
    bool DecodeOne(QueueLock& lk, const ToDecode& item,
                   const std::function<std::unique_ptr<AudioTask>(const AudioStreamPacket&)>& decode);
    // The decoder failed on it (no decoder included): it leaves.
    void DecodeFailed(const QueueLock& lk, const ToDecode& item);
    // Decoded into `task`: queued for playback with the packet's origin unless a clear came
    // meanwhile (then dropped). True: queued.
    bool Decoded(const QueueLock& lk, const ToDecode& item, std::unique_ptr<AudioTask> task);

    // ---- the output task ----
    bool CanOutput(const QueueLock& lk) const;
    struct ToOutput {
        std::unique_ptr<AudioTask> task;
        uint32_t gen = 0;
    };
    ToOutput TakeForOutput(const QueueLock& lk);
    // Right before OutputData(), the lock taken again: the generation check and the write-start
    // mark in one section. True: release the lock, OutputData(), take the lock, WriteEnd(). False:
    // a clear doomed it (dropped; a drain request it was the last of is met here).
    bool MarkWriteStart(const QueueLock& lk, const ToOutput& item);
    void WriteEnd(const QueueLock& lk, const Origin& origin);
    // The output task's one item (Claude review 156 Important 2): the mark, then `write` without the
    // lock (OutputData), then WriteEnd under the lock again. Nothing touches the book between the
    // two. `lk` is held on entry and on return. False: a clear doomed it (`write` not called).
    bool OutputOne(QueueLock& lk, const ToOutput& item, const std::function<void(AudioTask&)>& write);

    // ---- the gate (gate::AudioSink, under the gate lock) and the UiController ----
    // A stop: the gate's stop_serial is now `serial`. Empties both queues only when the book says
    // so; returns every item removed (local sounds included: dropped_ms).
    uint32_t Stop(const QueueLock& lk, uint32_t serial);
    void Clear(const QueueLock& lk);         // unconditional: the start from Idle, ClearForListening
    void RequestDrain(const QueueLock& lk);  // AutoStop (design §2.4)

    // ---- the AudioService ----
    // AudioService::Stop (FW-A's ResetDecoder used it too until plan 2B-2b): empty both queues.
    // Returns the items removed.
    uint32_t Reset(const QueueLock& lk);
    // EnableAudioTesting(false): the recording replaces the decode queue, as local sounds; the
    // playback queue is emptied too (a clear empties both: Claude review 156 Minor 1).
    void ReplaceDecodeQueue(const QueueLock& lk, std::deque<std::unique_ptr<AudioStreamPacket>> packets);
    bool Empty(const QueueLock& lk) const;  // both queues (FW-A's WaitForPlaybackQueueEmpty)
    size_t decode_size(const QueueLock& lk) const;
    size_t playback_size(const QueueLock& lk) const;

    const PlaybackBook& book(const QueueLock& lk) const;
    PipelineStats stats(const QueueLock& lk) const;

private:
    uint32_t ClearAll();
    void Post(bool drained);

    const PipelineLimits limits_;
    std::function<void()> post_drained_;
    std::function<void()> on_cleared_;
    std::deque<std::unique_ptr<AudioStreamPacket>> decode_;
    std::deque<std::unique_ptr<AudioTask>> playback_;
    PlaybackBook book_;
    PipelineStats stats_;
};

// The gate's AudioSink on the pipeline: takes the audio queue lock (gate_mutex_ ->
// audio_queue_mutex_, design §1.2) and notifies the AudioService's condition variable.
class PipelineSink : public gate::AudioSink {
public:
    PipelineSink(std::mutex* mutex, AudioPipeline* pipeline, std::function<void()> notify);
    uint32_t Stop(uint32_t serial) override;
    void Clear() override;

private:
    std::mutex* mutex_;
    AudioPipeline* pipeline_;
    std::function<void()> notify_;
};

}  // namespace stackchan::audio
