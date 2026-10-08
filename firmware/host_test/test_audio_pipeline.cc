// StackChan FW-A2 plan 2B-2a: the AudioService's playback queues and book (plan 2A handoff 1-3,
// follow-up 4). Each test runs the entries the codec task, the output task, the gate and the
// UiController call, in the order those tasks call them, under one std::mutex as the AudioService's
// audio_queue_mutex_.
#include <gtest/gtest.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "audio_pipeline.h"

namespace a = stackchan::audio;

namespace {

std::unique_ptr<AudioStreamPacket> Packet(uint8_t tag = 0) {
    auto p = std::make_unique<AudioStreamPacket>();
    p->payload = {tag};
    return p;
}

std::unique_ptr<AudioTask> Pcm() {
    auto t = std::make_unique<AudioTask>();
    t->type = kAudioTaskTypeDecodeToPlaybackQueue;
    t->pcm = {1, 2, 3};
    return t;
}

struct Rig {
    std::mutex mu;
    int drained = 0;
    int cleared = 0;  // on_cleared: the AudioService resets its decoder there
    a::AudioPipeline p;
    explicit Rig(a::PipelineLimits limits = {}) : p(limits, [this] { drained++; }, [this] { cleared++; }) {}

    bool Server(uint8_t tag = 0) {
        a::QueueLock lk(mu);
        auto pkt = Packet(tag);
        return p.PushServer(lk, pkt);
    }
    bool Local() {
        a::QueueLock lk(mu);
        auto pkt = Packet();
        return p.PushLocal(lk, pkt);
    }
    // The codec task: take one packet and decode it (true: queued for playback)
    bool DecodeOne() {
        a::QueueLock lk(mu);
        auto item = p.TakeForDecode(lk);
        return p.Decoded(lk, item, Pcm());
    }
    void RequestDrain() {
        a::QueueLock lk(mu);
        p.RequestDrain(lk);
    }
    uint32_t Stop(uint32_t serial) {
        a::QueueLock lk(mu);
        return p.Stop(lk, serial);
    }
    uint32_t Reset() {
        a::QueueLock lk(mu);
        return p.Reset(lk);
    }
    a::PlaybackBook Book() {
        a::QueueLock lk(mu);
        return p.book(lk);
    }
    a::PipelineStats Stats() {
        a::QueueLock lk(mu);
        return p.stats(lk);
    }
    size_t DecodeSize() {
        a::QueueLock lk(mu);
        return p.decode_size(lk);
    }
    size_t PlaybackSize() {
        a::QueueLock lk(mu);
        return p.playback_size(lk);
    }
};

TEST(AudioPipeline, ServerAudioIsCountedOnlyWhenItEnters) {
    Rig r({2, 2});
    ASSERT_TRUE(r.Server(1));
    ASSERT_TRUE(r.Server(2));
    a::QueueLock lk(r.mu);
    auto third = Packet(3);
    EXPECT_FALSE(r.p.PushServer(lk, third));
    ASSERT_NE(third, nullptr);  // stays with the caller
    EXPECT_EQ(third->payload[0], 3);
    EXPECT_EQ(r.p.stats(lk).server_full, 1u);
    EXPECT_EQ(r.p.book(lk).server_in_flight(), 2u);
    EXPECT_EQ(r.p.book(lk).counter().pending(), 2u);
    EXPECT_TRUE(r.p.DecodeQueueFull(lk));
    auto item = r.p.TakeForDecode(lk);
    EXPECT_TRUE(item.packet->origin.server);
    EXPECT_EQ(item.packet->origin.accept_serial, 0u);
    EXPECT_EQ(item.packet->payload[0], 1);
    EXPECT_FALSE(r.p.DecodeQueueFull(lk));
}

TEST(AudioPipeline, ServerAudioCarriesTheStopSerialItWasQueuedUnder) {
    Rig r;
    r.Stop(1);
    r.Stop(2);
    ASSERT_TRUE(r.Server());
    a::QueueLock lk(r.mu);
    auto item = r.p.TakeForDecode(lk);
    EXPECT_TRUE(item.packet->origin.server);
    EXPECT_EQ(item.packet->origin.accept_serial, 2u);
}

TEST(AudioPipeline, LocalSoundsAreLocalAndNeverCounted) {
    Rig r({1, 2});
    ASSERT_TRUE(r.Local());
    {
        a::QueueLock lk(r.mu);
        EXPECT_EQ(r.p.book(lk).server_in_flight(), 0u);
        auto more = Packet(9);
        more->origin = a::Origin{true, 5};  // whatever it says, a local push is local
        EXPECT_FALSE(r.p.PushLocal(lk, more));
        ASSERT_NE(more, nullptr);
        EXPECT_EQ(r.p.stats(lk).local_full, 1u);
        EXPECT_EQ(r.p.stats(lk).server_full, 0u);
    }
    r.RequestDrain();  // no server audio: told at once
    EXPECT_EQ(r.drained, 1);
    a::QueueLock lk(r.mu);
    auto item = r.p.TakeForDecode(lk);
    EXPECT_FALSE(item.packet->origin.server);
}

TEST(AudioPipeline, LocalPushStampsTheLocalOrigin) {
    Rig r;
    a::QueueLock lk(r.mu);
    auto p = Packet();
    p->origin = a::Origin{true, 7};
    ASSERT_TRUE(r.p.PushLocal(lk, p));
    EXPECT_EQ(r.p.book(lk).server_in_flight(), 0u);
    auto item = r.p.TakeForDecode(lk);
    EXPECT_FALSE(item.packet->origin.server);
}

// Plan 2A handoff 1: the normal output tells the drain at the write end, not at the mark.
TEST(AudioPipeline, NormalOutputDrainsAtTheWriteEnd) {
    Rig r;
    ASSERT_TRUE(r.Server());
    r.RequestDrain();
    EXPECT_EQ(r.drained, 0);
    ASSERT_TRUE(r.DecodeOne());
    a::QueueLock lk(r.mu);
    ASSERT_TRUE(r.p.CanOutput(lk));
    auto out = r.p.TakeForOutput(lk);
    ASSERT_NE(out.task, nullptr);
    EXPECT_TRUE(out.task->origin.server);  // the packet's origin reached the PCM
    ASSERT_TRUE(r.p.MarkWriteStart(lk, out));
    EXPECT_EQ(r.drained, 0);  // "may write" is not "drained" (Codex review 143 Important 1)
    EXPECT_EQ(r.p.book(lk).counter().pending(), 0u);  // marked: a stop no longer clears for it
    lk.unlock();               // OutputData()
    lk.lock();
    r.p.WriteEnd(lk, out.task->origin);
    EXPECT_EQ(r.drained, 1);
    EXPECT_EQ(r.p.book(lk).server_in_flight(), 0u);
    EXPECT_EQ(r.p.stats(lk).stale_output, 0u);
}

// Plan 2A handoff 1: the last server frame, taken by the output task and doomed by a stop, tells the
// drain at the mark (it is never written).
TEST(AudioPipeline, TheLastDoomedFrameDrainsAtTheMark) {
    Rig r;
    ASSERT_TRUE(r.Server());
    ASSERT_TRUE(r.DecodeOne());
    a::QueueLock lk(r.mu);
    auto out = r.p.TakeForOutput(lk);
    r.p.RequestDrain(lk);
    EXPECT_EQ(r.drained, 0);
    const uint32_t gen = r.p.book(lk).generation();
    EXPECT_EQ(r.p.Stop(lk, 1), 0u);  // the queues are empty, but the stop must clear for the taken one
    EXPECT_EQ(r.p.book(lk).generation(), gen + 1);
    EXPECT_EQ(r.cleared, 1);
    EXPECT_EQ(r.drained, 0);
    EXPECT_FALSE(r.p.MarkWriteStart(lk, out));
    EXPECT_EQ(r.drained, 1);
    EXPECT_EQ(r.p.stats(lk).stale_output, 1u);
    EXPECT_EQ(r.p.book(lk).server_in_flight(), 0u);
}

TEST(AudioPipeline, ADecodeFailureLeavesAndIsNoReasonToClear) {
    Rig r;
    ASSERT_TRUE(r.Server());
    r.RequestDrain();
    {
        a::QueueLock lk(r.mu);
        auto item = r.p.TakeForDecode(lk);
        r.p.DecodeFailed(lk, item);
        EXPECT_EQ(r.drained, 1);  // it was the last server item
        EXPECT_EQ(r.p.stats(lk).decode_failed, 1u);
        EXPECT_EQ(r.p.book(lk).counter().pending(), 0u);
    }
    ASSERT_TRUE(r.Local());
    EXPECT_EQ(r.Stop(1), 0u);  // nothing to clear for: the popup stays
    EXPECT_EQ(r.DecodeSize(), 1u);
}

TEST(AudioPipeline, DecodedAfterAClearIsDropped) {
    Rig r;
    ASSERT_TRUE(r.Server());
    a::QueueLock lk(r.mu);
    auto item = r.p.TakeForDecode(lk);
    r.p.RequestDrain(lk);
    EXPECT_EQ(r.p.Reset(lk), 0u);  // nothing queued; the taken packet is doomed by the generation
    EXPECT_EQ(r.drained, 0);
    EXPECT_FALSE(r.p.Decoded(lk, item, Pcm()));
    EXPECT_EQ(r.p.playback_size(lk), 0u);
    EXPECT_EQ(r.p.stats(lk).stale_decoded, 1u);
    EXPECT_EQ(r.drained, 1);
    EXPECT_EQ(r.p.book(lk).server_in_flight(), 0u);
}

TEST(AudioPipeline, DecodedInTimeIsQueuedWithItsOrigin) {
    Rig r;
    r.Stop(3);
    ASSERT_TRUE(r.Server());
    a::QueueLock lk(r.mu);
    auto item = r.p.TakeForDecode(lk);
    auto task = Pcm();
    task->origin = a::Origin{false, 0};
    ASSERT_TRUE(r.p.Decoded(lk, item, std::move(task)));
    auto out = r.p.TakeForOutput(lk);
    EXPECT_TRUE(out.task->origin.server);
    EXPECT_EQ(out.task->origin.accept_serial, 3u);
    EXPECT_EQ(out.task->pcm.size(), 3u);
}

// Plan 2A handoff 2: a stop that must clear empties both queues (local sounds too) and returns them all.
TEST(AudioPipeline, AStopThatMustClearEmptiesBothQueues) {
    Rig r;
    ASSERT_TRUE(r.Server());
    ASSERT_TRUE(r.DecodeOne());  // one server item in the playback queue
    ASSERT_TRUE(r.Server());
    ASSERT_TRUE(r.Local());
    r.RequestDrain();
    EXPECT_EQ(r.Stop(1), 3u);
    EXPECT_EQ(r.DecodeSize(), 0u);
    EXPECT_EQ(r.PlaybackSize(), 0u);
    EXPECT_EQ(r.drained, 1);
    EXPECT_EQ(r.Book().server_in_flight(), 0u);
    EXPECT_EQ(r.Stats().clears, 1u);
}

TEST(AudioPipeline, AStopWithNothingNewKeepsLocalSounds) {
    Rig r;
    ASSERT_TRUE(r.Server());
    EXPECT_EQ(r.Stop(1), 1u);
    EXPECT_EQ(r.cleared, 1);
    ASSERT_TRUE(r.Local());
    const uint32_t gen = r.Book().generation();
    EXPECT_EQ(r.Stop(2), 0u);
    EXPECT_EQ(r.cleared, 1);
    EXPECT_EQ(r.DecodeSize(), 1u);
    EXPECT_EQ(r.Book().generation(), gen);
    EXPECT_EQ(r.Book().stop_serial(), 2u);
    EXPECT_EQ(r.Stats().clears, 1u);
}

TEST(AudioPipeline, ClearIsUnconditional) {
    Rig r;
    ASSERT_TRUE(r.Local());
    ASSERT_TRUE(r.DecodeOne());
    ASSERT_TRUE(r.Local());
    const uint32_t gen = r.Book().generation();
    {
        a::QueueLock lk(r.mu);
        r.p.Clear(lk);
    }
    EXPECT_EQ(r.DecodeSize(), 0u);
    EXPECT_EQ(r.PlaybackSize(), 0u);
    EXPECT_EQ(r.Book().generation(), gen + 1);
    EXPECT_EQ(r.cleared, 1);
}

// Follow-up 4: one generation. A PCM item taken before FW-A's reset is dropped at the mark.
TEST(AudioPipeline, ResetDoomsWhatTheOutputTaskHolds) {
    Rig r;
    ASSERT_TRUE(r.Local());
    ASSERT_TRUE(r.DecodeOne());
    ASSERT_TRUE(r.Server());
    a::QueueLock lk(r.mu);
    auto out = r.p.TakeForOutput(lk);
    EXPECT_EQ(r.p.Reset(lk), 1u);
    EXPECT_EQ(r.cleared, 1);
    EXPECT_FALSE(r.p.MarkWriteStart(lk, out));
    EXPECT_EQ(r.p.book(lk).server_in_flight(), 0u);
    EXPECT_TRUE(r.p.Empty(lk));
}

TEST(AudioPipeline, TheRecordingReplacesTheDecodeQueueAsLocalSounds) {
    Rig r;
    ASSERT_TRUE(r.Server());
    r.RequestDrain();
    std::deque<std::unique_ptr<AudioStreamPacket>> rec;
    rec.push_back(Packet(1));
    rec.push_back(Packet(2));
    rec.back()->origin = a::Origin{true, 4};
    const uint32_t gen = r.Book().generation();
    {
        a::QueueLock lk(r.mu);
        r.p.ReplaceDecodeQueue(lk, std::move(rec));
    }
    EXPECT_EQ(r.drained, 1);
    EXPECT_EQ(r.cleared, 1);
    EXPECT_EQ(r.DecodeSize(), 2u);
    EXPECT_EQ(r.Book().generation(), gen + 1);
    EXPECT_EQ(r.Book().server_in_flight(), 0u);
    EXPECT_EQ(r.Stats().clears, 1u);
    a::QueueLock lk(r.mu);
    auto first = r.p.TakeForDecode(lk);
    auto second = r.p.TakeForDecode(lk);
    EXPECT_FALSE(first.packet->origin.server);
    EXPECT_FALSE(second.packet->origin.server);
    EXPECT_EQ(second.packet->payload[0], 2);
}

// After a clear, the next items carry the new generation and play.
TEST(AudioPipeline, AfterAClearTheNextItemsPlay) {
    Rig r;
    ASSERT_TRUE(r.Server());
    EXPECT_EQ(r.Reset(), 1u);
    ASSERT_TRUE(r.Server());
    ASSERT_TRUE(r.DecodeOne());
    a::QueueLock lk(r.mu);
    EXPECT_EQ(r.p.stats(lk).stale_decoded, 0u);
    auto out = r.p.TakeForOutput(lk);
    EXPECT_TRUE(r.p.MarkWriteStart(lk, out));
    EXPECT_EQ(r.p.stats(lk).stale_output, 0u);
}

// A clear counts only the server items it removes: a server packet being decoded is still in flight.
TEST(AudioPipeline, AClearCountsOnlyTheServerItemsItRemoves) {
    Rig r;
    ASSERT_TRUE(r.Local());
    ASSERT_TRUE(r.DecodeOne());  // a local sound waits in the playback queue
    ASSERT_TRUE(r.Server());
    a::QueueLock lk(r.mu);
    auto decoding = r.p.TakeForDecode(lk);
    r.p.RequestDrain(lk);
    r.p.Clear(lk);
    EXPECT_EQ(r.drained, 0);
    EXPECT_EQ(r.p.book(lk).server_in_flight(), 1u);
    EXPECT_FALSE(r.p.Decoded(lk, decoding, Pcm()));
    EXPECT_EQ(r.drained, 1);
}

TEST(AudioPipeline, EmptyLooksAtBothQueues) {
    Rig r;
    ASSERT_TRUE(r.Local());
    ASSERT_TRUE(r.DecodeOne());
    a::QueueLock lk(r.mu);
    EXPECT_EQ(r.p.decode_size(lk), 0u);
    EXPECT_FALSE(r.p.Empty(lk));
    r.p.TakeForOutput(lk);
    EXPECT_TRUE(r.p.Empty(lk));
}

TEST(AudioPipeline, TheTasksWaitOnTheirOwnConditions) {
    Rig r({3, 1});
    a::QueueLock lk(r.mu);
    EXPECT_FALSE(r.p.CanDecode(lk));
    EXPECT_FALSE(r.p.CanOutput(lk));
    EXPECT_TRUE(r.p.Empty(lk));
    EXPECT_EQ(r.p.TakeForDecode(lk).packet, nullptr);
    EXPECT_EQ(r.p.TakeForOutput(lk).task, nullptr);
    auto p1 = Packet(), p2 = Packet();
    ASSERT_TRUE(r.p.PushLocal(lk, p1));
    ASSERT_TRUE(r.p.PushLocal(lk, p2));
    EXPECT_FALSE(r.p.Empty(lk));
    EXPECT_TRUE(r.p.CanDecode(lk));
    auto item = r.p.TakeForDecode(lk);
    ASSERT_TRUE(r.p.Decoded(lk, item, Pcm()));
    EXPECT_FALSE(r.p.CanDecode(lk));  // the playback queue (1) is full
    EXPECT_TRUE(r.p.CanOutput(lk));
    auto out = r.p.TakeForOutput(lk);
    EXPECT_TRUE(r.p.CanDecode(lk));
    EXPECT_TRUE(r.p.MarkWriteStart(lk, out));  // a local sound of the current generation
}

// Claude review 156 Important 2: the tasks' one-item steps. `write` and `decode` run without the
// lock; the write end and the decode result are told under it again.
bool HeldByAnother(std::mutex& mu) {
    bool other_could_lock = true;
    std::thread t([&] {
        other_could_lock = mu.try_lock();
        if (other_could_lock) mu.unlock();
    });
    t.join();
    return !other_could_lock;
}

TEST(AudioPipeline, OutputOneWritesWithoutTheLockThenEndsUnderIt) {
    Rig r;
    ASSERT_TRUE(r.Server());
    ASSERT_TRUE(r.DecodeOne());
    r.RequestDrain();
    a::QueueLock lk(r.mu);
    auto out = r.p.TakeForOutput(lk);
    int writes = 0;
    bool locked_while_writing = true;
    EXPECT_TRUE(r.p.OutputOne(lk, out, [&](const AudioTask& task) {
        writes++;
        locked_while_writing = HeldByAnother(r.mu);
        EXPECT_EQ(task.pcm.size(), 3u);
        EXPECT_EQ(r.drained, 0);  // not before the write ends
    }));
    EXPECT_TRUE(lk.owns_lock());
    EXPECT_EQ(writes, 1);
    EXPECT_FALSE(locked_while_writing);
    EXPECT_EQ(r.drained, 1);  // the write end met the drain
    EXPECT_EQ(r.p.book(lk).server_in_flight(), 0u);
}

TEST(AudioPipeline, OutputOneSkipsADoomedItem) {
    Rig r;
    ASSERT_TRUE(r.Server());
    ASSERT_TRUE(r.DecodeOne());
    a::QueueLock lk(r.mu);
    auto out = r.p.TakeForOutput(lk);
    r.p.RequestDrain(lk);
    r.p.Stop(lk, 1);
    int writes = 0;
    EXPECT_FALSE(r.p.OutputOne(lk, out, [&](const AudioTask&) { writes++; }));
    EXPECT_TRUE(lk.owns_lock());
    EXPECT_EQ(writes, 0);
    EXPECT_EQ(r.drained, 1);  // told at the mark
}

TEST(AudioPipeline, DecodeOneDecodesWithoutTheLockAndTellsTheResult) {
    Rig r;
    ASSERT_TRUE(r.Server(5));
    ASSERT_TRUE(r.Server(6));
    r.RequestDrain();
    a::QueueLock lk(r.mu);
    bool locked_while_decoding = true;
    auto first = r.p.TakeForDecode(lk);
    EXPECT_TRUE(r.p.DecodeOne(lk, first, [&](const AudioStreamPacket& p) {
        locked_while_decoding = HeldByAnother(r.mu);
        EXPECT_EQ(p.payload[0], 5);
        return Pcm();
    }));
    EXPECT_TRUE(lk.owns_lock());
    EXPECT_FALSE(locked_while_decoding);
    EXPECT_EQ(r.p.playback_size(lk), 1u);
    auto second = r.p.TakeForDecode(lk);
    EXPECT_FALSE(r.p.DecodeOne(lk, second, [](const AudioStreamPacket&) { return std::unique_ptr<AudioTask>(); }));
    EXPECT_TRUE(lk.owns_lock());
    EXPECT_EQ(r.p.stats(lk).decode_failed, 1u);
    EXPECT_EQ(r.p.playback_size(lk), 1u);
    EXPECT_EQ(r.p.book(lk).server_in_flight(), 1u);  // the decoded one is still in flight
    EXPECT_EQ(r.drained, 0);
    auto out = r.p.TakeForOutput(lk);
    EXPECT_TRUE(r.p.OutputOne(lk, out, [](const AudioTask&) {}));
    EXPECT_EQ(r.drained, 1);
}

TEST(AudioPipeline, DecodeOneAfterAClearDropsThePcm) {
    Rig r;
    ASSERT_TRUE(r.Server());
    a::QueueLock lk(r.mu);
    auto item = r.p.TakeForDecode(lk);
    bool cleared_meanwhile = false;
    EXPECT_FALSE(r.p.DecodeOne(lk, item, [&](const AudioStreamPacket&) {
        // a stop runs while decoding (try_to_lock: a step that decoded under the lock fails here
        // instead of deadlocking)
        a::QueueLock other(r.mu, std::try_to_lock);
        if (other.owns_lock()) {
            r.p.Reset(other);
            cleared_meanwhile = true;
        }
        return Pcm();
    }));
    EXPECT_TRUE(cleared_meanwhile);
    EXPECT_EQ(r.p.stats(lk).stale_decoded, 1u);
    EXPECT_EQ(r.p.stats(lk).decode_failed, 0u);
    EXPECT_EQ(r.p.playback_size(lk), 0u);
}

// Claude review 156 Minor 1: the recording's swap empties the playback queue too, so a server
// frame left there cannot uncount one queued after the swap (the counter-example).
TEST(AudioPipeline, TheRecordingsSwapEmptiesThePlaybackQueueToo) {
    Rig r;
    ASSERT_TRUE(r.Server(1));
    ASSERT_TRUE(r.DecodeOne());  // P waits for playback
    r.RequestDrain();
    {
        a::QueueLock lk(r.mu);
        std::deque<std::unique_ptr<AudioStreamPacket>> rec;
        rec.push_back(Packet(9));
        r.p.ReplaceDecodeQueue(lk, std::move(rec));
        EXPECT_EQ(r.p.playback_size(lk), 0u);
        EXPECT_EQ(r.p.book(lk).server_in_flight(), 0u);
    }
    EXPECT_EQ(r.drained, 1);
    ASSERT_TRUE(r.Server(2));  // X
    EXPECT_EQ(r.Stop(1), 2u);  // X is cleared (with the recording)
    EXPECT_EQ(r.DecodeSize(), 0u);
}

// Claude review 156 Minor 6: an entry without the audio queue lock aborts
TEST(AudioPipelineDeathTest, AnEntryWithoutTheLockAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Rig r;
    a::QueueLock unlocked(r.mu, std::defer_lock);
    EXPECT_DEATH(r.p.Empty(unlocked), "");
    auto p = Packet();
    EXPECT_DEATH(r.p.PushServer(unlocked, p), "");
}

// The drain notice is posted under the audio queue lock (design §1.2: audio_queue_mutex_ ->
// ui_queue_mutex_), through the gate's AudioSink too.
TEST(PipelineSink, StopAndClearTakeTheLockAndNotify) {
    std::mutex mu;
    int notified = 0;
    int drained_under_lock = 0;
    int cleared_under_lock = 0;
    auto locked = [&] {
        bool other_could_lock = true;
        std::thread t([&] {
            other_could_lock = mu.try_lock();
            if (other_could_lock) mu.unlock();
        });
        t.join();
        return !other_could_lock;
    };
    a::AudioPipeline p(
        {}, [&] { drained_under_lock += locked() ? 1 : 0; }, [&] { cleared_under_lock += locked() ? 1 : 0; });
    a::PipelineSink sink(&mu, &p, [&] { notified++; });
    {
        a::QueueLock lk(mu);
        auto s = Packet();
        ASSERT_TRUE(p.PushServer(lk, s));
        auto l = Packet();
        ASSERT_TRUE(p.PushLocal(lk, l));
        p.RequestDrain(lk);
    }
    EXPECT_EQ(sink.Stop(1), 2u);
    EXPECT_EQ(notified, 1);
    EXPECT_EQ(drained_under_lock, 1);
    EXPECT_EQ(cleared_under_lock, 1);
    {
        a::QueueLock lk(mu);
        auto l = Packet();
        ASSERT_TRUE(p.PushLocal(lk, l));
    }
    EXPECT_EQ(sink.Stop(2), 0u);  // nothing new from the server: the popup stays
    {
        a::QueueLock lk(mu);
        EXPECT_EQ(p.decode_size(lk), 1u);
    }
    sink.Clear();
    EXPECT_EQ(notified, 3);
    EXPECT_EQ(cleared_under_lock, 2);
    a::QueueLock lk(mu);
    EXPECT_EQ(p.decode_size(lk), 0u);
}

}  // namespace
