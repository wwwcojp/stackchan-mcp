// FW-A design §2.1 / §5: abort request acceptance, transition, dropped_ms, reply shape.
#include <gtest/gtest.h>
#include <cJSON.h>

#include "abort_policy.h"

using namespace stackchan;

namespace {

struct Json {
    cJSON* root;
    explicit Json(const char* text) : root(cJSON_Parse(text)) {}
    ~Json() { cJSON_Delete(root); }
};

TEST(AbortPolicy, AcceptsRequestWithMatchingSession) {
    Json j("{\"session_id\":\"s1\",\"type\":\"abort\",\"reason\":\"hush\"}");
    std::string reason;
    EXPECT_TRUE(IsAbortRequest(j.root, "s1", &reason));
    EXPECT_EQ(reason, "hush");
}

TEST(AbortPolicy, ReasonDefaultsToEmpty) {
    Json j("{\"session_id\":\"s1\",\"type\":\"abort\"}");
    std::string reason = "x";
    EXPECT_TRUE(IsAbortRequest(j.root, "s1", &reason));
    EXPECT_EQ(reason, "");
}

TEST(AbortPolicy, RejectsReplyShapedAbort) {
    Json j("{\"session_id\":\"s1\",\"type\":\"abort\",\"state\":\"done\",\"dropped_ms\":120}");
    EXPECT_FALSE(IsAbortRequest(j.root, "s1", nullptr));
}

TEST(AbortPolicy, RejectsMissingOrMismatchedSession) {
    Json missing("{\"type\":\"abort\"}");
    Json other("{\"session_id\":\"s2\",\"type\":\"abort\"}");
    EXPECT_FALSE(IsAbortRequest(missing.root, "s1", nullptr));
    EXPECT_FALSE(IsAbortRequest(other.root, "s1", nullptr));
    EXPECT_FALSE(IsAbortRequest(other.root, "", nullptr));  // no session yet
}

TEST(AbortPolicy, RejectsEmptySessionEvenIfEqual) {
    // Before the server hello the device has no session yet; an abort with an empty
    // session_id must not match it (Review Focus 4 in the FW-A plan).
    Json empty("{\"session_id\":\"\",\"type\":\"abort\"}");
    EXPECT_FALSE(IsAbortRequest(empty.root, "", nullptr));
}

TEST(AbortPolicy, RejectsOtherTypesAndNonObjects) {
    Json tts("{\"session_id\":\"s1\",\"type\":\"tts\",\"state\":\"stop\"}");
    Json arr("[1]");
    EXPECT_FALSE(IsAbortRequest(tts.root, "s1", nullptr));
    EXPECT_FALSE(IsAbortRequest(arr.root, "s1", nullptr));
    EXPECT_FALSE(IsAbortRequest(nullptr, "s1", nullptr));
}

TEST(AbortPolicy, OnlySpeakingReturnsToIdle) {
    EXPECT_TRUE(AbortReturnsToIdle(kDeviceStateSpeaking));
    EXPECT_FALSE(AbortReturnsToIdle(kDeviceStateIdle));
    EXPECT_FALSE(AbortReturnsToIdle(kDeviceStateListening));
    EXPECT_FALSE(AbortReturnsToIdle(kDeviceStateConnecting));
}

TEST(AbortPolicy, DroppedMsIsPacketsTimesFrame) {
    EXPECT_EQ(DroppedMs(0), 0u);
    EXPECT_EQ(DroppedMs(33), 1980u);
    EXPECT_EQ(DroppedMs(2, 20), 40u);
}

TEST(AbortPolicy, DoneReplyShape) {
    cJSON* r = BuildAbortDone("s1", "hush", 1980);
    EXPECT_STREQ(cJSON_GetObjectItem(r, "session_id")->valuestring, "s1");
    EXPECT_STREQ(cJSON_GetObjectItem(r, "type")->valuestring, "abort");
    EXPECT_STREQ(cJSON_GetObjectItem(r, "state")->valuestring, "done");
    EXPECT_STREQ(cJSON_GetObjectItem(r, "reason")->valuestring, "hush");
    EXPECT_EQ(cJSON_GetObjectItem(r, "dropped_ms")->valuedouble, 1980);
    EXPECT_FALSE(IsAbortRequest(r, "s1", nullptr));  // a reply is never a request
    cJSON_Delete(r);
}

}  // namespace
