// StackChan FW-A2 plan 2B-1 (design §3.6, contract S3): what one connect worker does, decided
// before it touches a socket, pure: the URL of the attempt, the ws://IPv4 target and the request
// headers (today's WebsocketProtocol: Authorization, Protocol-Version, Device-Id, Client-Id; the
// control link adds X-Stackchan-Role: control). The worker reads NVS and Kconfig into
// ConnectInputs once per attempt (self.gateway_config.set takes effect on the next attempt).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "gateway_target.h"
#include "tx_loop.h"

namespace stackchan::link {

struct ConnectInputs {
    TargetInputs targets;
    std::string nvs_token;      // websocket.token
    std::string kconfig_token;  // CONFIG_DEFAULT_WEBSOCKET_TOKEN ("" when not set)
    int nvs_version = 0;        // websocket.version (0: not set, version 1)
    std::string device_id;      // the MAC address
    std::string client_id;      // the board's UUID
};

struct ConnectPlan {
    bool ok = false;
    std::string url;
    WsTarget target;
    std::vector<std::pair<std::string, std::string>> headers;
    int version = 1;     // Protocol-Version of the binary audio frames
    std::string error;   // why there is no plan (logged; the attempt is a connect failure)
};

// The audio link's URL for attempt n (rotating through the candidates, design §3.6); "" when
// there is none.
std::string AudioUrlFor(const ConnectInputs& in, uint32_t attempt);

// The audio hello (today's WebsocketProtocol::GetHelloMessage + contract S1): type, version,
// features {aec (server AEC builds only), mcp, stackchan_ext: 1, stackchan_ctrl: 1}, transport,
// audio_params {opus, 16000 Hz, 1 channel, frame_duration}. fw_epoch / seq are stamped by the
// send task. The caller owns the result.
std::string BuildAudioHello(int version, bool server_aec, int frame_duration_ms);

// The plan for one link. The control link takes the audio link's URL of the same pair (the same
// host and port, contract S3), not a fresh candidate.
ConnectPlan PlanConnect(const ConnectInputs& in, const std::string& url, LinkSide side);

}  // namespace stackchan::link
