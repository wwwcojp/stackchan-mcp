#pragma once

// StackChan FW-A: stamp every device->server JSON with fw_epoch and seq.
//
//   fw_epoch = boot_count * 65536 + conn_index   (boot_count from NVS, conn_index per hello)
//   seq      = 1, 2, 3, ... per connection (hello is 1)
//
// Pure (no ESP-IDF dependency) so it can be unit-tested on the host.
// See stackchan-works docs/superpowers/specs/2026-10-02-fw-a-design.md §2.2.

#include <cstdint>

struct cJSON;

namespace stackchan {

class MessageStamp {
public:
    explicit MessageStamp(uint32_t boot_count) : boot_count_(boot_count) {}

    // Call once per WebSocket connection before stamping its hello.
    // Advances conn_index (first call keeps 0; wraps at 65535 -> 0) and resets seq to 0.
    void BeginConnection();

    void SetBootCount(uint32_t boot_count) { boot_count_ = boot_count; }

    uint64_t fw_epoch() const { return static_cast<uint64_t>(boot_count_) * 65536u + conn_index_; }
    uint32_t boot_count() const { return boot_count_; }
    uint16_t conn_index() const { return conn_index_; }
    uint32_t seq() const { return seq_; }

    // Pre-increments seq, then writes/overwrites "fw_epoch" and "seq" on root.
    // root must be a cJSON object; anything else is left untouched (and seq is not consumed).
    void Stamp(cJSON* root);

private:
    uint32_t boot_count_;
    uint16_t conn_index_ = 0;
    bool started_ = false;
    uint32_t seq_ = 0;
};

}  // namespace stackchan
