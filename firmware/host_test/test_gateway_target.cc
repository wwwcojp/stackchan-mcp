// StackChan FW-A2 §3.6 (v10): the gateway candidates, the rotation and the ws://IPv4 check.
#include <gtest/gtest.h>

#include "gateway_target.h"

using namespace stackchan::link;

namespace {
using V = std::vector<std::string>;
const std::string kNvs = "ws://192.168.10.119:8765";
const std::string kKconfig = "ws://192.168.10.2:8765";
const std::string kFallback = "ws://192.168.10.3:8765";
}  // namespace

TEST(GatewayTarget, NvsFirstThenTheFallback) {
    EXPECT_EQ(GatewayCandidates({kNvs, kKconfig, false, kFallback, ""}), (V{kNvs, kFallback}));
    // NVS empty: the build-time default takes its place
    EXPECT_EQ(GatewayCandidates({"", kKconfig, false, "", ""}), (V{kKconfig}));
    EXPECT_EQ(GatewayCandidates({"", "", false, "", ""}), V{});
}

TEST(GatewayTarget, ForceUsesTheBuildTimeUrlOnlyWhenItIsSet) {
    EXPECT_EQ(GatewayCandidates({kNvs, kKconfig, true, "", ""}), (V{kKconfig}));
    EXPECT_EQ(GatewayCandidates({kNvs, "", true, "", ""}), (V{kNvs}));  // nothing to force
}

TEST(GatewayTarget, TheFallbackFromNvsOrTheBuildTime) {
    EXPECT_EQ(GatewayCandidates({kNvs, "", false, "", kFallback}), (V{kNvs, kFallback}));
    EXPECT_EQ(GatewayCandidates({kNvs, "", false, kKconfig, kFallback}), (V{kNvs, kKconfig}));  // NVS wins
    EXPECT_EQ(GatewayCandidates({kNvs, "", true, kKconfig, kFallback}), (V{kNvs, kFallback}));  // forced
    EXPECT_EQ(GatewayCandidates({kNvs, "", true, kKconfig, ""}), (V{kNvs, kKconfig}));
}

TEST(GatewayTarget, DuplicatesAreSkipped) {
    EXPECT_EQ(GatewayCandidates({kNvs, "", false, kNvs, ""}), (V{kNvs}));
}

TEST(GatewayTarget, OneCandidatePerAttemptRotating) {
    const V c{kNvs, kFallback};
    EXPECT_EQ(CandidateFor(c, 1), kNvs);
    EXPECT_EQ(CandidateFor(c, 2), kFallback);
    EXPECT_EQ(CandidateFor(c, 3), kNvs);
    EXPECT_EQ(CandidateFor(V{}, 1), "");
    EXPECT_EQ(CandidateFor(c, 0), "");
}

TEST(GatewayTarget, WsWithAnIpv4Address) {
    WsTarget t = ParseWsTarget("ws://192.168.10.119:8765/xiaozhi/v1/");
    ASSERT_TRUE(t.ok) << t.error;
    EXPECT_EQ(t.host, "192.168.10.119");
    EXPECT_EQ(t.port, 8765);
    EXPECT_EQ(t.path, "/xiaozhi/v1/");
    t = ParseWsTarget("ws://10.0.0.1");
    ASSERT_TRUE(t.ok);
    EXPECT_EQ(t.port, 80);
    EXPECT_EQ(t.path, "/");
    EXPECT_TRUE(ParseWsTarget("ws://0.0.0.0:1").ok);
    EXPECT_TRUE(ParseWsTarget("ws://255.255.255.255:65535").ok);
}

TEST(GatewayTarget, WssNamesAndBrokenUrlsAreRefused) {
    for (const char* bad : {"wss://192.168.10.119:8765", "http://192.168.10.119", "192.168.10.119:8765",
                            "ws://gateway.local:8765", "ws://stackchan:8765", "ws://192.168.10:8765",
                            "ws://192.168.10.119.1:8765", "ws://192.168.10.256:8765", "ws://192.168.010.1:8765",
                            "ws://192.168.10.1:0", "ws://192.168.10.1:65536", "ws://192.168.10.1:80a",
                            "ws://192.168.10.1:", "ws://:8765", "ws://192.168.-1.1", "ws://"}) {
        EXPECT_FALSE(ParseWsTarget(bad).ok) << bad;
    }
    EXPECT_EQ(ParseWsTarget("wss://192.168.10.119").error, "wss is not supported");
    EXPECT_EQ(ParseWsTarget("ws://gateway.local").error, "the host is not an IPv4 address");
}
