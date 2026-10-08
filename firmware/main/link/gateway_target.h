// StackChan FW-A2 §3.6 (v10): where the link manager connects. Pure: the shell reads NVS and
// Kconfig and passes the strings in, once per attempt (so self.gateway_config.set takes effect
// on the next attempt). One attempt uses one candidate; attempts rotate through them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace stackchan::link {

struct TargetInputs {
    std::string nvs_url;           // websocket.url
    std::string kconfig_url;       // CONFIG_DEFAULT_WEBSOCKET_URL ("" when not set)
    bool force_kconfig = false;    // CONFIG_FORCE_DEFAULT_WEBSOCKET_URL
    std::string nvs_fallback;      // websocket.fallback_url
    std::string kconfig_fallback;  // CONFIG_DEFAULT_WEBSOCKET_FALLBACK_URL ("" when not set)
};

// The same order as today's OpenAudioChannelInternal (websocket_protocol.cc:269-350) without
// mDNS (out of FW-A2's scope; K151 has it off): empty and duplicate URLs are skipped.
std::vector<std::string> GatewayCandidates(const TargetInputs& in);

// The candidate for attempt n (1-based), rotating. Empty when there is none.
std::string CandidateFor(const std::vector<std::string>& candidates, uint32_t attempt);

struct WsTarget {
    bool ok = false;
    std::string host;  // a dotted IPv4 address
    uint16_t port = 80;
    std::string path = "/";
    std::string error;  // why it was refused (logged)
};

// ws://<IPv4>[:port][/path] only. wss:// is refused (design §4.2 change 7) and so is a host
// name: gethostbyname has no deadline and could hold the connect worker past the 3 s exit
// wait of an ending pair (design §3.6, v10).
WsTarget ParseWsTarget(const std::string& url);

}  // namespace stackchan::link
