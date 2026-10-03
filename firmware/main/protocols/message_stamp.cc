#include "message_stamp.h"

#include <cJSON.h>

#include <cstring>

namespace stackchan {

uint32_t NextBootCount(int32_t stored) {
    uint32_t bits;
    std::memcpy(&bits, &stored, sizeof(bits));
    return bits + 1u;  // unsigned: wraps at 2^32
}

int32_t BootCountToStore(uint32_t boot_count) {
    int32_t stored;
    std::memcpy(&stored, &boot_count, sizeof(stored));
    return stored;
}

void MessageStamp::BeginConnection() {
    if (started_) {
        conn_index_ = static_cast<uint16_t>(conn_index_ + 1u);  // wraps at 65535
    }
    started_ = true;
    seq_ = 0;
}

void MessageStamp::Stamp(cJSON* root) {
    if (root == nullptr || !cJSON_IsObject(root)) {
        return;
    }
    ++seq_;
    cJSON_DeleteItemFromObject(root, "fw_epoch");
    cJSON_DeleteItemFromObject(root, "seq");
    cJSON_AddNumberToObject(root, "fw_epoch", static_cast<double>(fw_epoch()));
    cJSON_AddNumberToObject(root, "seq", static_cast<double>(seq_));
}

}  // namespace stackchan
