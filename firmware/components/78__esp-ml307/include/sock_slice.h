// StackChan FW-A2 (design §4.2 changes 1-3, STACKCHAN_CHANGES.md): socket steps with deadlines,
// POSIX calls only (lwIP on the device, Linux in the host tests). Every wait is cut into slices
// of at most 200 ms so a caller can see a stop request. A timeout of 0 is never given to a socket
// (lwIP takes SO_SNDTIMEO / SO_RCVTIMEO 0 as "block forever"): under 1 ms is refused.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace sockslice {

constexpr int kSliceMs = 200;

// Connects to a dotted IPv4 address within timeout_ms with a non-blocking connect() and select(),
// checking stop() at least every kSliceMs. Returns the socket (blocking again) or -1 with *err:
// EINVAL (not an IPv4 address), ETIMEDOUT (deadline), ECANCELED (stop),
// or the connect error (ECONNREFUSED, ...).
int ConnectWithin(const char* ipv4, uint16_t port, int timeout_ms, const std::function<bool()>& stop, int* err);

// One send() with SO_SNDTIMEO = timeout_us: the bytes sent (> 0), 0 when the timeout passed with
// nothing sent (EAGAIN), -1 on an error or a timeout under 1 ms.
int SendSlice(int fd, const uint8_t* data, size_t len, int64_t timeout_us);

// One recv() with SO_RCVTIMEO = timeout_ms: the bytes received (> 0), 0 on the timeout (EAGAIN),
// -1 when the peer closed (*closed = true) or on an error or a timeout under 1 ms (*closed = false).
int RecvSlice(int fd, char* buf, size_t len, int timeout_ms, bool* closed);

}  // namespace sockslice
