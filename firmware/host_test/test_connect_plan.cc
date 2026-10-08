// StackChan FW-A2 plan 2B-1: what one connect worker does before it touches a socket (design
// §3.6, contract S3).
#include <gtest/gtest.h>

#include <cJSON.h>

#include <string>
#include <utility>
#include <vector>

#include "connect_plan.h"

using namespace stackchan::link;

namespace {

ConnectInputs Inputs() {
    ConnectInputs in;
    in.targets.nvs_url = "ws://192.168.10.119:8765/ws";
    in.targets.nvs_fallback = "ws://192.168.10.120:8765";
    in.device_id = "aa:bb";
    in.client_id = "uuid-1";
    return in;
}

using Headers = std::vector<std::pair<std::string, std::string>>;

// A number / string field, or -1 / "<none>" when it is missing (a test must fail, not crash)
int Int(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : -1;
}
std::string Str(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "<none>";
}

}  // namespace

TEST(ConnectPlan, AttemptsRotateThroughTheCandidates) {
    const ConnectInputs in = Inputs();
    EXPECT_EQ(AudioUrlFor(in, 1), "ws://192.168.10.119:8765/ws");
    EXPECT_EQ(AudioUrlFor(in, 2), "ws://192.168.10.120:8765");
    EXPECT_EQ(AudioUrlFor(in, 3), "ws://192.168.10.119:8765/ws");
    EXPECT_EQ(AudioUrlFor(ConnectInputs{}, 1), "");
}

TEST(ConnectPlan, TheAudioLinkHeaders) {
    ConnectInputs in = Inputs();
    in.nvs_token = "abc";
    const ConnectPlan p = PlanConnect(in, AudioUrlFor(in, 1), LinkSide::kAudio);
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(p.target.host, "192.168.10.119");
    EXPECT_EQ(p.target.port, 8765);
    EXPECT_EQ(p.target.path, "/ws");
    EXPECT_EQ(p.version, 1);
    EXPECT_EQ(p.headers, (Headers{{"Authorization", "Bearer abc"},
                                  {"Protocol-Version", "1"},
                                  {"Device-Id", "aa:bb"},
                                  {"Client-Id", "uuid-1"}}));
}

// Contract S3: the control link goes to the same URL with the same authentication and the role.
TEST(ConnectPlan, TheControlLinkAddsItsRole) {
    ConnectInputs in = Inputs();
    in.nvs_version = 3;
    const ConnectPlan p = PlanConnect(in, "ws://192.168.10.120:8765", LinkSide::kCtrl);
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(p.target.host, "192.168.10.120");
    EXPECT_EQ(p.version, 3);
    EXPECT_EQ(p.headers, (Headers{{"Protocol-Version", "3"},
                                  {"Device-Id", "aa:bb"},
                                  {"Client-Id", "uuid-1"},
                                  {"X-Stackchan-Role", "control"}}));
}

// Today's token rules: NVS first, Kconfig when NVS is empty or forced; "Bearer " unless the
// token already has a scheme (a space).
TEST(ConnectPlan, TokenRules) {
    struct Case {
        std::string nvs, kconfig;
        bool force;
        std::string want;  // "" = no Authorization header
    };
    for (const Case& c : {Case{"abc", "kc", false, "Bearer abc"}, Case{"", "kc", false, "Bearer kc"},
                          Case{"abc", "kc", true, "Bearer kc"}, Case{"abc", "", true, "Bearer abc"},
                          Case{"Basic xyz", "", false, "Basic xyz"}, Case{"", "", false, ""}}) {
        ConnectInputs in = Inputs();
        in.nvs_token = c.nvs;
        in.kconfig_token = c.kconfig;
        in.targets.force_kconfig = c.force;
        const ConnectPlan p = PlanConnect(in, "ws://10.0.0.1", LinkSide::kAudio);
        ASSERT_TRUE(p.ok);
        std::string got;
        for (const auto& h : p.headers) {
            if (h.first == "Authorization") got = h.second;
        }
        EXPECT_EQ(got, c.want) << c.nvs << "|" << c.kconfig << "|" << c.force;
    }
}

// No URL, a host name or wss:// is a connect failure with its reason (design §3.6, §4.2 change 7).
TEST(ConnectPlan, NoPlanWithoutAUsableUrl) {
    const ConnectInputs in = Inputs();
    for (const char* url : {"", "ws://gateway.local:8765", "wss://192.168.10.119:8765"}) {
        const ConnectPlan p = PlanConnect(in, url, LinkSide::kAudio);
        EXPECT_FALSE(p.ok) << url;
        EXPECT_FALSE(p.error.empty()) << url;
        EXPECT_TRUE(p.headers.empty()) << url;
    }
}

// Today's hello plus contract S1 (features.stackchan_ctrl: 1); FW-A's stackchan_ext stays.
TEST(ConnectPlan, TheAudioHello) {
    const std::string text = BuildAudioHello(2, false, 40);
    cJSON* root = cJSON_Parse(text.c_str());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(Str(root, "type"), "hello");
    EXPECT_EQ(Int(root, "version"), 2);
    EXPECT_EQ(Str(root, "transport"), "websocket");
    const cJSON* f = cJSON_GetObjectItem(root, "features");
    EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(f, "mcp")));
    EXPECT_EQ(Int(f, "stackchan_ext"), 1);
    EXPECT_EQ(Int(f, "stackchan_ctrl"), 1);
    EXPECT_EQ(cJSON_GetObjectItem(f, "aec"), nullptr);
    const cJSON* a = cJSON_GetObjectItem(root, "audio_params");
    EXPECT_EQ(Str(a, "format"), "opus");
    EXPECT_EQ(Int(a, "sample_rate"), 16000);
    EXPECT_EQ(Int(a, "channels"), 1);
    EXPECT_EQ(Int(a, "frame_duration"), 40);
    EXPECT_EQ(cJSON_GetObjectItem(root, "fw_epoch"), nullptr);  // the send task stamps it
    cJSON_Delete(root);
    cJSON* aec = cJSON_Parse(BuildAudioHello(1, true, 60).c_str());
    EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(aec, "features"), "aec")));
    cJSON_Delete(aec);
}
