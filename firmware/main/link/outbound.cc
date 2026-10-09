#include "outbound.h"

#include <cJSON.h>

#include <optional>
#include <utility>

#include "tx_loop.h"

namespace stackchan::link {

namespace {

// The printed JSON, or nothing (the root is freed either way)
std::optional<std::string> Print(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) return std::nullopt;
    std::string json(text);
    cJSON_free(text);
    return json;
}

}  // namespace

Outbound::Outbound(OutboundDeps deps) : d_(std::move(deps)) {}

OutResult Outbound::Queue(uint64_t e, std::string json) {
    gate::PlaybackGate* g = d_.gate;
    NoticeQueue* n = d_.notices;
    const net::PushResult r = QueueJsonOrEnd(
        *d_.audio_queue, e, std::move(json), d_.now_us(),
        [g](uint64_t pair, EndReason reason) { g->StopForDeath(pair, reason); },
        [n](const Input& in) { return n->Post(in); });
    switch (r) {
        case net::PushResult::kQueued:
            return OutResult::kQueued;
        case net::PushResult::kFull:
            return OutResult::kFull;
        default:
            closed_++;
            return OutResult::kClosed;
    }
}

OutResult Outbound::McpReply(uint64_t e, const std::string& payload) {
    const std::optional<std::string> session = d_.gate->SessionFor(e);
    if (!session) {
        unbound_++;
        return OutResult::kUnbound;
    }
    cJSON* body = cJSON_Parse(payload.c_str());
    if (body == nullptr) {
        bad_json_++;
        return OutResult::kBadJson;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session->c_str());
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddItemToObject(root, "payload", body);
    std::optional<std::string> json = Print(root);
    if (!json) {
        bad_json_++;
        return OutResult::kBadJson;
    }
    return Queue(e, std::move(*json));
}

OutResult Outbound::StackChanEvent(const std::string& event_type, const std::string& subtype,
                                   uint64_t duration_ms, int64_t ts_ms) {
    const uint64_t e = d_.bound_pair();
    const std::optional<std::string> session = d_.gate->SessionFor(e);
    if (!session) {
        unbound_++;
        return OutResult::kUnbound;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session->c_str());
    cJSON_AddStringToObject(root, "type", "stackchan-event");
    cJSON_AddStringToObject(root, "event_type", event_type.c_str());
    cJSON_AddStringToObject(root, "subtype", subtype.c_str());
    cJSON_AddNumberToObject(root, "duration_ms", static_cast<double>(duration_ms));
    cJSON_AddNumberToObject(root, "ts", static_cast<double>(ts_ms));
    std::optional<std::string> json = Print(root);
    if (!json) {
        bad_json_++;
        return OutResult::kBadJson;
    }
    return Queue(e, std::move(*json));
}

OutResult Outbound::JsonString(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        bad_json_++;
        return OutResult::kBadJson;
    }
    cJSON_Delete(root);
    const uint64_t e = d_.bound_pair();
    if (!d_.gate->SessionFor(e)) {
        unbound_++;
        return OutResult::kUnbound;
    }
    return Queue(e, json);
}

OutboundStats Outbound::stats() const {
    OutboundStats s;
    s.unbound = unbound_.load();
    s.closed = closed_.load();
    s.bad_json = bad_json_.load();
    return s;
}

}  // namespace stackchan::link
