// StackChan FW-A2 (design §4.2 change 6, STACKCHAN_CHANGES.md): the WebSocket frame parts,
// pure (no FreeRTOS, no sockets) so the host tests can drive them. The receive task decodes the
// server's frames with Decoder (every frame, control frames included, comes out as a message;
// nothing is answered here). The send task encodes client frames with Encode (one frame per
// message, always FIN, always masked). The connect worker builds and reads the opening
// handshake.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wsframe {

enum class Opcode : uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

enum class Kind { kText, kBinary, kClose, kPing, kPong };

struct Message {
    Kind kind = Kind::kText;
    std::string payload;
};

constexpr size_t kMaxMessage = 64 * 1024;  // a data message (all its fragments) at most
constexpr size_t kMaxControl = 125;        // RFC 6455 §5.5

enum class Status { kMessage, kNeedMore, kError };

// Decodes the server's byte stream in any split. Data frames are joined across fragments;
// control frames (ping, pong, close) come out as they arrive, also between fragments. A broken
// frame (reserved bits, unknown opcode, a fragmented or long control frame, a continuation
// without a start, a new data frame while fragmenting, a length over the limit) is an error
// that stays: the link ends. Masked server frames are unmasked (today's WebSocket accepts them).
class Decoder {
public:
    explicit Decoder(size_t max_message = kMaxMessage) : max_message_(max_message) {}
    void Feed(const char* data, size_t len);
    // The next message, kNeedMore until the bytes for one have arrived, or kError (stays).
    Status Next(Message* out);
    const char* error() const { return error_; }
    // Frames decoded so far, every fragment and control frame included (the receive side marks a
    // receive for F1 on every frame, not only on a whole message: design §3.3).
    uint64_t frames() const { return frames_; }

private:
    Status Fail(const char* why);

    const size_t max_message_;
    std::string buffer_;
    size_t offset_ = 0;  // consumed bytes at the front of buffer_
    bool fragmenting_ = false;
    Kind fragment_kind_ = Kind::kText;
    std::string fragment_;
    const char* error_ = "";
    uint64_t frames_ = 0;
};

// One client frame: FIN set, masked with `mask` (the caller draws it at random; the tests pass
// a fixed one). len may be 0 (data may then be null).
std::string Encode(Opcode op, const void* data, size_t len, const uint8_t mask[4]);

// The opening handshake request (RFC 6455 §4.1). headers are sent in the given order after
// Host, Upgrade, Connection, Sec-WebSocket-Version and Sec-WebSocket-Key.
std::string BuildHandshake(const std::string& host, uint16_t port, const std::string& path,
                           const std::vector<std::pair<std::string, std::string>>& headers,
                           const std::string& key_base64);

enum class HandshakeStatus { kNeedMore, kOk, kFailed };
constexpr size_t kMaxHandshakeResponse = 4096;
// Reads the response at the front of *buffer: kOk when the status line is "HTTP/1.1 101", and
// then removes the response from *buffer (bytes after it are frames). kFailed for any other
// status, or no blank line within kMaxHandshakeResponse bytes.
HandshakeStatus ParseHandshake(std::string* buffer);

std::string Base64(const uint8_t* data, size_t len);

}  // namespace wsframe
