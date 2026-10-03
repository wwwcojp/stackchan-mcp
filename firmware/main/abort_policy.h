#pragma once

// StackChan FW-A: pure decisions for the server->device "abort" message.
// See stackchan-works docs/superpowers/specs/2026-10-02-fw-a-design.md §2.1.
// No ESP-IDF dependency so it can be unit-tested on the host.

#include <cstdint>
#include <string>

#include "device_state.h"

struct cJSON;

namespace stackchan {

// True only for a request: {"type":"abort"} without "state", whose session_id
// equals the current one. A reply-shaped message ({"state":"done"}) is never a request.
// reason_out receives "reason" (empty if absent).
bool IsAbortRequest(const cJSON* root, const std::string& session_id, std::string* reason_out);

// Only Speaking returns to Idle on abort; other states are left alone.
bool AbortReturnsToIdle(DeviceState state);

// Diagnostic estimate of discarded audio: queued packets * frame duration.
uint32_t DroppedMs(uint32_t cleared_packets, uint32_t frame_ms = 60);

// {"session_id", "type":"abort", "state":"done", "reason", "dropped_ms"}.
// fw_epoch/seq are added later by Protocol::SendJson. Caller owns the result.
cJSON* BuildAbortDone(const std::string& session_id, const std::string& reason, uint32_t dropped_ms);

}  // namespace stackchan
