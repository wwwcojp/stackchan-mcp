// StackChan FW-A2 §3.6 (v10): where the link manager connects (see gateway_target.h).
#include "gateway_target.h"

#include <algorithm>
#include <cctype>

namespace stackchan::link {

namespace {

void Add(std::vector<std::string>* out, const std::string& url) {
    if (url.empty() || std::find(out->begin(), out->end(), url) != out->end()) return;
    out->push_back(url);
}

// a decimal number in [0, max] without sign, spaces or leading zeros (other than "0")
bool Number(const std::string& s, uint32_t max, uint32_t* out) {
    if (s.empty() || s.size() > 5 || (s.size() > 1 && s[0] == '0')) return false;
    uint32_t v = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        v = v * 10 + static_cast<uint32_t>(c - '0');
    }
    if (v > max) return false;
    *out = v;
    return true;
}

bool IsIpv4(const std::string& host) {
    size_t start = 0;
    for (int part = 0; part < 4; part++) {
        const size_t dot = host.find('.', start);
        const bool last = part == 3;
        if (last != (dot == std::string::npos)) return false;
        const std::string piece = host.substr(start, last ? std::string::npos : dot - start);
        uint32_t v = 0;
        if (!Number(piece, 255, &v)) return false;
        start = dot + 1;
    }
    return true;
}

}  // namespace

std::vector<std::string> GatewayCandidates(const TargetInputs& in) {
    std::vector<std::string> out;
    const bool forced = in.force_kconfig && !in.kconfig_url.empty();
    if (forced) {
        Add(&out, in.kconfig_url);
    } else {
        Add(&out, in.nvs_url);
        if (in.nvs_url.empty()) Add(&out, in.kconfig_url);
    }
    std::string fallback = in.nvs_fallback;
    if (in.force_kconfig && !in.kconfig_fallback.empty()) {
        fallback = in.kconfig_fallback;
    } else if (fallback.empty()) {
        fallback = in.kconfig_fallback;
    }
    Add(&out, fallback);
    return out;
}

std::string CandidateFor(const std::vector<std::string>& candidates, uint32_t attempt) {
    if (candidates.empty() || attempt == 0) return "";
    return candidates[(attempt - 1) % candidates.size()];
}

WsTarget ParseWsTarget(const std::string& url) {
    WsTarget t;
    const std::string scheme = "ws://";
    if (url.compare(0, scheme.size(), scheme) != 0) {
        t.error = url.compare(0, 6, "wss://") == 0 ? "wss is not supported" : "not a ws:// URL";
        return t;
    }
    const std::string rest = url.substr(scheme.size());
    const size_t slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    if (slash != std::string::npos) t.path = rest.substr(slash);
    const size_t colon = authority.find(':');
    t.host = authority.substr(0, colon);
    if (colon != std::string::npos) {
        uint32_t port = 0;
        if (!Number(authority.substr(colon + 1), 65535, &port) || port == 0) {
            t.error = "bad port";
            return t;
        }
        t.port = static_cast<uint16_t>(port);
    }
    if (!IsIpv4(t.host)) {
        t.error = "the host is not an IPv4 address";
        return t;
    }
    t.ok = true;
    return t;
}

}  // namespace stackchan::link
