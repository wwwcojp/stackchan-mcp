// StackChan FW-A2 plan 2B-1: what the receive tasks decide, pure (cJSON only). The audio link's
// binary frames (Protocol-Version 1-3, as today's websocket_protocol.cc), the audio hello reply,
// and where a text message of each link goes (contract S7, design §2.2 and §4.1). The receive
// tasks act on the result; nothing here locks, posts or sends.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "wire.h"

struct cJSON;

namespace stackchan::link {

// ---- binary audio frames ----

struct AudioIn {
    uint32_t timestamp = 0;  // version 2 only (server-side AEC); 0 otherwise
    std::string payload;     // opus
};

// False when the header is cut short or its payload size is larger than the frame (today's code
// copied past the end). A frame longer than its header says keeps the header's size.
bool DecodeAudioFrame(int version, const uint8_t* data, size_t len, AudioIn* out);
// The mic audio frame for the audio send queue (today's WebsocketProtocol::SendAudio).
std::string EncodeAudioFrame(int version, uint32_t timestamp, const uint8_t* opus, size_t len);

// ---- the audio hello reply ----

struct AudioHelloReply {
    std::string session_id;
    bool ctrl_offered = false;  // "stackchan_ctrl": 1 (contract S2)
    int sample_rate = 0;        // audio_params; 0 when missing (the caller keeps its value)
    int frame_duration = 0;
};
// transport "websocket" and a non-empty string session_id, as today's ParseServerHello (the
// session is the key of every tts / listen / abort / stat check). False otherwise.
bool ParseAudioHelloReply(const cJSON* root, AudioHelloReply* out);

// ---- where a text message goes ----

enum class AudioRoute {
    kHelloReply,  // the receive task reports it once per link (LinkPhase::TakeHelloReply)
    kTtsStart,    // gate OnTtsStart
    kTtsStop,     // gate OnTtsStop
    kTtsOther,    // tts states for the display (sentence_start, ...): the app
    kListen,      // UiController GwListen
    kApp,         // mcp, llm, stt, system, alert, custom and unknown types: the app (plan 2B-2)
    kDrop,        // logged and counted (why)
};

struct AudioRouted {
    AudioRoute route = AudioRoute::kDrop;
    wire::TtsStart tts_start;
    uint32_t tts_stop_gen = 0;
    wire::GwListen listen;
    const char* why = "";
};

// tts and listen must carry the pair's session_id (an empty session matches nothing); an abort
// request on the audio link is dropped (contract S7: control link only); a broken tts start /
// stop or listen is dropped.
AudioRouted RouteAudioText(const cJSON* root, const std::string& session_id);

enum class CtrlRoute {
    kHelloReply,  // the manager compares audio_epoch with the pair (S4.1)
    kAbort,       // gate OnAbort (the gate compares the session: contract R1, S8)
    kStat,        // the stat reply (plan 2B-2)
    kDrop,
};

struct CtrlRouted {
    CtrlRoute route = CtrlRoute::kDrop;
    uint64_t audio_epoch = 0;
    wire::AbortRequest abort;
    std::string req_id;
    const char* why = "";
};

// The control link carries the hello reply, abort requests and stat requests only (contract
// S7); a stat request must carry the pair's session.
CtrlRouted RouteCtrlText(const cJSON* root, const std::string& session_id);

}  // namespace stackchan::link
