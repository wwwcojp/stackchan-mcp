// StackChan FW-A2 plan 2B-1: what the receive tasks decide, pure: the audio link's binary frames
// (Protocol-Version 1-3, today's websocket_protocol.cc), the audio hello reply, and where each
// text message of the audio and control links goes (contract S7, design §2.2 and §4.1).
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <cJSON.h>

#include <cstring>
#include <string>

#include "rx_route.h"

using namespace stackchan::link;

namespace {

struct Json {
    cJSON* root;
    explicit Json(const char* text) : root(cJSON_Parse(text)) {}
    ~Json() { cJSON_Delete(root); }
};

std::string Bytes(std::initializer_list<int> b) {
    std::string s;
    for (int x : b) s.push_back(static_cast<char>(x));
    return s;
}

}  // namespace

TEST(AudioFrame, Version1IsTheOpusPayloadAsItIs) {
    const std::string f = EncodeAudioFrame(1, 1234, reinterpret_cast<const uint8_t*>("op"), 2);
    EXPECT_EQ(f, "op");
    AudioIn in;
    ASSERT_TRUE(DecodeAudioFrame(1, reinterpret_cast<const uint8_t*>("xyz"), 3, &in));
    EXPECT_EQ(in.payload, "xyz");
    EXPECT_EQ(in.timestamp, 0u);
}

TEST(AudioFrame, Version2HasABigEndianHeader) {
    const std::string f = EncodeAudioFrame(2, 0x01020304, reinterpret_cast<const uint8_t*>("op"), 2);
    // version 2, type 0, reserved 0, timestamp, payload size, payload
    EXPECT_EQ(f, Bytes({0, 2, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 0, 0, 0, 2}) + "op");
    AudioIn in;
    ASSERT_TRUE(DecodeAudioFrame(2, reinterpret_cast<const uint8_t*>(f.data()), f.size(), &in));
    EXPECT_EQ(in.payload, "op");
    EXPECT_EQ(in.timestamp, 0x01020304u);
}

TEST(AudioFrame, Version3HasAShortHeader) {
    const std::string f = EncodeAudioFrame(3, 99, reinterpret_cast<const uint8_t*>("abc"), 3);
    EXPECT_EQ(f, Bytes({0, 0, 0, 3}) + "abc");
    AudioIn in;
    ASSERT_TRUE(DecodeAudioFrame(3, reinterpret_cast<const uint8_t*>(f.data()), f.size(), &in));
    EXPECT_EQ(in.payload, "abc");
    EXPECT_EQ(in.timestamp, 0u);
}

// Today's code read the payload size from the header and copied that many bytes past the frame.
TEST(AudioFrame, AShortOrLyingHeaderIsRefused) {
    AudioIn in;
    const std::string v2 = Bytes({0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9}) + "op";  // says 9, has 2
    EXPECT_FALSE(DecodeAudioFrame(2, reinterpret_cast<const uint8_t*>(v2.data()), v2.size(), &in));
    EXPECT_FALSE(DecodeAudioFrame(2, reinterpret_cast<const uint8_t*>(v2.data()), 15, &in));  // header cut
    const std::string v3 = Bytes({0, 0, 0, 4}) + "abc";
    EXPECT_FALSE(DecodeAudioFrame(3, reinterpret_cast<const uint8_t*>(v3.data()), v3.size(), &in));
    EXPECT_FALSE(DecodeAudioFrame(3, reinterpret_cast<const uint8_t*>(v3.data()), 3, &in));
    const std::string v2_long = Bytes({0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}) + "op";  // says 1
    ASSERT_TRUE(DecodeAudioFrame(2, reinterpret_cast<const uint8_t*>(v2_long.data()), v2_long.size(), &in));
    EXPECT_EQ(in.payload, "o");  // the size in the header wins, trailing bytes are ignored
}

TEST(AudioHelloReply, NeedsTheTransportAndASession) {
    AudioHelloReply r;
    Json ok(R"({"type":"hello","transport":"websocket","session_id":"s1","stackchan_ctrl":1,)"
            R"("audio_params":{"sample_rate":24000,"frame_duration":60}})");
    ASSERT_TRUE(ParseAudioHelloReply(ok.root, &r));
    EXPECT_EQ(r.session_id, "s1");
    EXPECT_TRUE(r.ctrl_offered);
    EXPECT_EQ(r.sample_rate, 24000);
    EXPECT_EQ(r.frame_duration, 60);

    AudioHelloReply plain;
    Json no_ctrl(R"({"type":"hello","transport":"websocket","session_id":"s2"})");
    ASSERT_TRUE(ParseAudioHelloReply(no_ctrl.root, &plain));
    EXPECT_FALSE(plain.ctrl_offered);
    EXPECT_EQ(plain.sample_rate, 0);  // missing: the caller keeps its value

    for (const char* bad : {R"({"type":"hello","session_id":"s"})",
                            R"({"type":"hello","transport":"udp","session_id":"s"})",
                            R"({"type":"hello","transport":"websocket"})",
                            R"({"type":"hello","transport":"websocket","session_id":""})",
                            R"({"type":"hello","transport":"websocket","session_id":3})"}) {
        Json j(bad);
        AudioHelloReply x;
        EXPECT_FALSE(ParseAudioHelloReply(j.root, &x)) << bad;
    }
}

TEST(RouteAudio, TtsAndListenNeedTheSession) {
    const std::string sid = "s1";
    {
        Json j(R"({"type":"tts","state":"start","session_id":"s1","gen":3,"aborted_gen":2,"dev_abort_seen":1})");
        const AudioRouted r = RouteAudioText(j.root, sid);
        EXPECT_EQ(r.route, AudioRoute::kTtsStart);
        EXPECT_EQ(r.tts_start.gen, 3u);
        EXPECT_EQ(r.tts_start.aborted_gen, 2u);
        EXPECT_EQ(r.tts_start.dev_abort_seen, 1u);
    }
    {
        Json j(R"({"type":"tts","state":"stop","session_id":"s1","gen":3})");
        const AudioRouted r = RouteAudioText(j.root, sid);
        EXPECT_EQ(r.route, AudioRoute::kTtsStop);
        EXPECT_EQ(r.tts_stop_gen, 3u);
    }
    {
        Json j(R"({"type":"tts","state":"sentence_start","session_id":"s1","text":"hi"})");
        EXPECT_EQ(RouteAudioText(j.root, sid).route, AudioRoute::kTtsOther);
    }
    {
        Json j(R"({"type":"listen","state":"start","session_id":"s1","mode":"auto"})");
        const AudioRouted r = RouteAudioText(j.root, sid);
        EXPECT_EQ(r.route, AudioRoute::kListen);
        EXPECT_TRUE(r.listen.start);
        EXPECT_EQ(r.listen.mode, stackchan::wire::ListenMode::kAutoStop);
    }
    for (const char* other_session : {R"({"type":"tts","state":"start","session_id":"x","gen":3,"aborted_gen":0,"dev_abort_seen":0})",
                                      R"({"type":"tts","state":"stop","gen":3})",
                                      R"({"type":"listen","state":"stop","session_id":"x"})"}) {
        Json j(other_session);
        EXPECT_EQ(RouteAudioText(j.root, sid).route, AudioRoute::kDrop) << other_session;
    }
    // before the hello reply the session is empty: nothing gets through
    Json j(R"({"type":"tts","state":"stop","session_id":"","gen":3})");
    EXPECT_EQ(RouteAudioText(j.root, "").route, AudioRoute::kDrop);
}

TEST(RouteAudio, BrokenTtsAndListenAreDropped) {
    for (const char* bad : {R"({"type":"tts","state":"start","session_id":"s1","gen":3})",
                            R"({"type":"tts","state":"stop","session_id":"s1"})",
                            R"({"type":"tts","session_id":"s1"})",
                            R"({"type":"listen","state":"detect","session_id":"s1"})"}) {
        Json j(bad);
        const AudioRouted r = RouteAudioText(j.root, "s1");
        EXPECT_EQ(r.route, AudioRoute::kDrop) << bad;
        EXPECT_STRNE(r.why, "") << bad;
    }
}

// Contract S7: an abort request comes on the control link only; the audio link's hello is the
// hello reply; everything else (mcp, llm, stt, system, alert, custom, unknown) goes to the app.
TEST(RouteAudio, AbortHelloAndTheRest) {
    {
        Json j(R"({"type":"abort","session_id":"s1","gen":2,"req_id":"r","reason":"hush"})");
        EXPECT_EQ(RouteAudioText(j.root, "s1").route, AudioRoute::kDrop);
    }
    {
        Json j(R"({"type":"hello","transport":"websocket","session_id":"s9"})");
        EXPECT_EQ(RouteAudioText(j.root, "").route, AudioRoute::kHelloReply);
    }
    for (const char* app : {R"({"type":"mcp","payload":{}})", R"({"type":"llm","emotion":"happy"})",
                            R"({"type":"stt","text":"x"})", R"({"type":"system","command":"reboot"})",
                            R"({"type":"alert","status":"s","message":"m"})", R"({"type":"custom"})",
                            R"({"type":"something_new"})"}) {
        Json j(app);
        EXPECT_EQ(RouteAudioText(j.root, "s1").route, AudioRoute::kApp) << app;
    }
    Json no_type(R"({"state":"start"})");
    EXPECT_EQ(RouteAudioText(no_type.root, "s1").route, AudioRoute::kDrop);
    EXPECT_EQ(RouteAudioText(nullptr, "s1").route, AudioRoute::kDrop);  // not JSON at all
}

TEST(RouteCtrl, HelloAbortAndStat) {
    {
        Json j(R"({"type":"hello","role":"control","audio_epoch":4294967299})");
        const CtrlRouted r = RouteCtrlText(j.root, "s1");
        EXPECT_EQ(r.route, CtrlRoute::kHelloReply);
        EXPECT_EQ(r.audio_epoch, 4294967299ull);
    }
    {
        Json j(R"({"type":"abort","session_id":"other","gen":2,"req_id":"r1","reason":"hush"})");
        const CtrlRouted r = RouteCtrlText(j.root, "s1");
        EXPECT_EQ(r.route, CtrlRoute::kAbort);  // the gate compares the session (contract R1, S8)
        EXPECT_EQ(r.abort.req_id, "r1");
        EXPECT_EQ(r.abort.session_id, "other");
    }
    {
        Json j(R"({"type":"stat","session_id":"s1","req_id":"q"})");
        const CtrlRouted r = RouteCtrlText(j.root, "s1");
        EXPECT_EQ(r.route, CtrlRoute::kStat);
        EXPECT_EQ(r.req_id, "q");
    }
    for (const char* drop : {R"({"type":"stat","session_id":"x","req_id":"q"})",
                             R"({"type":"abort","state":"done","session_id":"s1","gen":2,"req_id":"r"})",
                             R"({"type":"abort","session_id":"s1","gen":2})",
                             R"({"type":"hello","role":"audio","audio_epoch":1})",
                             R"({"type":"tts","state":"stop","session_id":"s1","gen":1})",
                             R"({"type":"mcp"})", R"({"no":"type"})"}) {
        Json j(drop);
        const CtrlRouted r = RouteCtrlText(j.root, "s1");
        EXPECT_EQ(r.route, CtrlRoute::kDrop) << drop;
        EXPECT_STRNE(r.why, "") << drop;
    }
}
