// StackChan FW-A2: the contract's JSON messages (see wire.h).
#include "wire.h"

#include <cJSON.h>

#include <cmath>
#include <cstring>

namespace stackchan::wire {

namespace {

bool IsString(const cJSON* root, const char* key, const char* value) {
    const cJSON* v = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(v) && std::strcmp(v->valuestring, value) == 0;
}

// A non-negative integer that fits `max` (JSON numbers are doubles in cJSON).
bool GetUint(const cJSON* root, const char* key, double max, uint64_t* out) {
    const cJSON* v = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(v)) return false;
    const double d = v->valuedouble;
    if (!(d >= 0) || d > max || std::floor(d) != d) return false;
    *out = static_cast<uint64_t>(d);
    return true;
}

bool GetU32(const cJSON* root, const char* key, uint32_t* out) {
    uint64_t v = 0;
    if (!GetUint(root, key, 4294967295.0, &v)) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

// 2^53: the largest integer a double holds exactly
constexpr double kMaxExact = 9007199254740992.0;

bool IsRequest(const cJSON* root, const char* type) {
    return cJSON_IsObject(root) && IsString(root, "type", type) && !cJSON_HasObjectItem(root, "state");
}

bool GetReqId(const cJSON* root, std::string* out) {
    const cJSON* id = cJSON_GetObjectItem(root, "req_id");
    if (!cJSON_IsString(id)) return false;
    const size_t n = std::strlen(id->valuestring);
    if (n == 0 || n > kMaxReqIdLen) return false;
    *out = id->valuestring;
    return true;
}

const char* ModeName(ListenMode m) {
    switch (m) {
        case ListenMode::kRealtime: return "realtime";
        case ListenMode::kAutoStop: return "auto";
        case ListenMode::kManualStop: break;
    }
    return "manual";
}

}  // namespace

std::string TypeOf(const cJSON* root) {
    if (!cJSON_IsObject(root)) return "";
    const cJSON* t = cJSON_GetObjectItem(root, "type");
    return cJSON_IsString(t) ? t->valuestring : "";
}

bool SessionMatches(const cJSON* root, const std::string& session_id) {
    if (!cJSON_IsObject(root) || session_id.empty()) return false;
    const cJSON* sid = cJSON_GetObjectItem(root, "session_id");
    return cJSON_IsString(sid) && session_id == sid->valuestring;
}

bool CtrlOffered(const cJSON* hello_reply) {
    if (!cJSON_IsObject(hello_reply)) return false;
    const cJSON* v = cJSON_GetObjectItem(hello_reply, "stackchan_ctrl");
    return cJSON_IsNumber(v) && v->valuedouble == 1.0;
}

bool ParseCtrlHelloReply(const cJSON* root, uint64_t* audio_epoch) {
    if (!cJSON_IsObject(root) || !IsString(root, "type", "hello") || !IsString(root, "role", "control")) {
        return false;
    }
    return GetUint(root, "audio_epoch", kMaxExact, audio_epoch);
}

bool ParseTtsStart(const cJSON* root, TtsStart* out) {
    if (!cJSON_IsObject(root) || !IsString(root, "type", "tts") || !IsString(root, "state", "start")) {
        return false;
    }
    TtsStart t;
    if (!GetU32(root, "gen", &t.gen) || t.gen == 0) return false;
    if (!GetU32(root, "aborted_gen", &t.aborted_gen)) return false;
    if (!GetU32(root, "dev_abort_seen", &t.dev_abort_seen)) return false;
    *out = t;
    return true;
}

bool ParseTtsStop(const cJSON* root, uint32_t* gen) {
    if (!cJSON_IsObject(root) || !IsString(root, "type", "tts") || !IsString(root, "state", "stop")) {
        return false;
    }
    uint32_t g = 0;
    if (!GetU32(root, "gen", &g) || g == 0) return false;
    *gen = g;
    return true;
}

bool ParseAbortRequest(const cJSON* root, AbortRequest* out) {
    if (!IsRequest(root, "abort")) return false;
    AbortRequest a;
    if (!GetU32(root, "gen", &a.gen) || a.gen == 0) return false;
    if (!GetReqId(root, &a.req_id)) return false;
    const cJSON* reason = cJSON_GetObjectItem(root, "reason");
    a.reason = cJSON_IsString(reason) ? reason->valuestring : "";
    const cJSON* sid = cJSON_GetObjectItem(root, "session_id");
    a.session_id = cJSON_IsString(sid) ? sid->valuestring : "";
    *out = a;
    return true;
}

bool ParseStatRequest(const cJSON* root, std::string* req_id) {
    if (!IsRequest(root, "stat")) return false;
    return GetReqId(root, req_id);
}

bool ParseGwListen(const cJSON* root, GwListen* out) {
    if (!cJSON_IsObject(root) || !IsString(root, "type", "listen")) return false;
    GwListen g;
    if (IsString(root, "state", "start")) {
        g.start = true;
    } else if (!IsString(root, "state", "stop")) {
        return false;
    }
    if (g.start) {
        const cJSON* mode = cJSON_GetObjectItem(root, "mode");
        if (mode != nullptr) {
            if (IsString(root, "mode", "auto")) {
                g.mode = ListenMode::kAutoStop;
            } else if (IsString(root, "mode", "realtime")) {
                g.mode = ListenMode::kRealtime;
            } else if (!IsString(root, "mode", "manual")) {
                g.mode_unknown = true;
            }
        }
        const cJSON* profile = cJSON_GetObjectItem(root, "profile");
        if (profile != nullptr) {
            if (IsString(root, "profile", "raw")) {
                g.profile = ListenProfile::kRaw;
            } else if (!IsString(root, "profile", "voice")) {
                g.profile_unknown = true;
            }
        }
    }
    *out = g;
    return true;
}

void AddCtrlFeature(cJSON* audio_hello) {
    if (!cJSON_IsObject(audio_hello)) return;
    cJSON* features = cJSON_GetObjectItem(audio_hello, "features");
    if (!cJSON_IsObject(features)) {
        cJSON_DeleteItemFromObject(audio_hello, "features");
        features = cJSON_AddObjectToObject(audio_hello, "features");
    }
    cJSON_DeleteItemFromObject(features, "stackchan_ctrl");
    cJSON_AddNumberToObject(features, "stackchan_ctrl", 1);
}

cJSON* BuildCtrlHello(uint64_t e) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "role", "control");
    cJSON_AddNumberToObject(root, "audio_epoch", static_cast<double>(e));
    return root;
}

cJSON* BuildReady(uint64_t e) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ready");
    cJSON_AddNumberToObject(root, "audio_epoch", static_cast<double>(e));
    return root;
}

cJSON* BuildAbortDone(const AbortRequest& req, bool already, uint32_t dropped_ms) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "abort");
    cJSON_AddStringToObject(root, "state", "done");
    cJSON_AddStringToObject(root, "req_id", req.req_id.c_str());
    cJSON_AddStringToObject(root, "reason", req.reason.c_str());
    cJSON_AddNumberToObject(root, "gen", static_cast<double>(req.gen));
    cJSON_AddStringToObject(root, "result", already ? "already" : "stopped");
    cJSON_AddNumberToObject(root, "dropped_ms", already ? 0.0 : static_cast<double>(dropped_ms));
    return root;
}

cJSON* BuildDeviceAbort(const std::string& session_id, uint32_t gen, uint32_t dev_abort_seq,
                        DeviceAbortReason reason) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id.c_str());
    cJSON_AddStringToObject(root, "type", "abort");
    cJSON_AddNumberToObject(root, "gen", static_cast<double>(gen));
    cJSON_AddNumberToObject(root, "dev_abort_seq", static_cast<double>(dev_abort_seq));
    if (reason == DeviceAbortReason::kWakeWord) {
        cJSON_AddStringToObject(root, "reason", "wake_word_detected");
    }
    return root;
}

cJSON* BuildListenStart(const std::string& session_id, ListenMode mode) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id.c_str());
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "start");
    cJSON_AddStringToObject(root, "mode", ModeName(mode));
    return root;
}

cJSON* BuildListenStop(const std::string& session_id) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id.c_str());
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "stop");
    return root;
}

cJSON* BuildStatReply(const std::string& req_id, const std::vector<StatGroup>& groups) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "stat");
    cJSON_AddStringToObject(root, "state", "done");
    cJSON_AddStringToObject(root, "req_id", req_id.c_str());
    for (const StatGroup& g : groups) {
        cJSON* obj = cJSON_AddObjectToObject(root, g.name);
        for (const StatItem& i : g.items) cJSON_AddNumberToObject(obj, i.name, static_cast<double>(i.value));
    }
    return root;
}

void CtrlStamp::Stamp(cJSON* root) {
    if (!cJSON_IsObject(root)) return;
    ++seq_;
    cJSON_DeleteItemFromObject(root, "fw_epoch");
    cJSON_DeleteItemFromObject(root, "seq");
    cJSON_AddNumberToObject(root, "fw_epoch", static_cast<double>(e_));
    cJSON_AddNumberToObject(root, "seq", static_cast<double>(seq_));
}

}  // namespace stackchan::wire
