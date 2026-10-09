// StackChan FW-A2 §4.1: one send queue per link (audio, control). Elements carry the pair E and
// one deadline (queued time + 2 s, F2) shared by the wait in the queue, the send and every partial
// send. Pushing never waits: the result tells the caller what happened (inside the gate a full
// queue is StopForDeathLocked, outside it StopForDeath; never call out from under this lock).
// std::mutex / std::condition_variable work on ESP-IDF (pthread) and on the host.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace stackchan::net {

constexpr int64_t kSendDeadlineUs = 2'000'000;  // F2

enum class ElemKind {
    kJson,  // a text frame (JSON). Order matters: a full queue ends the pair
    kMic,   // a binary frame of mic audio: the oldest is dropped when there is no room
    kPong,  // a WebSocket pong (payload = the ping's payload)
    kClose,
};

struct Elem {
    uint64_t e = 0;
    ElemKind kind = ElemKind::kJson;
    std::string payload;
    int64_t deadline_us = 0;
};

struct Limits {
    size_t max_items;
    size_t max_bytes;
    size_t max_mic;  // mic frames at most (about 1 s): room is left for JSON before F2
};
constexpr Limits kAudioLimits{48, 24 * 1024, 16};
constexpr Limits kCtrlLimits{16, 8 * 1024, 0};

enum class PushResult {
    kQueued,
    kQueuedDroppedOldMic,  // queued after dropping the oldest mic frame(s)
    kDroppedMic,           // a mic frame with no room even after dropping old ones: not queued
    kFull,                 // a JSON / pong / close with no room: the caller ends the pair
    kClosed,               // the queue is closed, or E is not the queue's pair: not queued
};

// The queue's element counts (for stat and tests)
struct QueueStats {
    size_t items = 0;
    size_t bytes = 0;
    size_t mic = 0;
    uint32_t mic_dropped = 0;
    uint32_t rejected_closed = 0;
    size_t min_free_items = 0;  // the smallest number of free items seen
};

class SendQueue {
public:
    explicit SendQueue(Limits limits);

    // A new pair: empty the queue, accept elements of E only.
    void Open(uint64_t e);
    // The pair ended: drop everything and reject new elements.
    void Close();
    // A contract violation: reject new elements but keep the queued ones for the send task to
    // flush (the done). Drained() turns true when they are gone.
    void CloseForFlush();

    PushResult Push(uint64_t e, ElemKind kind, std::string payload, int64_t now_us);
    // R5: the device abort and the listen start go in together or not at all.
    PushResult PushPair(uint64_t e, std::string first, std::string second, int64_t now_us);

    // The send task: the next element, waiting up to timeout_us; nullopt on timeout, when
    // stopping, or when the queue is closed (for good or for a flush) and empty: a closed queue
    // never keeps it waiting (Codex review 145 Minor 1).
    std::optional<Elem> Pop(int64_t timeout_us);
    // Wake Pop (the stop request).
    void Wake();

    bool Drained() const;  // closed for flush (or closed) and nothing left
    bool Flushing() const;   // closed for a flush (the done is being sent)
    bool FlushDone() const;  // closed for a flush and nothing left (kCtrlFlushed)
    // FlushDone for the pair E, in one lock section: a link of the next pair never takes an
    // earlier pair's finished flush for its own while Open(E) races it (Codex review 153 Minor 1)
    bool FlushDoneFor(uint64_t e) const;
    QueueStats Stats() const;
    uint64_t e() const;

private:
    bool RoomFor(size_t bytes, size_t extra_items) const;
    void DropOldestMic();
    void Account();

    const Limits limits_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Elem> items_;
    uint64_t e_ = 0;
    bool open_ = false;
    bool flushing_ = false;
    bool woken_ = false;
    QueueStats stats_;
};

}  // namespace stackchan::net
