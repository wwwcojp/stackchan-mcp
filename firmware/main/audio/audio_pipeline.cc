// StackChan FW-A2 plan 2B-2a: the AudioService's playback queues and book (see audio_pipeline.h).
#include "audio_pipeline.h"

#include <cstdlib>
#include <utility>

namespace stackchan::audio {

namespace {

// Every entry runs under the AudioService's audio queue lock.
void Held(const QueueLock& lk) {
    if (!lk.owns_lock()) std::abort();
}

}  // namespace

AudioPipeline::AudioPipeline(PipelineLimits limits, std::function<void()> post_drained,
                             std::function<void()> on_cleared)
    : limits_(limits), post_drained_(std::move(post_drained)), on_cleared_(std::move(on_cleared)) {}

void AudioPipeline::Post(bool drained) {
    if (drained && post_drained_) post_drained_();
}

bool AudioPipeline::PushServer(const QueueLock& lk, std::unique_ptr<AudioStreamPacket>& packet) {
    Held(lk);
    if (decode_.size() >= limits_.decode) {
        stats_.server_full++;
        return false;
    }
    packet->origin = book_.OnServerQueued();
    decode_.push_back(std::move(packet));
    return true;
}

bool AudioPipeline::PushLocal(const QueueLock& lk, std::unique_ptr<AudioStreamPacket>& packet) {
    Held(lk);
    if (decode_.size() >= limits_.decode) {
        stats_.local_full++;
        return false;
    }
    packet->origin = PlaybackBook::Local();
    decode_.push_back(std::move(packet));
    return true;
}

bool AudioPipeline::DecodeQueueFull(const QueueLock& lk) const {
    Held(lk);
    return decode_.size() >= limits_.decode;
}

bool AudioPipeline::CanDecode(const QueueLock& lk) const {
    Held(lk);
    return !decode_.empty() && playback_.size() < limits_.playback;
}

AudioPipeline::ToDecode AudioPipeline::TakeForDecode(const QueueLock& lk) {
    Held(lk);
    ToDecode item;
    if (decode_.empty()) return item;
    item.packet = std::move(decode_.front());
    decode_.pop_front();
    item.gen = book_.generation();
    return item;
}

bool AudioPipeline::DecodeOne(QueueLock& lk, const ToDecode& item,
                              const std::function<std::unique_ptr<AudioTask>(const AudioStreamPacket&)>& decode) {
    Held(lk);
    lk.unlock();
    std::unique_ptr<AudioTask> task = decode(*item.packet);
    lk.lock();
    if (!task) {
        DecodeFailed(lk, item);
        return false;
    }
    return Decoded(lk, item, std::move(task));
}

void AudioPipeline::DecodeFailed(const QueueLock& lk, const ToDecode& item) {
    Held(lk);
    stats_.decode_failed++;
    Post(book_.OnDecodeDropped(item.packet->origin, item.gen));
}

bool AudioPipeline::Decoded(const QueueLock& lk, const ToDecode& item, std::unique_ptr<AudioTask> task) {
    Held(lk);
    if (item.gen != book_.generation()) {
        stats_.stale_decoded++;
        Post(book_.OnDecodeDropped(item.packet->origin, item.gen));
        return false;
    }
    task->origin = item.packet->origin;
    playback_.push_back(std::move(task));
    return true;
}

bool AudioPipeline::CanOutput(const QueueLock& lk) const {
    Held(lk);
    return !playback_.empty();
}

AudioPipeline::ToOutput AudioPipeline::TakeForOutput(const QueueLock& lk) {
    Held(lk);
    ToOutput item;
    if (playback_.empty()) return item;
    item.task = std::move(playback_.front());
    playback_.pop_front();
    item.gen = book_.generation();
    return item;
}

bool AudioPipeline::MarkWriteStart(const QueueLock& lk, const ToOutput& item) {
    Held(lk);
    const PlaybackBook::Mark m = book_.MarkWriteStart(item.task->origin, item.gen);
    if (!m.write) stats_.stale_output++;
    Post(m.drained);
    return m.write;
}

void AudioPipeline::WriteEnd(const QueueLock& lk, const Origin& origin) {
    Held(lk);
    Post(book_.OnWriteEnd(origin));
}

bool AudioPipeline::OutputOne(QueueLock& lk, const ToOutput& item, const std::function<void(AudioTask&)>& write) {
    if (!MarkWriteStart(lk, item)) return false;
    lk.unlock();
    write(*item.task);
    lk.lock();
    WriteEnd(lk, item.task->origin);
    return true;
}

uint32_t AudioPipeline::ClearAll() {
    uint32_t server = 0;
    for (const auto& p : decode_) server += p->origin.server ? 1 : 0;
    for (const auto& t : playback_) server += t->origin.server ? 1 : 0;
    const uint32_t removed = static_cast<uint32_t>(decode_.size() + playback_.size());
    decode_.clear();
    playback_.clear();
    stats_.clears++;
    if (on_cleared_) on_cleared_();
    Post(book_.OnCleared(server));
    return removed;
}

uint32_t AudioPipeline::Stop(const QueueLock& lk, uint32_t serial) {
    Held(lk);
    if (!book_.OnStop(serial)) return 0;
    return ClearAll();
}

void AudioPipeline::Clear(const QueueLock& lk) {
    Held(lk);
    ClearAll();
}

void AudioPipeline::RequestDrain(const QueueLock& lk) {
    Held(lk);
    Post(book_.RequestDrain());
}

uint32_t AudioPipeline::Reset(const QueueLock& lk) {
    Held(lk);
    return ClearAll();
}

void AudioPipeline::ReplaceDecodeQueue(const QueueLock& lk, std::deque<std::unique_ptr<AudioStreamPacket>> packets) {
    Held(lk);
    uint32_t server = 0;
    for (const auto& p : decode_) server += p->origin.server ? 1 : 0;
    for (const auto& t : playback_) server += t->origin.server ? 1 : 0;
    for (auto& p : packets) p->origin = PlaybackBook::Local();
    decode_ = std::move(packets);
    playback_.clear();
    stats_.clears++;
    if (on_cleared_) on_cleared_();
    Post(book_.OnCleared(server));
}

bool AudioPipeline::Empty(const QueueLock& lk) const {
    Held(lk);
    return decode_.empty() && playback_.empty();
}

size_t AudioPipeline::decode_size(const QueueLock& lk) const {
    Held(lk);
    return decode_.size();
}

size_t AudioPipeline::playback_size(const QueueLock& lk) const {
    Held(lk);
    return playback_.size();
}

const PlaybackBook& AudioPipeline::book(const QueueLock& lk) const {
    Held(lk);
    return book_;
}

PipelineStats AudioPipeline::stats(const QueueLock& lk) const {
    Held(lk);
    return stats_;
}

PipelineSink::PipelineSink(std::mutex* mutex, AudioPipeline* pipeline, std::function<void()> notify)
    : mutex_(mutex), pipeline_(pipeline), notify_(std::move(notify)) {}

uint32_t PipelineSink::Stop(uint32_t serial) {
    uint32_t removed = 0;
    {
        QueueLock lk(*mutex_);
        removed = pipeline_->Stop(lk, serial);
    }
    if (notify_) notify_();
    return removed;
}

void PipelineSink::Clear() {
    {
        QueueLock lk(*mutex_);
        pipeline_->Clear(lk);
    }
    if (notify_) notify_();
}

}  // namespace stackchan::audio
