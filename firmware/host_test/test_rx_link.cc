// StackChan FW-A2 plan 2B-1: a link's receive task (design §3.2, §3.3, §3.8, contract S7) fed
// with server frames, the ports recorded.
#include <gtest/gtest.h>

#include <cJSON.h>

#include <string>
#include <vector>

#include "rx_link.h"
#include "ws_frame.h"

using namespace stackchan;
using namespace stackchan::link;

namespace {

constexpr uint64_t E = 65536ull * 65536 + 5;  // above 2^32 (design §3.1)

// A server frame (unmasked)
std::string Frame(uint8_t first, const std::string& payload) {
    std::string f(1, static_cast<char>(first));
    f.push_back(static_cast<char>(payload.size()));  // the tests keep payloads under 126
    return f + payload;
}
std::string Text(const std::string& json) { return Frame(0x81, json); }

struct Rig {
    int64_t now = 7'000'000;
    std::vector<Input> posted;
    std::vector<int64_t> received;
    std::vector<std::string> pongs, calls;
    net::PushResult pong_result = net::PushResult::kQueued;
    std::string hello_session;
    int64_t hello_delay = 0;  // the app's hello callback takes this long
    std::vector<std::pair<uint64_t, EndReason>> deaths;
    LinkPhase phase;
    RxPorts Ports() {
        RxPorts p;
        p.now_us = [this] { return now; };
        p.post = [this](const Input& in) {
            posted.push_back(in);
            return true;
        };
        p.received = [this](int64_t t) { received.push_back(t); };
        p.push_pong = [this](uint64_t e, std::string payload, int64_t) {
            pongs.push_back(std::to_string(e) + ":" + payload);
            return pong_result;
        };
        p.hello_reply = [this](const AudioHelloReply& r, const cJSON*) {
            hello_session = r.session_id;
            now += hello_delay;
        };
        p.stop_for_death = [this](uint64_t e, EndReason r) { deaths.push_back({e, r}); };
        p.tts_start = [this](uint64_t e, const wire::TtsStart& t) {
            calls.push_back("tts_start " + std::to_string(e) + " " + std::to_string(t.gen));
        };
        p.tts_stop = [this](uint64_t e, uint32_t gen) {
            calls.push_back("tts_stop " + std::to_string(e) + " " + std::to_string(gen));
        };
        p.server_audio = [this](uint64_t e, AudioIn a) { calls.push_back("audio " + std::to_string(e) + " " + a.payload); };
        p.gw_listen = [this](uint64_t e, const wire::GwListen& l) {
            calls.push_back(std::string("listen ") + std::to_string(e) + (l.start ? " start" : " stop"));
        };
        p.app_json = [this](uint64_t e, const cJSON* root) {
            calls.push_back("app " + std::to_string(e) + " " + wire::TypeOf(root));
        };
        p.abort = [this](uint64_t e, const wire::AbortRequest& r) { calls.push_back("abort " + std::to_string(e) + " " + r.req_id); };
        p.stat = [this](uint64_t e, const std::string& id) { calls.push_back("stat " + std::to_string(e) + " " + id); };
        return p;
    }
};

const char* kHello = R"({"type":"hello","transport":"websocket","session_id":"s1","stackchan_ctrl":1})";

void Feed(RxLink& rx, const std::string& bytes) { rx.OnBytes(bytes.data(), bytes.size()); }

}  // namespace

// The audio hello reply is reported once, with its receive time (S6 counts from it) and the
// link's E; the session is known before the notice. A second hello is dropped (design §3.8).
TEST(RxLink, TheAudioHelloReplyIsReportedOnce) {
    Rig rig;
    RxLink rx(LinkSide::kAudio, 4, E, "", 1, &rig.phase, rig.Ports());
    const int64_t received_at = rig.now;
    rig.hello_delay = 300'000;  // S6 counts from the receive, not from after the app's callback
    Feed(rx, Text(kHello) + Text(kHello));
    ASSERT_EQ(rig.posted.size(), 1u);
    EXPECT_EQ(rig.posted[0].kind, InKind::kAudioHelloReply);
    EXPECT_EQ(rig.posted[0].attempt, 4u);
    EXPECT_EQ(rig.posted[0].e, E);
    EXPECT_TRUE(rig.posted[0].ctrl_offered);
    EXPECT_EQ(rig.posted[0].at_us, received_at);
    EXPECT_EQ(rx.session_id(), "s1");
    EXPECT_EQ(rig.hello_session, "s1");
    EXPECT_EQ(rx.stats().extra_hellos, 1u);
}

// A reply without a session is not reported (the 5 s hello deadline ends the pair) and does not
// use up the one report.
TEST(RxLink, AHelloReplyWithoutASessionIsNotReported) {
    Rig rig;
    RxLink rx(LinkSide::kAudio, 4, E, "", 1, &rig.phase, rig.Ports());
    Feed(rx, Text(R"({"type":"hello","transport":"websocket"})"));
    EXPECT_TRUE(rig.posted.empty());
    EXPECT_EQ(rx.stats().dropped, 1u);
    Feed(rx, Text(kHello));
    ASSERT_EQ(rig.posted.size(), 1u);
}

// The session gates tts / listen; audio frames go to the gate with the link's E; the rest goes
// to the app with E.
TEST(RxLink, AudioLinkMessagesGoToTheirOwners) {
    Rig rig;
    RxLink rx(LinkSide::kAudio, 4, E, "", 3, &rig.phase, rig.Ports());
    const std::string before = Text(R"({"type":"tts","state":"stop","session_id":"s1","gen":1})");
    Feed(rx, before);  // before the hello reply: no session yet, dropped
    Feed(rx, Text(kHello));
    Feed(rx, Text(R"({"type":"tts","state":"start","session_id":"s1","gen":2,"aborted_gen":1,"dev_abort_seen":0})"));
    Feed(rx, Frame(0x82, std::string("\x00\x00\x00\x02op", 6)));  // version 3
    Feed(rx, Text(R"({"type":"tts","state":"stop","session_id":"s1","gen":2})"));
    Feed(rx, Text(R"({"type":"listen","state":"start","session_id":"s1"})"));
    Feed(rx, Text(R"({"type":"mcp","payload":{}})"));
    Feed(rx, Text(R"({"type":"tts","state":"sentence_start","session_id":"s1","text":"x"})"));
    Feed(rx, Text(R"({"type":"abort","session_id":"s1","gen":2,"req_id":"r"})"));  // control link only
    const std::string e = std::to_string(E);
    EXPECT_EQ(rig.calls, (std::vector<std::string>{"tts_start " + e + " 2", "audio " + e + " op",
                                                   "tts_stop " + e + " 2", "listen " + e + " start", "app " + e + " mcp",
                                                   "app " + e + " tts"}));
    EXPECT_EQ(rx.stats().dropped, 2u);
}

TEST(RxLink, ABrokenAudioFrameIsCountedAndDropped) {
    Rig rig;
    RxLink rx(LinkSide::kAudio, 4, E, "", 3, &rig.phase, rig.Ports());
    Feed(rx, Frame(0x82, std::string("\x00\x00\x00\x09op", 6)));  // says 9 bytes, has 2
    EXPECT_TRUE(rig.calls.empty());
    EXPECT_EQ(rx.stats().bad_audio, 1u);
}

// Every frame is a receive (F1 watches the control link's receives, pong included); a ping is
// answered with its payload through the link's own send queue.
TEST(RxLink, EveryFrameIsAReceiveAndAPingIsAnswered) {
    Rig rig;
    RxLink rx(LinkSide::kCtrl, 4, E, "s1", 1, &rig.phase, rig.Ports());
    Feed(rx, Frame(0x89, "p1") + Frame(0x8A, "x") + Text(R"({"type":"nothing"})"));
    EXPECT_EQ(rig.received, (std::vector<int64_t>{rig.now}));
    EXPECT_EQ(rig.pongs, (std::vector<std::string>{std::to_string(E) + ":p1"}));
    rig.pong_result = net::PushResult::kClosed;  // the pair ended meanwhile: nothing to end
    Feed(rx, Frame(0x89, "p2"));
    EXPECT_EQ(rx.stats().pongs_lost, 1u);
    EXPECT_TRUE(rig.posted.empty());
    EXPECT_TRUE(rig.deaths.empty());
    rig.now += 1000;
    Feed(rx, Frame(0x01, "frag"));  // a fragment alone: no message yet, but a receive (F1)
    ASSERT_FALSE(rig.received.empty());  // fail, not crash, when no receive was marked
    EXPECT_EQ(rig.received.back(), rig.now);
    const size_t marks = rig.received.size();
    rig.now += 1000;
    Feed(rx, std::string(1, static_cast<char>(0x80)));  // half a frame: not a receive yet
    EXPECT_EQ(rig.received.size(), marks);
}

// A pong that does not fit the open queue ends the pair (design §4.1: a pong is never dropped;
// Codex review 148 Important 3).
TEST(RxLink, APongThatDoesNotFitEndsThePair) {
    Rig rig;
    rig.phase.OnResultPosted();
    rig.pong_result = net::PushResult::kFull;
    RxLink rx(LinkSide::kCtrl, 4, E, "s1", 1, &rig.phase, rig.Ports());
    Feed(rx, Frame(0x89, "p"));
    ASSERT_EQ(rig.deaths.size(), 1u);
    EXPECT_EQ(rig.deaths[0], std::make_pair(E, EndReason::kQueueFull));
    ASSERT_EQ(rig.posted.size(), 1u);
    EXPECT_EQ(rig.posted[0].kind, InKind::kEndRequest);
    EXPECT_EQ(rig.posted[0].reason, EndReason::kQueueFull);
    EXPECT_TRUE(rx.ended());
}

// The control link: the hello reply (once, with the epoch it carries), abort requests (the gate
// compares the session), stat requests (with the pair's session), nothing else.
TEST(RxLink, ControlLinkMessages) {
    Rig rig;
    RxLink rx(LinkSide::kCtrl, 4, E, "s1", 1, &rig.phase, rig.Ports());
    const std::string hello = Text(R"({"type":"hello","role":"control","audio_epoch":12})");
    Feed(rx, hello + hello);
    ASSERT_EQ(rig.posted.size(), 1u);
    EXPECT_EQ(rig.posted[0].kind, InKind::kCtrlHelloReply);
    EXPECT_EQ(rig.posted[0].e, 12u);
    Feed(rx, Text(R"({"type":"abort","session_id":"s1","gen":2,"req_id":"r1","reason":"hush"})"));
    Feed(rx, Text(R"({"type":"stat","session_id":"s1","req_id":"q"})"));
    Feed(rx, Text(R"({"type":"stat","session_id":"zz","req_id":"q"})"));
    Feed(rx, Text(R"({"type":"tts","state":"stop","session_id":"s1","gen":2})"));
    Feed(rx, Frame(0x82, "bin"));
    const std::string e = std::to_string(E);
    EXPECT_EQ(rig.calls, (std::vector<std::string>{"abort " + e + " r1", "stat " + e + " q"}));
    EXPECT_EQ(rx.stats().dropped, 3u);
    EXPECT_EQ(rx.stats().extra_hellos, 1u);
}

// A close (K6), a broken frame or a passive disconnect ends the pair once: after the connect
// result the receive task posts the end request with its reason; later frames are ignored.
TEST(RxLink, TheLinkEndsOnceAfterTheResult) {
    struct Case {
        const char* name;
        std::string bytes;  // empty: a passive disconnect
        EndReason reason;
    };
    for (const Case& c : {Case{"close", Frame(0x88, ""), EndReason::kServerClose},
                          Case{"broken", Frame(0xC1, "x"), EndReason::kCtrlClosed},
                          Case{"disconnect", "", EndReason::kCtrlClosed}}) {
        Rig rig;
        rig.phase.OnResultPosted();
        RxLink rx(LinkSide::kCtrl, 4, E, "s1", 1, &rig.phase, rig.Ports());
        if (c.bytes.empty()) {
            rx.OnDisconnected();
        } else {
            Feed(rx, c.bytes);
        }
        ASSERT_EQ(rig.posted.size(), 1u) << c.name;  // at once, not when the socket ends later
        rx.OnDisconnected();  // the socket's own end comes after a close too
        Feed(rx, Text(R"({"type":"stat","session_id":"s1","req_id":"q"})"));
        ASSERT_EQ(rig.posted.size(), 1u) << c.name;
        EXPECT_EQ(rig.posted[0].kind, InKind::kEndRequest) << c.name;
        EXPECT_EQ(rig.posted[0].e, E) << c.name;
        EXPECT_EQ(rig.posted[0].reason, c.reason) << c.name;
        EXPECT_TRUE(rig.calls.empty()) << c.name;
        EXPECT_TRUE(rx.ended()) << c.name;
    }
}

// Before the connect result the end is only marked: the worker posts it after the result.
TEST(RxLink, BeforeTheResultTheWorkerPostsTheEnd) {
    Rig rig;
    RxLink rx(LinkSide::kAudio, 4, E, "", 1, &rig.phase, rig.Ports());
    rx.OnDisconnected();
    EXPECT_TRUE(rig.posted.empty());
    EXPECT_TRUE(rig.phase.OnResultPosted());
    EXPECT_EQ(rig.phase.EndReasonOr(EndReason::kNone), EndReason::kAudioClosed);
}

// The gateway's close before the result: the worker's end request says so (Claude review 152 Minor 1)
TEST(RxLink, AServerCloseBeforeTheResultKeepsItsReason) {
    Rig rig;
    RxLink rx(LinkSide::kCtrl, 4, E, "s1", 1, &rig.phase, rig.Ports());
    Feed(rx, Frame(0x88, ""));
    EXPECT_TRUE(rig.posted.empty());
    EXPECT_TRUE(rig.phase.OnResultPosted());
    EXPECT_EQ(rig.phase.EndReasonOr(EndReason::kCtrlClosed), EndReason::kServerClose);
}

// Claude review 152 Minor 3: LinkHub refuses an app port left empty
TEST(RxLink, MissingAppPortsAreNamed) {
    Rig rig;
    RxPorts p = rig.Ports();
    EXPECT_EQ(MissingAppPort(p), nullptr);
    p.received = nullptr;  // the link fills it, not the app
    EXPECT_EQ(MissingAppPort(p), nullptr);
    const std::vector<std::pair<std::string, void (*)(RxPorts&)>> cases = {
        {"hello_reply", [](RxPorts& x) { x.hello_reply = nullptr; }},
        {"server_audio", [](RxPorts& x) { x.server_audio = nullptr; }},
        {"app_json", [](RxPorts& x) { x.app_json = nullptr; }},
        {"stat", [](RxPorts& x) { x.stat = nullptr; }},
    };
    for (const auto& [name, clear] : cases) {
        RxPorts q = rig.Ports();
        clear(q);
        ASSERT_NE(MissingAppPort(q), nullptr) << name;
        EXPECT_EQ(std::string(MissingAppPort(q)), name);
    }
}
