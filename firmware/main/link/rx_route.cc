// StackChan FW-A2 plan 2B-1: what the receive tasks decide (see rx_route.h).
#include "rx_route.h"

#include <cJSON.h>

#include <cstring>

namespace stackchan::link {

namespace {

uint32_t Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void PutBe16(std::string* s, uint16_t v) {
    s->push_back(static_cast<char>(v >> 8));
    s->push_back(static_cast<char>(v & 0xFF));
}

void PutBe32(std::string* s, uint32_t v) {
    PutBe16(s, static_cast<uint16_t>(v >> 16));
    PutBe16(s, static_cast<uint16_t>(v & 0xFFFF));
}

constexpr size_t kHeader2 = 16;  // version 2, type 2, reserved 4, timestamp 4, payload size 4
constexpr size_t kHeader3 = 4;   // type 1, reserved 1, payload size 2

const char* StateOf(const cJSON* root) {
    const cJSON* state = cJSON_GetObjectItem(root, "state");
    return cJSON_IsString(state) ? state->valuestring : "";
}

AudioRouted Drop(const char* why) {
    AudioRouted r;
    r.route = AudioRoute::kDrop;
    r.why = why;
    return r;
}

CtrlRouted CtrlDrop(const char* why) {
    CtrlRouted r;
    r.route = CtrlRoute::kDrop;
    r.why = why;
    return r;
}

}  // namespace

bool DecodeAudioFrame(int version, const uint8_t* data, size_t len, AudioIn* out) {
    out->timestamp = 0;
    if (version == 2) {
        if (len < kHeader2) return false;
        const uint32_t size = Be32(data + 12);
        if (size > len - kHeader2) return false;
        out->timestamp = Be32(data + 8);
        out->payload.assign(reinterpret_cast<const char*>(data + kHeader2), size);
        return true;
    }
    if (version == 3) {
        if (len < kHeader3) return false;
        const uint32_t size = (static_cast<uint32_t>(data[2]) << 8) | data[3];
        if (size > len - kHeader3) return false;
        out->payload.assign(reinterpret_cast<const char*>(data + kHeader3), size);
        return true;
    }
    out->payload.assign(reinterpret_cast<const char*>(data), len);
    return true;
}

std::string EncodeAudioFrame(int version, uint32_t timestamp, const uint8_t* opus, size_t len) {
    std::string f;
    if (version == 2) {
        PutBe16(&f, 2);  // version
        PutBe16(&f, 0);  // type: opus
        PutBe32(&f, 0);  // reserved
        PutBe32(&f, timestamp);
        PutBe32(&f, static_cast<uint32_t>(len));
    } else if (version == 3) {
        f.push_back(0);  // type: opus
        f.push_back(0);  // reserved
        PutBe16(&f, static_cast<uint16_t>(len));
    }
    f.append(reinterpret_cast<const char*>(opus), len);
    return f;
}

bool ParseAudioHelloReply(const cJSON* root, AudioHelloReply* out) {
    const cJSON* transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "websocket") != 0) return false;
    const cJSON* session = cJSON_GetObjectItem(root, "session_id");
    if (!cJSON_IsString(session) || session->valuestring == nullptr || session->valuestring[0] == '\0') {
        return false;
    }
    out->session_id = session->valuestring;
    out->ctrl_offered = wire::CtrlOffered(root);
    out->sample_rate = 0;
    out->frame_duration = 0;
    const cJSON* params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(params)) {
        const cJSON* rate = cJSON_GetObjectItem(params, "sample_rate");
        if (cJSON_IsNumber(rate)) out->sample_rate = rate->valueint;
        const cJSON* frame = cJSON_GetObjectItem(params, "frame_duration");
        if (cJSON_IsNumber(frame)) out->frame_duration = frame->valueint;
    }
    return true;
}

AudioRouted RouteAudioText(const cJSON* root, const std::string& session_id) {
    if (!cJSON_IsObject(root)) return Drop("not a JSON object");
    const std::string type = wire::TypeOf(root);
    if (type.empty()) return Drop("no type");
    if (type == "hello") {
        AudioRouted r;
        r.route = AudioRoute::kHelloReply;
        return r;
    }
    if (type == "abort") return Drop("an abort request on the audio link (control link only)");
    if (type == "tts" || type == "listen") {
        if (!wire::SessionMatches(root, session_id)) return Drop("session_id mismatch or missing");
        AudioRouted r;
        if (type == "listen") {
            if (!wire::ParseGwListen(root, &r.listen)) return Drop("broken listen");
            r.route = AudioRoute::kListen;
            return r;
        }
        const char* state = StateOf(root);
        if (strcmp(state, "start") == 0) {
            if (!wire::ParseTtsStart(root, &r.tts_start)) return Drop("broken tts start");
            r.route = AudioRoute::kTtsStart;
            return r;
        }
        if (strcmp(state, "stop") == 0) {
            if (!wire::ParseTtsStop(root, &r.tts_stop_gen)) return Drop("broken tts stop");
            r.route = AudioRoute::kTtsStop;
            return r;
        }
        if (state[0] == '\0') return Drop("tts without a state");
        r.route = AudioRoute::kTtsOther;
        return r;
    }
    AudioRouted r;
    r.route = AudioRoute::kApp;
    return r;
}

CtrlRouted RouteCtrlText(const cJSON* root, const std::string& session_id) {
    if (!cJSON_IsObject(root)) return CtrlDrop("not a JSON object");
    const std::string type = wire::TypeOf(root);
    CtrlRouted r;
    if (type == "hello") {
        if (!wire::ParseCtrlHelloReply(root, &r.audio_epoch)) return CtrlDrop("broken control hello reply");
        r.route = CtrlRoute::kHelloReply;
        return r;
    }
    if (type == "abort") {
        if (!wire::ParseAbortRequest(root, &r.abort)) return CtrlDrop("not an abort request");
        r.route = CtrlRoute::kAbort;
        return r;
    }
    if (type == "stat") {
        if (!wire::SessionMatches(root, session_id)) return CtrlDrop("stat: session_id mismatch or missing");
        if (!wire::ParseStatRequest(root, &r.req_id)) return CtrlDrop("not a stat request");
        r.route = CtrlRoute::kStat;
        return r;
    }
    return CtrlDrop("not for the control link");
}

}  // namespace stackchan::link
