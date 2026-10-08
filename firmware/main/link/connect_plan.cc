// StackChan FW-A2 plan 2B-1: what one connect worker does (see connect_plan.h).
#include "connect_plan.h"

#include <cJSON.h>

#include "wire.h"

namespace stackchan::link {

namespace {

// Today's token rules (websocket_protocol.cc): with the force switch a non-empty Kconfig token
// wins; otherwise NVS, then Kconfig. A token without a space gets "Bearer ".
std::string AuthorizationOf(const ConnectInputs& in) {
    std::string token = in.nvs_token;
    if ((in.targets.force_kconfig || token.empty()) && !in.kconfig_token.empty()) token = in.kconfig_token;
    if (!token.empty() && token.find(' ') == std::string::npos) token = "Bearer " + token;
    return token;
}

}  // namespace

std::string BuildAudioHello(int version, bool server_aec, int frame_duration_ms) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version);
    cJSON* features = cJSON_CreateObject();
    if (server_aec) cJSON_AddBoolToObject(features, "aec", true);
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddNumberToObject(features, "stackchan_ext", 1);  // FW-A (design §1)
    cJSON_AddItemToObject(root, "features", features);
    wire::AddCtrlFeature(root);  // contract S1
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "format", "opus");
    cJSON_AddNumberToObject(params, "sample_rate", 16000);
    cJSON_AddNumberToObject(params, "channels", 1);
    cJSON_AddNumberToObject(params, "frame_duration", frame_duration_ms);
    cJSON_AddItemToObject(root, "audio_params", params);
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text != nullptr ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

std::string AudioUrlFor(const ConnectInputs& in, uint32_t attempt) {
    return CandidateFor(GatewayCandidates(in.targets), attempt);
}

ConnectPlan PlanConnect(const ConnectInputs& in, const std::string& url, LinkSide side) {
    ConnectPlan p;
    p.url = url;
    if (url.empty()) {
        p.error = "no gateway URL";
        return p;
    }
    p.target = ParseWsTarget(url);
    if (!p.target.ok) {
        p.error = p.target.error;
        return p;
    }
    p.version = in.nvs_version != 0 ? in.nvs_version : 1;
    const std::string auth = AuthorizationOf(in);
    if (!auth.empty()) p.headers.emplace_back("Authorization", auth);
    p.headers.emplace_back("Protocol-Version", std::to_string(p.version));
    p.headers.emplace_back("Device-Id", in.device_id);
    p.headers.emplace_back("Client-Id", in.client_id);
    if (side == LinkSide::kCtrl) p.headers.emplace_back("X-Stackchan-Role", "control");
    p.ok = true;
    return p;
}

}  // namespace stackchan::link
