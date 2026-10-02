// FW-A design §2.2 / §5: fw_epoch and seq stamping (pure, host-tested).
#include <gtest/gtest.h>
#include <cJSON.h>

#include "message_stamp.h"

using stackchan::MessageStamp;

namespace {

struct Json {
    cJSON* root;
    explicit Json(const char* text) : root(cJSON_Parse(text)) {}
    ~Json() { cJSON_Delete(root); }
    double num(const char* key) const { return cJSON_GetObjectItem(root, key)->valuedouble; }
    bool has(const char* key) const { return cJSON_HasObjectItem(root, key); }
};

TEST(MessageStamp, EpochIsBootCountTimes65536PlusConnIndex) {
    MessageStamp s(7);
    s.BeginConnection();
    EXPECT_EQ(s.conn_index(), 0);
    EXPECT_EQ(s.fw_epoch(), 7ull * 65536ull);
    s.BeginConnection();
    EXPECT_EQ(s.conn_index(), 1);
    EXPECT_EQ(s.fw_epoch(), 7ull * 65536ull + 1ull);
}

TEST(MessageStamp, ConnIndexWrapsAt65535) {
    MessageStamp s(1);
    for (int i = 0; i < 65536; ++i) {
        s.BeginConnection();
    }
    EXPECT_EQ(s.conn_index(), 65535);
    s.BeginConnection();
    EXPECT_EQ(s.conn_index(), 0);
    EXPECT_EQ(s.fw_epoch(), 65536ull);
}

TEST(MessageStamp, SeqStartsAtOnePerConnectionAndRestartsOnNewConnection) {
    MessageStamp s(3);
    s.BeginConnection();
    Json hello("{\"type\":\"hello\"}");
    s.Stamp(hello.root);
    EXPECT_EQ(hello.num("seq"), 1);
    EXPECT_EQ(hello.num("fw_epoch"), 3.0 * 65536);
    Json listen("{\"type\":\"listen\",\"state\":\"start\"}");
    s.Stamp(listen.root);
    EXPECT_EQ(listen.num("seq"), 2);
    s.BeginConnection();
    Json hello2("{\"type\":\"hello\"}");
    s.Stamp(hello2.root);
    EXPECT_EQ(hello2.num("seq"), 1);
    EXPECT_EQ(hello2.num("fw_epoch"), 3.0 * 65536 + 1);
}

TEST(MessageStamp, StampOverwritesExistingKeysAndKeepsOthers) {
    MessageStamp s(2);
    s.BeginConnection();
    Json j("{\"type\":\"abort\",\"state\":\"done\",\"fw_epoch\":9,\"seq\":99}");
    s.Stamp(j.root);
    EXPECT_EQ(j.num("fw_epoch"), 2.0 * 65536);
    EXPECT_EQ(j.num("seq"), 1);
    EXPECT_STREQ(cJSON_GetObjectItem(j.root, "state")->valuestring, "done");
    EXPECT_EQ(cJSON_GetArraySize(j.root), 4);
}

TEST(MessageStamp, NonObjectIsNotStampedAndDoesNotConsumeSeq) {
    MessageStamp s(2);
    s.BeginConnection();
    Json arr("[1,2]");
    s.Stamp(arr.root);
    s.Stamp(nullptr);
    EXPECT_EQ(s.seq(), 0u);
    Json obj("{}");
    s.Stamp(obj.root);
    EXPECT_EQ(obj.num("seq"), 1);
}

}  // namespace
