// StackChan FW-A2 plan 2B-2b (design §4.1, plan 2B-2a handoff 7): what the device sends on its own
// (the MCP replies, stackchan-event, SendJsonString) with the real gate and send queue.
#include <gtest/gtest.h>

#include <cJSON.h>

#include <memory>
#include <string>
#include <vector>

#include "outbound.h"

namespace g = stackchan::gate;
namespace l = stackchan::link;
namespace n = stackchan::net;

namespace {

constexpr uint64_t E1 = (uint64_t{1} << 33) + 9;
constexpr uint64_t E2 = E1 + 1;

struct NullAudio : g::AudioSink {
    uint32_t Stop(uint32_t) override { return 0; }
    void Clear() override {}
};

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json Parse(const std::string& s) { return Json(cJSON_Parse(s.c_str()), &cJSON_Delete); }

std::string Str(const cJSON* root, const char* key) {
    const cJSON* v = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(v) ? v->valuestring : "<missing>";
}

struct Rig {
    NullAudio audio;
    n::SendQueue audio_q{n::kAudioLimits};
    n::SendQueue ctrl_q{n::kCtrlLimits};
    l::NoticeQueue notices;
    std::vector<l::EndReason> deaths;
    g::PlaybackGate gate{g::GatePorts{&audio, &audio_q, &ctrl_q, [] { return int64_t{5}; },
                                      [](uint64_t, bool, uint32_t) {}, [](uint64_t, bool, uint32_t) {},
                                      [this](uint64_t, l::EndReason r, bool) { deaths.push_back(r); }}};
    uint64_t bound = 0;  // LinkHub::BoundPair
    std::unique_ptr<l::Outbound> out;

    Rig() {
        l::OutboundDeps d;
        d.gate = &gate;
        d.audio_queue = &audio_q;
        d.notices = &notices;
        d.now_us = [] { return int64_t{1000}; };
        d.bound_pair = [this] { return bound; };
        out = std::make_unique<l::Outbound>(d);
    }
    void Bind(uint64_t e, const std::string& session) {
        audio_q.Open(e);
        ctrl_q.Open(e);
        ASSERT_EQ(gate.Bind(e, session), g::Outcome::kBound);
        bound = e;
    }
    void End(uint64_t e) {
        ASSERT_EQ(gate.Unbind(e), g::Outcome::kReset);
        audio_q.Close();
        ctrl_q.Close();
        bound = 0;
    }
    // The next element of the audio queue as JSON (null when none)
    Json Next(uint64_t* e = nullptr) {
        auto el = audio_q.Pop(0);
        if (!el) return Json(nullptr, &cJSON_Delete);
        EXPECT_EQ(el->kind, n::ElemKind::kJson);
        if (e) *e = el->e;
        return Parse(el->payload);
    }
};

}  // namespace

TEST(Outbound, AnMcpReplyGoesOutOnTheRequestsPairWithItsSession) {
    Rig rig;
    rig.Bind(E1, "s1");
    EXPECT_EQ(rig.out->McpReply(E1, R"({"jsonrpc":"2.0","id":3,"result":{"ok":true}})"),
              l::OutResult::kQueued);
    uint64_t e = 0;
    auto root = rig.Next(&e);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(e, E1);
    EXPECT_EQ(Str(root.get(), "session_id"), "s1");
    EXPECT_EQ(Str(root.get(), "type"), "mcp");
    const cJSON* payload = cJSON_GetObjectItem(root.get(), "payload");
    ASSERT_TRUE(cJSON_IsObject(payload));
    EXPECT_EQ(cJSON_GetObjectItem(payload, "id")->valueint, 3);
    EXPECT_EQ(rig.out->stats().unbound, 0u);
}

TEST(Outbound, AnEarlierPairsReplyIsDroppedNeverRelabelled) {
    // The request came on E1; its reply is made after E1 ended and E2 was bound (design §4.1):
    // the reply must not go out with E2's session.
    Rig rig;
    rig.Bind(E1, "s1");
    rig.End(E1);
    rig.Bind(E2, "s2");
    EXPECT_EQ(rig.out->McpReply(E1, R"({"jsonrpc":"2.0","id":1,"result":{}})"), l::OutResult::kUnbound);
    EXPECT_EQ(rig.Next(), nullptr);
    EXPECT_EQ(rig.out->stats().unbound, 1u);
    // E2's own reply goes out with s2
    EXPECT_EQ(rig.out->McpReply(E2, R"({"jsonrpc":"2.0","id":2,"result":{}})"), l::OutResult::kQueued);
    auto root = rig.Next();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(Str(root.get(), "session_id"), "s2");
}

TEST(Outbound, NoPairOrADeadPairDropsTheReply) {
    Rig rig;
    EXPECT_EQ(rig.out->McpReply(0, R"({"id":1})"), l::OutResult::kUnbound);
    EXPECT_EQ(rig.out->McpReply(E1, R"({"id":1})"), l::OutResult::kUnbound);  // nothing bound
    rig.Bind(E1, "s1");
    rig.gate.StopForDeath(E1, l::EndReason::kF1);  // the pair is ending: dead
    EXPECT_EQ(rig.out->McpReply(E1, R"({"id":1})"), l::OutResult::kUnbound);
    EXPECT_EQ(rig.Next(), nullptr);
    EXPECT_EQ(rig.out->stats().unbound, 3u);
}

TEST(Outbound, AReplyThatIsNotJsonIsDropped) {
    Rig rig;
    rig.Bind(E1, "s1");
    EXPECT_EQ(rig.out->McpReply(E1, "not json"), l::OutResult::kBadJson);
    EXPECT_EQ(rig.Next(), nullptr);
    EXPECT_EQ(rig.out->stats().bad_json, 1u);
}

TEST(Outbound, AClosedQueueIsCountedAndAFullOneEndsThePair) {
    Rig rig;
    rig.Bind(E1, "s1");
    rig.audio_q.Close();  // the manager closed it; the gate still holds E1 for a moment
    EXPECT_EQ(rig.out->McpReply(E1, R"({"id":1})"), l::OutResult::kClosed);
    EXPECT_EQ(rig.out->stats().closed, 1u);
    EXPECT_TRUE(rig.deaths.empty());

    Rig full;
    full.Bind(E1, "s1");
    l::OutResult r = l::OutResult::kQueued;
    for (int i = 0; i < 100 && r == l::OutResult::kQueued; i++) {
        r = full.out->McpReply(E1, R"({"id":1})");
    }
    EXPECT_EQ(r, l::OutResult::kFull);  // JSON is never dropped: the pair ends (design §4.1)
    ASSERT_EQ(full.deaths.size(), 1u);
    EXPECT_EQ(full.deaths[0], l::EndReason::kQueueFull);
    auto in = full.notices.Take(0);
    ASSERT_TRUE(in.has_value());
    EXPECT_EQ(in->kind, l::InKind::kEndRequest);
    EXPECT_EQ(in->e, E1);
}

TEST(Outbound, AnEventGoesOutOnThePairBoundAtSendTime) {
    Rig rig;
    EXPECT_EQ(rig.out->StackChanEvent("touch", "tap", 120, 777), l::OutResult::kUnbound);
    rig.Bind(E1, "s1");
    EXPECT_EQ(rig.out->StackChanEvent("touch", "stroke", 450, 778), l::OutResult::kQueued);
    uint64_t e = 0;
    auto root = rig.Next(&e);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(e, E1);
    EXPECT_EQ(Str(root.get(), "session_id"), "s1");
    EXPECT_EQ(Str(root.get(), "type"), "stackchan-event");
    EXPECT_EQ(Str(root.get(), "event_type"), "touch");
    EXPECT_EQ(Str(root.get(), "subtype"), "stroke");
    EXPECT_EQ(cJSON_GetObjectItem(root.get(), "duration_ms")->valuedouble, 450);
    EXPECT_EQ(cJSON_GetObjectItem(root.get(), "ts")->valuedouble, 778);
    EXPECT_EQ(rig.out->stats().unbound, 1u);
}

TEST(Outbound, ABoundPairTheGateNoLongerHoldsIsUnbound) {
    // BoundPair is the manager's copy; it may lag behind the gate's Unbind (design §1.2)
    Rig rig;
    rig.Bind(E1, "s1");
    ASSERT_EQ(rig.gate.Unbind(E1), g::Outcome::kReset);  // the copy still says E1
    EXPECT_EQ(rig.out->StackChanEvent("touch", "tap", 1, 2), l::OutResult::kUnbound);
    EXPECT_EQ(rig.out->JsonString(R"({"type":"avatar_set_loaded"})"), l::OutResult::kUnbound);
    EXPECT_EQ(rig.Next(), nullptr);
}

TEST(Outbound, AJsonStringGoesOutAsItIs) {
    Rig rig;
    rig.Bind(E1, "s1");
    EXPECT_EQ(rig.out->JsonString(R"({"type":"avatar_set_loaded","set_id":"a"})"), l::OutResult::kQueued);
    auto root = rig.Next();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(Str(root.get(), "type"), "avatar_set_loaded");
    EXPECT_EQ(Str(root.get(), "set_id"), "a");
    EXPECT_EQ(Str(root.get(), "session_id"), "<missing>");  // today's SendJsonString adds none
    EXPECT_EQ(rig.out->JsonString("[1,2]"), l::OutResult::kBadJson);  // not an object
    EXPECT_EQ(rig.out->JsonString("{"), l::OutResult::kBadJson);
    EXPECT_EQ(rig.Next(), nullptr);
    EXPECT_EQ(rig.out->stats().bad_json, 2u);
}

TEST(Outbound, SessionForIsThePairsOwnOnly) {
    Rig rig;
    EXPECT_FALSE(rig.gate.SessionFor(0).has_value());
    EXPECT_FALSE(rig.gate.SessionFor(E1).has_value());
    rig.Bind(E1, "s1");
    ASSERT_TRUE(rig.gate.SessionFor(E1).has_value());
    EXPECT_EQ(*rig.gate.SessionFor(E1), "s1");
    EXPECT_FALSE(rig.gate.SessionFor(E2).has_value());
    rig.gate.StopForDeath(E1, l::EndReason::kF2);
    EXPECT_FALSE(rig.gate.SessionFor(E1).has_value());  // dead
}
