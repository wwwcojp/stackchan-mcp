// StackChan FW-A2 plan 2B-1 (design §3.2, §3.3, §3.8, contract S7): what a link's receive task
// does with the bytes it gets, pure (no ESP-IDF): decode the frames, mark every frame as a
// receive (F1), answer a ping through the link's send queue, end the pair on a close, a broken
// frame or a passive disconnect (once, through the link's phase), report the hello reply once,
// and hand every message to its owner (the gate, UiController, the app) through the ports.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "link_manager.h"
#include "rx_route.h"
#include "send_queue.h"
#include "tx_loop.h"
#include "ws_frame.h"

struct cJSON;

namespace stackchan::link {

struct RxPorts {
    std::function<int64_t()> now_us;
    std::function<bool(const Input&)> post;  // the manager's notice queue (never waits)
    // Every frame, fragments and ping / pong / close included, marks a receive (F1). Marked once
    // per batch of bytes with at least one whole frame, at the time the bytes arrived.
    std::function<void(int64_t now_us)> received;
    // The link's own send queue (a ping's pong)
    std::function<net::PushResult(uint64_t e, std::string payload, int64_t now_us)> push_pong;
    // A pong that does not fit the open queue ends the pair (design §4.1): the gate stops the sound
    std::function<void(uint64_t e, EndReason reason)> stop_for_death;
    // audio link
    std::function<void(const AudioHelloReply& reply, const cJSON* root)> hello_reply;  // apply time, rates
    std::function<void(uint64_t e, const wire::TtsStart& t)> tts_start;
    std::function<void(uint64_t e, uint32_t gen)> tts_stop;
    std::function<void(uint64_t e, AudioIn audio)> server_audio;
    std::function<void(uint64_t e, const wire::GwListen& listen)> gw_listen;
    std::function<void(uint64_t e, const cJSON* root)> app_json;  // tts display states, mcp, llm, ...
    // control link
    std::function<void(uint64_t e, const wire::AbortRequest& req)> abort;
    std::function<void(uint64_t e, const std::string& req_id)> stat;
};

struct RxStats {
    uint32_t frames = 0;
    uint32_t dropped = 0;      // routed to kDrop, or a binary frame on the control link
    uint32_t bad_audio = 0;    // a binary audio frame that did not decode
    uint32_t extra_hellos = 0;
    uint32_t pongs_lost = 0;   // a pong the send queue refused
    const char* last_drop = "";
};

class RxLink {
public:
    // e: the pair's E (the manager draws it before the audio connect; the control link is for
    // the same pair). session_id: the audio hello reply's session for the control link (its stat
    // checks); "" for the audio link, which learns it from its hello reply. audio_version:
    // Protocol-Version of the binary frames.
    RxLink(LinkSide side, uint32_t attempt, uint64_t e, std::string session_id, int audio_version, LinkPhase* phase,
           RxPorts ports);
    uint64_t epoch() const { return e_; }
    // The session of the audio hello reply ("" before it). Written before the hello reply is
    // posted, so the manager reads it after taking that notice.
    const std::string& session_id() const { return session_id_; }

    // Bytes from the socket (the handshake's leftover first, then every receive)
    void OnBytes(const char* data, size_t len);
    // The socket ended (closed by the peer, an error)
    void OnDisconnected();
    bool ended() const { return ended_; }
    const RxStats& stats() const { return stats_; }

private:
    void OnMessage(const wsframe::Message& m, int64_t now);
    void OnText(const std::string& text, int64_t now);
    void End(EndReason reason);
    void Drop(const char* why);

    const LinkSide side_;
    const uint32_t attempt_;
    const uint64_t e_;
    const int audio_version_;
    LinkPhase* phase_;
    RxPorts p_;
    wsframe::Decoder decoder_;
    std::string session_id_;
    bool ended_ = false;
    RxStats stats_;
};

}  // namespace stackchan::link
