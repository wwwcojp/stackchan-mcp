// StackChan FW-A2 plan 2B-2b (design §4.1, plan 2B-2a handoff 7): what the device sends on its own,
// into the audio send queue. An MCP reply goes out on the pair its request came on, with that
// pair's session; a reply made after the pair ended is dropped, never sent with a later pair's
// session. A device notice (stackchan-event, SendJsonString) takes the pair bound at send time.
// Any task may call it (the MCP replies come from the receive task and the main task). Never
// waits: a full queue ends the pair (JSON is never dropped, design §4.1), the rest is counted.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "link_manager.h"
#include "playback_gate.h"
#include "send_queue.h"

namespace stackchan::link {

enum class OutResult {
    kQueued,
    kUnbound,  // no bound live pair, or the request's pair is not the gate's any more: dropped
    kClosed,   // the audio send queue is closed for it (the pair is ending): dropped
    kFull,     // no room: the pair ends (the gate stops the sound, the manager is asked to end E)
    kBadJson,  // not a JSON object: dropped
};

struct OutboundStats {
    uint32_t unbound = 0;
    uint32_t closed = 0;
    uint32_t bad_json = 0;
};

struct OutboundDeps {
    gate::PlaybackGate* gate = nullptr;
    net::SendQueue* audio_queue = nullptr;
    NoticeQueue* notices = nullptr;
    std::function<int64_t()> now_us;
    std::function<uint64_t()> bound_pair;  // LinkHub::BoundPair
};

class Outbound {
public:
    explicit Outbound(OutboundDeps deps);
    // {"session_id", "type":"mcp", "payload": <payload>} on pair e (the request's)
    OutResult McpReply(uint64_t e, const std::string& payload);
    // {"session_id", "type":"stackchan-event", "event_type", "subtype", "duration_ms", "ts"}
    OutResult StackChanEvent(const std::string& event_type, const std::string& subtype,
                             uint64_t duration_ms, int64_t ts_ms);
    // A JSON object from the board, as it is (today's SendJsonString adds no session)
    OutResult JsonString(const std::string& json);
    OutboundStats stats() const;

private:
    OutResult Queue(uint64_t e, std::string json);

    OutboundDeps d_;
    std::atomic<uint32_t> unbound_{0}, closed_{0}, bad_json_{0};
};

}  // namespace stackchan::link
