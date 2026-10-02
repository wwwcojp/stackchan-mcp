#include "abort_policy.h"

#include <cJSON.h>
#include <cstring>

namespace stackchan {

bool IsAbortRequest(const cJSON* root, const std::string& session_id, std::string* reason_out) {
    if (root == nullptr || !cJSON_IsObject(root)) {
        return false;
    }
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "abort") != 0) {
        return false;
    }
    if (cJSON_HasObjectItem(root, "state")) {
        return false;  // reply-shaped ({"state":"done"}) is not a request
    }
    const cJSON* sid = cJSON_GetObjectItem(root, "session_id");
    if (!cJSON_IsString(sid) || session_id.empty() || session_id != sid->valuestring) {
        return false;
    }
    if (reason_out != nullptr) {
        const cJSON* reason = cJSON_GetObjectItem(root, "reason");
        *reason_out = cJSON_IsString(reason) ? reason->valuestring : "";
    }
    return true;
}

bool AbortReturnsToIdle(DeviceState state) {
    return state == kDeviceStateSpeaking;
}

uint32_t DroppedMs(uint32_t cleared_packets, uint32_t frame_ms) {
    return cleared_packets * frame_ms;
}

cJSON* BuildAbortDone(const std::string& session_id, const std::string& reason, uint32_t dropped_ms) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id.c_str());
    cJSON_AddStringToObject(root, "type", "abort");
    cJSON_AddStringToObject(root, "state", "done");
    cJSON_AddStringToObject(root, "reason", reason.c_str());
    cJSON_AddNumberToObject(root, "dropped_ms", static_cast<double>(dropped_ms));
    return root;
}

}  // namespace stackchan
