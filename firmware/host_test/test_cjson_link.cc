// StackChan FW-A T0: the host tests can build and link against cJSON
// (the ESP-IDF copy inside the espressif/idf image, or FetchContent).
#include <gtest/gtest.h>

#include <cJSON.h>

#include <string>

TEST(CjsonLink, RoundTripsAnObject) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "seq", 1);
    char* text = cJSON_PrintUnformatted(root);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(std::string(text), "{\"type\":\"hello\",\"seq\":1}");
    cJSON_free(text);
    cJSON_Delete(root);
}
