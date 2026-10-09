// StackChan FW-A2 plan 2B-2a: the receive side's app ports (see app_link.h).
#include "app_link.h"

#include <cJSON.h>

#include <cstring>
#include <utility>

#include "tx_loop.h"
#include "wire.h"

namespace stackchan::link {

namespace {

const char* Str(const cJSON* root, const char* key) {
    const cJSON* v = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}

}  // namespace

AppLink::AppLink(AppDeps deps) : d_(std::move(deps)) {}

const char* AppLink::Missing() const {
    if (d_.gate == nullptr) return "gate";
    if (d_.ctrl_queue == nullptr) return "ctrl_queue";
    if (d_.notices == nullptr) return "notices";
    if (!d_.now_us) return "now_us";
    if (!d_.apply_server_time) return "apply_server_time";
    if (!d_.on_audio_only) return "on_audio_only";
    if (!d_.push_server_audio) return "push_server_audio";
    if (!d_.post_app) return "post_app";
    if (!d_.mcp) return "mcp";
    if (!d_.stat_inputs) return "stat_inputs";
    return nullptr;
}

RxPorts AppLink::Ports() {
    RxPorts p;
    p.hello_reply = [this](const AudioHelloReply& r, const cJSON* root) { OnHelloReply(r, root); };
    p.server_audio = [this](uint64_t e, AudioIn audio) { OnServerAudio(e, std::move(audio)); };
    p.app_json = [this](uint64_t e, const cJSON* root) { OnAppJson(e, root); };
    p.stat = [this](uint64_t e, const std::string& req_id) { OnStat(e, req_id); };
    return p;
}

AppStats AppLink::stats() const {
    AppStats s;
    s.mcp_unbound = mcp_unbound_.load();
    s.unknown = unknown_.load();
    s.stat_full = stat_full_.load();
    s.stat_closed = stat_closed_.load();
    return s;
}

// The audio hello reply (once per link): the server's audio parameters for the packets that follow,
// its clock, and the audio-only pair's power save wake-up.
void AppLink::OnHelloReply(const AudioHelloReply& reply, const cJSON* root) {
    if (reply.sample_rate > 0) sample_rate_.store(reply.sample_rate);
    if (reply.frame_duration > 0) frame_duration_.store(reply.frame_duration);
    const cJSON* server_time = cJSON_GetObjectItem(root, "server_time");
    if (server_time != nullptr) d_.apply_server_time(server_time);
    if (!reply.ctrl_offered) d_.on_audio_only();
}

// R4: the gate decides under its lock; the packet is made and queued only when it takes it
// (gate_mutex_ -> audio_queue_mutex_, design §1.2).
void AppLink::OnServerAudio(uint64_t e, AudioIn audio) {
    d_.gate->OnServerAudio(e, [&]() {
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate_.load();
        packet->frame_duration = frame_duration_.load();
        packet->timestamp = audio.timestamp;
        packet->payload.assign(audio.payload.begin(), audio.payload.end());
        return d_.push_server_audio(packet);
    });
}

void AppLink::OnAppJson(uint64_t e, const cJSON* root) {
    const std::string type = wire::TypeOf(root);
    AppMessage m;
    if (type == "tts") {
        const char* state = Str(root, "state");
        const char* text = Str(root, "text");
        if (state == nullptr || std::strcmp(state, "sentence_start") != 0 || text == nullptr) {
            unknown_++;  // the gate took start / stop; other tts states have nothing to show
            return;
        }
        m.kind = AppMessage::Kind::kAssistantText;
        m.text = text;
    } else if (type == "stt") {
        const char* text = Str(root, "text");
        if (text == nullptr) {
            unknown_++;
            return;
        }
        m.kind = AppMessage::Kind::kUserText;
        m.text = text;
    } else if (type == "llm") {
        const char* emotion = Str(root, "emotion");
        if (emotion == nullptr) {
            unknown_++;
            return;
        }
        m.kind = AppMessage::Kind::kEmotion;
        m.emotion = emotion;
    } else if (type == "alert") {
        const char* status = Str(root, "status");
        const char* message = Str(root, "message");
        const char* emotion = Str(root, "emotion");
        if (status == nullptr || message == nullptr || emotion == nullptr) {
            unknown_++;
            return;
        }
        m.kind = AppMessage::Kind::kAlert;
        m.status = status;
        m.text = message;
        m.emotion = emotion;
    } else if (type == "system") {
        const char* command = Str(root, "command");
        if (command == nullptr || std::strcmp(command, "reboot") != 0) {
            unknown_++;
            return;
        }
        m.kind = AppMessage::Kind::kReboot;
    } else if (type == "custom" && d_.receive_custom) {
        const cJSON* payload = cJSON_GetObjectItem(root, "payload");
        char* text = cJSON_IsObject(payload) ? cJSON_PrintUnformatted(payload) : nullptr;
        if (text == nullptr) {
            unknown_++;
            return;
        }
        m.kind = AppMessage::Kind::kCustom;
        m.text = text;
        cJSON_free(text);
    } else if (type == "avatar_set_fetch") {
        if (!d_.avatar_set_fetch) {  // a board without an avatar
            unknown_++;
            return;
        }
        d_.avatar_set_fetch(root);
        return;
    } else if (type == "mcp") {
        const cJSON* payload = cJSON_GetObjectItem(root, "payload");
        const gate::State g = d_.gate->Snapshot();
        if (!cJSON_IsObject(payload) || e == 0 || g.bound_e != e || g.dead) {  // design §4.1
            mcp_unbound_++;
            return;
        }
        d_.mcp(e, payload);
        return;
    } else {
        unknown_++;
        return;
    }
    d_.post_app(std::move(m));
}

// The reply goes into the control queue with the link's E; a full queue ends the pair (design §4.1)
void AppLink::OnStat(uint64_t e, const std::string& req_id) {
    const std::string reply = BuildStat(req_id, d_.stat_inputs());
    const net::PushResult r = QueueJsonOrEnd(
        *d_.ctrl_queue, e, reply, d_.now_us(),
        [g = d_.gate](uint64_t pair, EndReason reason) { g->StopForDeath(pair, reason); },
        [n = d_.notices](const Input& in) { return n->Post(in); });
    if (r == net::PushResult::kFull) stat_full_++;
    if (r == net::PushResult::kClosed) stat_closed_++;
}

}  // namespace stackchan::link
