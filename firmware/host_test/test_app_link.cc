// StackChan FW-A2 plan 2B-2a: the receive side's app ports (plan 2B-1 handoff 2) with the real gate,
// send queue and notice queue, and fake application ports.
#include <gtest/gtest.h>

#include <cJSON.h>

#include <memory>
#include <string>
#include <vector>

#include "app_link.h"

namespace g = stackchan::gate;
namespace l = stackchan::link;
namespace n = stackchan::net;
namespace w = stackchan::wire;
using l::AppMessage;

namespace {

constexpr uint64_t E = (uint64_t{1} << 33) + 9;

struct NullAudio : g::AudioSink {
    uint32_t Stop(uint32_t) override { return 0; }
    void Clear() override {}
};

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json Parse(const std::string& s) { return Json(cJSON_Parse(s.c_str()), &cJSON_Delete); }

struct Rig {
    NullAudio audio;
    n::SendQueue audio_q{n::kAudioLimits};
    n::SendQueue ctrl_q{n::kCtrlLimits};
    l::NoticeQueue notices;
    std::vector<l::EndReason> deaths;
    g::PlaybackGate gate{g::GatePorts{&audio, &audio_q, &ctrl_q, [] { return int64_t{5}; },
                                      [](uint64_t, bool, uint32_t) {}, [](uint64_t, bool, uint32_t) {},
                                      [this](uint64_t, l::EndReason r, bool) { deaths.push_back(r); }}};
    std::vector<std::string> server_times;
    int audio_only = 0;
    bool push_ok = true;
    std::vector<AudioStreamPacket> pushed;
    std::vector<AppMessage> posted;
    std::vector<std::string> avatars;
    std::vector<std::pair<uint64_t, std::string>> mcps;
    int stat_reads = 0;
    l::AppDeps deps;
    std::unique_ptr<l::AppLink> app;

    explicit Rig(bool receive_custom = false) {
        l::AppDeps d;
        d.gate = &gate;
        d.ctrl_queue = &ctrl_q;
        d.notices = &notices;
        d.now_us = [] { return int64_t{1000}; };
        d.apply_server_time = [this](const cJSON* t) {
            char* s = cJSON_PrintUnformatted(t);
            server_times.push_back(s);
            cJSON_free(s);
        };
        d.on_audio_only = [this] { audio_only++; };
        d.push_server_audio = [this](std::unique_ptr<AudioStreamPacket>& p) {
            if (!push_ok) return false;
            pushed.push_back(*p);
            return true;
        };
        d.post_app = [this](AppMessage m) { posted.push_back(std::move(m)); };
        d.avatar_set_fetch = [this](const cJSON* root) { avatars.push_back(w::TypeOf(root)); };
        d.mcp = [this](uint64_t e, const cJSON* payload) {
            char* s = cJSON_PrintUnformatted(payload);
            mcps.push_back({e, s});
            cJSON_free(s);
        };
        d.stat_inputs = [this] {
            stat_reads++;
            l::StatInputs in;
            in.gate = gate.Snapshot();
            in.build = "b";
            return in;
        };
        d.receive_custom = receive_custom;
        deps = d;
        app = std::make_unique<l::AppLink>(d);
    }
    void Bind() {
        audio_q.Open(E);
        ctrl_q.Open(E);
        ASSERT_EQ(gate.Bind(E, "s"), g::Outcome::kBound);
    }
    void Json(const std::string& text, uint64_t e = E) {
        auto root = Parse(text);
        ASSERT_NE(root, nullptr) << text;
        app->Ports().app_json(e, root.get());
    }
};

l::AudioHelloReply Reply(int rate, int frame, bool ctrl) {
    l::AudioHelloReply r;
    r.session_id = "s";
    r.sample_rate = rate;
    r.frame_duration = frame;
    r.ctrl_offered = ctrl;
    return r;
}

}  // namespace

TEST(AppLink, TheHelloReplySetsTheAudioParametersAndTheClock) {
    Rig rig;
    EXPECT_EQ(rig.app->sample_rate(), 24000);  // today's defaults
    EXPECT_EQ(rig.app->frame_duration(), 60);
    auto root = Parse(R"({"type":"hello","server_time":{"utc_ms":1,"offset_min":540}})");
    rig.app->Ports().hello_reply(Reply(16000, 20, true), root.get());
    EXPECT_EQ(rig.app->sample_rate(), 16000);
    EXPECT_EQ(rig.app->frame_duration(), 20);
    ASSERT_EQ(rig.server_times.size(), 1u);
    EXPECT_EQ(rig.server_times[0], R"({"utc_ms":1,"offset_min":540})");
    EXPECT_EQ(rig.audio_only, 0);
    auto bare = Parse(R"({"type":"hello"})");
    rig.app->Ports().hello_reply(Reply(0, 0, false), bare.get());  // no audio_params: kept
    EXPECT_EQ(rig.app->sample_rate(), 16000);
    EXPECT_EQ(rig.app->frame_duration(), 20);
    EXPECT_EQ(rig.server_times.size(), 1u);
    EXPECT_EQ(rig.audio_only, 1);  // no control link offered: wake the power save timer anyway
}

TEST(AppLink, ServerAudioIsMadeOnlyWhenTheGateTakesIt) {
    Rig rig;
    rig.Bind();
    auto root = Parse(R"({"type":"hello"})");
    rig.app->Ports().hello_reply(Reply(16000, 40, true), root.get());
    l::AudioIn in;
    in.timestamp = 77;
    in.payload = std::string("\x01\x02\x03", 3);
    rig.app->Ports().server_audio(E, in);  // not speaking: the gate drops it
    EXPECT_TRUE(rig.pushed.empty());
    rig.gate.OnTtsStart(E, {1, 0, 0});
    rig.app->Ports().server_audio(E, in);
    ASSERT_EQ(rig.pushed.size(), 1u);
    EXPECT_EQ(rig.pushed[0].sample_rate, 16000);
    EXPECT_EQ(rig.pushed[0].frame_duration, 40);
    EXPECT_EQ(rig.pushed[0].timestamp, 77u);
    EXPECT_EQ(rig.pushed[0].payload, (std::vector<uint8_t>{1, 2, 3}));
    rig.app->Ports().server_audio(E + 1, in);  // another pair
    EXPECT_EQ(rig.pushed.size(), 1u);
    const uint32_t dropped = rig.gate.Stats().dropped_server;
    rig.push_ok = false;  // the decode queue is full: the gate counts it
    rig.app->Ports().server_audio(E, in);
    EXPECT_EQ(rig.gate.Stats().dropped_server, dropped + 1);
}

TEST(AppLink, TheAppsJsonGoesToTheMainTask) {
    Rig rig;
    rig.Json(R"({"type":"tts","state":"sentence_start","text":"hi"})");
    rig.Json(R"({"type":"stt","text":"yo"})");
    rig.Json(R"({"type":"llm","emotion":"happy"})");
    rig.Json(R"({"type":"alert","status":"s","message":"m","emotion":"e"})");
    rig.Json(R"({"type":"system","command":"reboot"})");
    ASSERT_EQ(rig.posted.size(), 5u);
    EXPECT_EQ(rig.posted[0].kind, AppMessage::Kind::kAssistantText);
    EXPECT_EQ(rig.posted[0].text, "hi");
    EXPECT_EQ(rig.posted[1].kind, AppMessage::Kind::kUserText);
    EXPECT_EQ(rig.posted[1].text, "yo");
    EXPECT_EQ(rig.posted[2].kind, AppMessage::Kind::kEmotion);
    EXPECT_EQ(rig.posted[2].emotion, "happy");
    EXPECT_EQ(rig.posted[3].kind, AppMessage::Kind::kAlert);
    EXPECT_EQ(rig.posted[3].status, "s");
    EXPECT_EQ(rig.posted[3].text, "m");
    EXPECT_EQ(rig.posted[3].emotion, "e");
    EXPECT_EQ(rig.posted[4].kind, AppMessage::Kind::kReboot);
    EXPECT_EQ(rig.app->stats().unknown, 0u);
}

TEST(AppLink, BrokenOrUnknownAppJsonIsCountedNotPosted) {
    Rig rig;
    for (const char* text : {R"({"type":"tts","state":"start"})", R"({"type":"tts","state":"sentence_start"})",
                             R"({"type":"tts","state":"sentence_end","text":"x"})", R"({"type":"stt"})",
                             R"({"type":"llm","emotion":3})", R"({"type":"alert","status":"s","message":"m"})",
                             R"({"type":"alert","status":"s","emotion":"e"})", R"({"type":"alert","message":"m","emotion":"e"})",
                             R"({"type":"system","command":"halt"})", R"({"type":"system"})",
                             R"({"type":"custom","payload":{"a":1}})", R"({"type":"nope"})", R"({"x":1})"}) {
        rig.Json(text);
    }
    EXPECT_TRUE(rig.posted.empty());
    EXPECT_EQ(rig.app->stats().unknown, 13u);
}

TEST(AppLink, CustomOnlyWhenTheBuildReceivesIt) {
    Rig rig(true);
    rig.Json(R"({"type":"custom","payload":{"a":1}})");
    rig.Json(R"({"type":"custom","payload":"x"})");
    ASSERT_EQ(rig.posted.size(), 1u);
    EXPECT_EQ(rig.posted[0].kind, AppMessage::Kind::kCustom);
    EXPECT_EQ(rig.posted[0].text, R"({"a":1})");
    EXPECT_EQ(rig.app->stats().unknown, 1u);
}

TEST(AppLink, TheAvatarFetchStaysWithTheBoard) {
    Rig rig;
    rig.Json(R"({"type":"avatar_set_fetch","url":"x"})");
    EXPECT_EQ(rig.avatars, (std::vector<std::string>{"avatar_set_fetch"}));
    EXPECT_TRUE(rig.posted.empty());
}

// Design §4.1: mcp on the bound pair only, with that pair's E
TEST(AppLink, McpOnlyOnTheBoundPair) {
    Rig rig;
    const std::string mcp = R"({"type":"mcp","payload":{"id":1}})";
    rig.Json(mcp);  // no pair yet
    rig.Bind();
    rig.Json(mcp, E + 1);  // another pair
    rig.Json(mcp, 0);
    rig.Json(R"({"type":"mcp","payload":3})");
    rig.Json(mcp);
    ASSERT_EQ(rig.mcps.size(), 1u);
    EXPECT_EQ(rig.mcps[0].first, E);
    EXPECT_EQ(rig.mcps[0].second, R"({"id":1})");
    rig.gate.StopForDeath(E, l::EndReason::kF1);  // dead: no more
    rig.Json(mcp);
    EXPECT_EQ(rig.mcps.size(), 1u);
    EXPECT_EQ(rig.app->stats().mcp_unbound, 5u);
}

TEST(AppLink, StatGoesToTheControlQueueWithTheLinksE) {
    Rig rig;
    rig.Bind();
    rig.app->Ports().stat(E, "r-7");
    EXPECT_EQ(rig.stat_reads, 1);
    auto elem = rig.ctrl_q.Pop(0);
    ASSERT_TRUE(elem.has_value());
    EXPECT_EQ(elem->e, E);
    auto root = Parse(elem->payload);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(w::TypeOf(root.get()), "stat");
    EXPECT_STREQ(cJSON_GetObjectItem(root.get(), "req_id")->valuestring, "r-7");
    const cJSON* pair = cJSON_GetObjectItem(root.get(), "pair");
    ASSERT_TRUE(cJSON_IsObject(pair));
    EXPECT_EQ(static_cast<uint64_t>(cJSON_GetObjectItem(pair, "audio_epoch")->valuedouble), E);
}

// Design §4.1: a JSON that does not fit ends the pair; a closed queue (the pair ended) only counts
TEST(AppLink, AStatThatDoesNotFitEndsThePair) {
    Rig rig;
    rig.Bind();
    for (int i = 0; i < 64; i++) {
        if (rig.ctrl_q.Push(E, n::ElemKind::kJson, std::string(400, 'x'), 0) != n::PushResult::kQueued) break;
    }
    rig.app->Ports().stat(E, "r-8");
    EXPECT_EQ(rig.app->stats().stat_full, 1u);
    EXPECT_EQ(rig.deaths, (std::vector<l::EndReason>{l::EndReason::kQueueFull}));
    auto notice = rig.notices.Take(0);
    ASSERT_TRUE(notice.has_value());
    EXPECT_EQ(notice->kind, l::InKind::kEndRequest);
    EXPECT_EQ(notice->e, E);
    EXPECT_EQ(notice->reason, l::EndReason::kQueueFull);

    Rig closed;
    closed.Bind();
    closed.ctrl_q.Close();
    closed.app->Ports().stat(E, "r-9");
    EXPECT_EQ(closed.app->stats().stat_closed, 1u);
    EXPECT_EQ(closed.app->stats().stat_full, 0u);
    EXPECT_TRUE(closed.deaths.empty());
    EXPECT_FALSE(closed.notices.Take(0).has_value());
}

// Claude review 156 Minor 5: an empty dependency is named before the link starts
TEST(AppLink, MissingDependenciesAreNamed) {
    Rig rig;
    EXPECT_EQ(rig.app->Missing(), nullptr);
    const std::vector<std::pair<std::string, void (*)(l::AppDeps&)>> cases = {
        {"gate", [](l::AppDeps& d) { d.gate = nullptr; }},
        {"ctrl_queue", [](l::AppDeps& d) { d.ctrl_queue = nullptr; }},
        {"notices", [](l::AppDeps& d) { d.notices = nullptr; }},
        {"now_us", [](l::AppDeps& d) { d.now_us = nullptr; }},
        {"apply_server_time", [](l::AppDeps& d) { d.apply_server_time = nullptr; }},
        {"on_audio_only", [](l::AppDeps& d) { d.on_audio_only = nullptr; }},
        {"push_server_audio", [](l::AppDeps& d) { d.push_server_audio = nullptr; }},
        {"post_app", [](l::AppDeps& d) { d.post_app = nullptr; }},
        {"mcp", [](l::AppDeps& d) { d.mcp = nullptr; }},
        {"stat_inputs", [](l::AppDeps& d) { d.stat_inputs = nullptr; }},
    };
    for (const auto& [name, clear] : cases) {
        l::AppDeps d = rig.deps;
        clear(d);
        l::AppLink app(d);
        ASSERT_NE(app.Missing(), nullptr) << name;
        EXPECT_EQ(std::string(app.Missing()), name);
    }
    l::AppDeps no_avatar = rig.deps;  // a board without an avatar: allowed, the message is counted
    no_avatar.avatar_set_fetch = nullptr;
    l::AppLink app(no_avatar);
    EXPECT_EQ(app.Missing(), nullptr);
    auto root = Parse(R"({"type":"avatar_set_fetch"})");
    app.Ports().app_json(E, root.get());
    EXPECT_EQ(app.stats().unknown, 1u);
}
