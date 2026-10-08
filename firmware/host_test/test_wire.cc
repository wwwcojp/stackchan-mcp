// StackChan FW-A2: the contract's JSON messages (contract §1 S1-S8, §2.1, §5.1).
#include <gtest/gtest.h>

#include <cJSON.h>

#include <memory>
#include <string>

#include "wire.h"

using namespace stackchan::wire;

namespace {

struct JsonDeleter {
    void operator()(cJSON* p) const { cJSON_Delete(p); }
};
using Json = std::unique_ptr<cJSON, JsonDeleter>;

Json Parse(const char* text) { return Json(cJSON_Parse(text)); }

std::string Print(cJSON* root) {
    char* s = cJSON_PrintUnformatted(root);
    std::string out = s != nullptr ? s : "";
    cJSON_free(s);
    return out;
}

std::string PrintOwned(cJSON* root) {
    Json j(root);
    return Print(j.get());
}

}  // namespace

TEST(Wire, TypeAndSession) {
    Json j = Parse(R"({"type":"tts","session_id":"s1"})");
    EXPECT_EQ(TypeOf(j.get()), "tts");
    EXPECT_EQ(TypeOf(Parse(R"({"type":1})").get()), "");
    EXPECT_EQ(TypeOf(Parse("[1]").get()), "");
    EXPECT_TRUE(SessionMatches(j.get(), "s1"));
    EXPECT_FALSE(SessionMatches(j.get(), "s2"));
    EXPECT_FALSE(SessionMatches(j.get(), ""));  // no session yet
    EXPECT_FALSE(SessionMatches(Parse(R"({"session_id":""})").get(), ""));
    EXPECT_FALSE(SessionMatches(Parse(R"({"type":"tts"})").get(), "s1"));
    EXPECT_FALSE(SessionMatches(Parse(R"({"session_id":1})").get(), "1"));
}

TEST(Wire, CtrlOfferedOnlyForTheNumberOne) {
    EXPECT_TRUE(CtrlOffered(Parse(R"({"type":"hello","stackchan_ctrl":1})").get()));
    EXPECT_FALSE(CtrlOffered(Parse(R"({"type":"hello"})").get()));
    EXPECT_FALSE(CtrlOffered(Parse(R"({"type":"hello","stackchan_ctrl":2})").get()));
    EXPECT_FALSE(CtrlOffered(Parse(R"({"type":"hello","stackchan_ctrl":"1"})").get()));
    EXPECT_FALSE(CtrlOffered(Parse(R"({"type":"hello","stackchan_ctrl":true})").get()));
}

TEST(Wire, CtrlHelloReplyCarriesAnExactEpoch) {
    uint64_t e = 0;
    // boot_count 65536, conn_index 3: above 2^32 (design §3.1)
    EXPECT_TRUE(ParseCtrlHelloReply(
        Parse(R"({"type":"hello","role":"control","audio_epoch":4294967299})").get(), &e));
    EXPECT_EQ(e, 4294967299ull);
    EXPECT_FALSE(ParseCtrlHelloReply(Parse(R"({"type":"hello","audio_epoch":5})").get(), &e));
    EXPECT_FALSE(ParseCtrlHelloReply(Parse(R"({"type":"hello","role":"control"})").get(), &e));
    EXPECT_FALSE(
        ParseCtrlHelloReply(Parse(R"({"type":"hello","role":"control","audio_epoch":-1})").get(), &e));
    EXPECT_FALSE(
        ParseCtrlHelloReply(Parse(R"({"type":"hello","role":"control","audio_epoch":1.5})").get(), &e));
    EXPECT_FALSE(
        ParseCtrlHelloReply(Parse(R"({"type":"ready","role":"control","audio_epoch":5})").get(), &e));
}

TEST(Wire, TtsStartNeedsEveryField) {
    TtsStart t;
    ASSERT_TRUE(ParseTtsStart(
        Parse(R"({"type":"tts","state":"start","gen":3,"aborted_gen":2,"dev_abort_seen":1})").get(), &t));
    EXPECT_EQ(t.gen, 3u);
    EXPECT_EQ(t.aborted_gen, 2u);
    EXPECT_EQ(t.dev_abort_seen, 1u);
    for (const char* bad : {
             R"({"type":"tts","state":"start","aborted_gen":0,"dev_abort_seen":0})",
             R"({"type":"tts","state":"start","gen":0,"aborted_gen":0,"dev_abort_seen":0})",
             R"({"type":"tts","state":"start","gen":1,"dev_abort_seen":0})",
             R"({"type":"tts","state":"start","gen":1,"aborted_gen":0})",
             R"({"type":"tts","state":"start","gen":"1","aborted_gen":0,"dev_abort_seen":0})",
             R"({"type":"tts","state":"start","gen":1.5,"aborted_gen":0,"dev_abort_seen":0})",
             R"({"type":"tts","state":"start","gen":4294967296,"aborted_gen":0,"dev_abort_seen":0})",
             R"({"type":"tts","state":"start","gen":1,"aborted_gen":4294967296,"dev_abort_seen":0})",
             R"({"type":"tts","state":"start","gen":1,"aborted_gen":-1,"dev_abort_seen":0})",
             R"({"type":"tts","state":"stop","gen":1,"aborted_gen":0,"dev_abort_seen":0})",
         }) {
        EXPECT_FALSE(ParseTtsStart(Parse(bad).get(), &t)) << bad;
    }
}

TEST(Wire, TtsStop) {
    uint32_t g = 0;
    ASSERT_TRUE(ParseTtsStop(Parse(R"({"type":"tts","state":"stop","gen":7})").get(), &g));
    EXPECT_EQ(g, 7u);
    EXPECT_FALSE(ParseTtsStop(Parse(R"({"type":"tts","state":"stop"})").get(), &g));
    EXPECT_FALSE(ParseTtsStop(Parse(R"({"type":"tts","state":"stop","gen":0})").get(), &g));
    EXPECT_FALSE(ParseTtsStop(Parse(R"({"type":"tts","state":"start","gen":7})").get(), &g));
}

TEST(Wire, AbortRequestIsNeverAReply) {
    AbortRequest a;
    ASSERT_TRUE(ParseAbortRequest(
        Parse(R"({"type":"abort","session_id":"s","gen":4,"req_id":"r-1","reason":"hush"})").get(), &a));
    EXPECT_EQ(a.gen, 4u);
    EXPECT_EQ(a.req_id, "r-1");
    EXPECT_EQ(a.reason, "hush");
    EXPECT_EQ(a.session_id, "s");
    ASSERT_TRUE(ParseAbortRequest(Parse(R"({"type":"abort","gen":4,"req_id":"r"})").get(), &a));
    EXPECT_EQ(a.reason, "");
    EXPECT_EQ(a.session_id, "");  // missing: kept empty for the gate to refuse
    ASSERT_TRUE(ParseAbortRequest(Parse(R"({"type":"abort","session_id":7,"gen":4,"req_id":"r"})").get(), &a));
    EXPECT_EQ(a.session_id, "");
    const std::string id32(kMaxReqIdLen, 'x');
    const std::string ok = R"({"type":"abort","gen":1,"req_id":")" + id32 + R"("})";
    EXPECT_TRUE(ParseAbortRequest(Parse(ok.c_str()).get(), &a));
    const std::string too_long = R"({"type":"abort","gen":1,"req_id":")" + id32 + R"(x"})";
    for (const std::string bad : {
             std::string(R"({"type":"abort","state":"done","gen":4,"req_id":"r"})"),
             std::string(R"({"type":"abort","gen":4})"),
             std::string(R"({"type":"abort","gen":4,"req_id":""})"),
             std::string(R"({"type":"abort","gen":0,"req_id":"r"})"),
             std::string(R"({"type":"abort","req_id":"r"})"),
             too_long,
         }) {
        EXPECT_FALSE(ParseAbortRequest(Parse(bad.c_str()).get(), &a)) << bad;
    }
}

TEST(Wire, StatRequest) {
    std::string id;
    ASSERT_TRUE(ParseStatRequest(Parse(R"({"type":"stat","session_id":"s","req_id":"q"})").get(), &id));
    EXPECT_EQ(id, "q");
    EXPECT_FALSE(ParseStatRequest(Parse(R"({"type":"stat","state":"done","req_id":"q"})").get(), &id));
    EXPECT_FALSE(ParseStatRequest(Parse(R"({"type":"stat"})").get(), &id));
}

TEST(Wire, GatewayListenModeAndProfile) {
    GwListen g;
    ASSERT_TRUE(ParseGwListen(Parse(R"({"type":"listen","state":"start"})").get(), &g));
    EXPECT_TRUE(g.start);
    EXPECT_EQ(g.mode, ListenMode::kManualStop);  // the gateway controls the stop boundary
    EXPECT_EQ(g.profile, ListenProfile::kVoice);
    ASSERT_TRUE(ParseGwListen(
        Parse(R"({"type":"listen","state":"start","mode":"auto","profile":"raw"})").get(), &g));
    EXPECT_EQ(g.mode, ListenMode::kAutoStop);
    EXPECT_EQ(g.profile, ListenProfile::kRaw);
    ASSERT_TRUE(ParseGwListen(Parse(R"({"type":"listen","state":"start","mode":"realtime"})").get(), &g));
    EXPECT_EQ(g.mode, ListenMode::kRealtime);
    ASSERT_TRUE(ParseGwListen(
        Parse(R"({"type":"listen","state":"start","mode":"loud","profile":7})").get(), &g));
    EXPECT_EQ(g.mode, ListenMode::kManualStop);
    EXPECT_TRUE(g.mode_unknown);
    EXPECT_EQ(g.profile, ListenProfile::kVoice);
    EXPECT_TRUE(g.profile_unknown);
    ASSERT_TRUE(ParseGwListen(Parse(R"({"type":"listen","state":"stop"})").get(), &g));
    EXPECT_FALSE(g.start);
    EXPECT_FALSE(ParseGwListen(Parse(R"({"type":"listen","state":"detect"})").get(), &g));
    EXPECT_FALSE(ParseGwListen(Parse(R"({"type":"listen"})").get(), &g));
}

TEST(Wire, AudioHelloGetsTheCtrlFeature) {
    Json h = Parse(R"({"type":"hello","features":{"mcp":true,"stackchan_ext":1}})");
    AddCtrlFeature(h.get());
    EXPECT_EQ(Print(h.get()),
              R"({"type":"hello","features":{"mcp":true,"stackchan_ext":1,"stackchan_ctrl":1}})");
    Json bare = Parse(R"({"type":"hello"})");
    AddCtrlFeature(bare.get());
    EXPECT_EQ(Print(bare.get()), R"({"type":"hello","features":{"stackchan_ctrl":1}})");
    Json twice = Parse(R"({"type":"hello","features":{"stackchan_ctrl":0}})");
    AddCtrlFeature(twice.get());
    EXPECT_EQ(Print(twice.get()), R"({"type":"hello","features":{"stackchan_ctrl":1}})");
}

TEST(Wire, ControlHelloReadyAndStamps) {
    const uint64_t e = 4294967299ull;
    CtrlStamp stamp(e);
    Json hello(BuildCtrlHello(e));
    stamp.Stamp(hello.get());
    EXPECT_EQ(Print(hello.get()),
              R"({"type":"hello","role":"control","audio_epoch":4294967299,"fw_epoch":4294967299,"seq":1})");
    Json ready(BuildReady(e));
    stamp.Stamp(ready.get());
    EXPECT_EQ(Print(ready.get()), R"({"type":"ready","audio_epoch":4294967299,"fw_epoch":4294967299,"seq":2})");
    EXPECT_EQ(stamp.seq(), 2u);
}

TEST(Wire, AbortDone) {
    AbortRequest req;
    req.gen = 5;
    req.req_id = "r9";
    req.reason = "user";
    EXPECT_EQ(PrintOwned(BuildAbortDone(req, false, 180)),
              R"({"type":"abort","state":"done","req_id":"r9","reason":"user","gen":5,"result":"stopped","dropped_ms":180})");
    EXPECT_EQ(PrintOwned(BuildAbortDone(req, true, 180)),
              R"({"type":"abort","state":"done","req_id":"r9","reason":"user","gen":5,"result":"already","dropped_ms":0})");
}

TEST(Wire, DeviceAbortAndListen) {
    EXPECT_EQ(PrintOwned(BuildDeviceAbort("s", 3, 1, DeviceAbortReason::kTouch)),
              R"({"session_id":"s","type":"abort","gen":3,"dev_abort_seq":1})");
    EXPECT_EQ(PrintOwned(BuildDeviceAbort("s", 3, 2, DeviceAbortReason::kWakeWord)),
              R"({"session_id":"s","type":"abort","gen":3,"dev_abort_seq":2,"reason":"wake_word_detected"})");
    EXPECT_EQ(PrintOwned(BuildListenStart("s", ListenMode::kManualStop)),
              R"({"session_id":"s","type":"listen","state":"start","mode":"manual"})");
    EXPECT_EQ(PrintOwned(BuildListenStart("s", ListenMode::kAutoStop)),
              R"({"session_id":"s","type":"listen","state":"start","mode":"auto"})");
    EXPECT_EQ(PrintOwned(BuildListenStart("s", ListenMode::kRealtime)),
              R"({"session_id":"s","type":"listen","state":"start","mode":"realtime"})");
    EXPECT_EQ(PrintOwned(BuildListenStop("s")), R"({"session_id":"s","type":"listen","state":"stop"})");
}

TEST(Wire, StatReplyShape) {
    EXPECT_EQ(PrintOwned(BuildStatReply("q1", {{"gen", {{"current", 3}, {"aborted", 2}}}, {"heap", {}}})),
              R"({"type":"stat","state":"done","req_id":"q1","gen":{"current":3,"aborted":2},"heap":{}})");
}
