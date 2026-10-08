// StackChan FW-A2 §4.1: the send queue (see send_queue.h).
#include "send_queue.h"

#include <algorithm>
#include <chrono>

namespace stackchan::net {

SendQueue::SendQueue(Limits limits) : limits_(limits) { stats_.min_free_items = limits.max_items; }

void SendQueue::Open(uint64_t e) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    e_ = e;
    open_ = true;
    flushing_ = false;
    woken_ = false;
    Account();
    cv_.notify_all();
}

void SendQueue::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    open_ = false;
    flushing_ = false;
    Account();
    cv_.notify_all();
}

void SendQueue::CloseForFlush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return;
    open_ = false;
    flushing_ = true;
    cv_.notify_all();
}

bool SendQueue::RoomFor(size_t bytes, size_t extra_items) const {
    return stats_.items + extra_items <= limits_.max_items && stats_.bytes + bytes <= limits_.max_bytes;
}

void SendQueue::DropOldestMic() {
    auto it = std::find_if(items_.begin(), items_.end(), [](const Elem& x) { return x.kind == ElemKind::kMic; });
    if (it == items_.end()) return;
    items_.erase(it);
    stats_.mic_dropped++;
    Account();
}

void SendQueue::Account() {
    stats_.items = items_.size();
    stats_.bytes = 0;
    stats_.mic = 0;
    for (const Elem& x : items_) {
        stats_.bytes += x.payload.size();
        if (x.kind == ElemKind::kMic) stats_.mic++;
    }
    stats_.min_free_items = std::min(stats_.min_free_items, limits_.max_items - std::min(stats_.items, limits_.max_items));
}

PushResult SendQueue::Push(uint64_t e, ElemKind kind, std::string payload, int64_t now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || e != e_ || e == 0) {
        stats_.rejected_closed++;
        return PushResult::kClosed;
    }
    const size_t bytes = payload.size();
    bool dropped = false;
    if (kind == ElemKind::kMic) {
        if (limits_.max_mic == 0) {
            stats_.mic_dropped++;
            return PushResult::kDroppedMic;
        }
        // would it fit if every queued mic frame went? If not, keep them and drop the new one
        size_t mic_bytes = 0;
        for (const Elem& x : items_) {
            if (x.kind == ElemKind::kMic) mic_bytes += x.payload.size();
        }
        if (stats_.items - stats_.mic + 1 > limits_.max_items ||
            stats_.bytes - mic_bytes + bytes > limits_.max_bytes) {
            stats_.mic_dropped++;
            return PushResult::kDroppedMic;
        }
        while (stats_.mic > 0 && (stats_.mic >= limits_.max_mic || !RoomFor(bytes, 1))) {
            DropOldestMic();
            dropped = true;
        }
        if (!RoomFor(bytes, 1)) {  // not reached when the check above holds
            stats_.mic_dropped++;
            return PushResult::kDroppedMic;
        }
    } else if (!RoomFor(bytes, 1)) {
        return PushResult::kFull;
    }
    items_.push_back(Elem{e, kind, std::move(payload), now_us + kSendDeadlineUs});
    Account();
    cv_.notify_all();
    return dropped ? PushResult::kQueuedDroppedOldMic : PushResult::kQueued;
}

PushResult SendQueue::PushPair(uint64_t e, std::string first, std::string second, int64_t now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || e != e_ || e == 0) {
        stats_.rejected_closed++;
        return PushResult::kClosed;
    }
    if (!RoomFor(first.size() + second.size(), 2)) return PushResult::kFull;
    const int64_t deadline = now_us + kSendDeadlineUs;
    items_.push_back(Elem{e, ElemKind::kJson, std::move(first), deadline});
    items_.push_back(Elem{e, ElemKind::kJson, std::move(second), deadline});
    Account();
    cv_.notify_all();
    return PushResult::kQueued;
}

std::optional<Elem> SendQueue::Pop(int64_t timeout_us) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::microseconds(timeout_us),
                 [this] { return woken_ || !items_.empty() || !open_; });
    if (woken_) {
        woken_ = false;
        return std::nullopt;
    }
    if (items_.empty()) return std::nullopt;
    Elem x = std::move(items_.front());
    items_.pop_front();
    Account();
    return x;
}

void SendQueue::Wake() {
    std::lock_guard<std::mutex> lock(mutex_);
    woken_ = true;
    cv_.notify_all();
}

bool SendQueue::Drained() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !open_ && items_.empty();
}

bool SendQueue::Flushing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return flushing_;
}

bool SendQueue::FlushDone() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return flushing_ && items_.empty();
}

QueueStats SendQueue::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

uint64_t SendQueue::e() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return e_;
}

}  // namespace stackchan::net
