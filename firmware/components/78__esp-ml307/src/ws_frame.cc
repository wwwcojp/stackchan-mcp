// StackChan FW-A2: the WebSocket frame parts (see include/ws_frame.h).
#include "ws_frame.h"

namespace wsframe {

namespace {

bool IsControl(uint8_t op) { return (op & 0x8) != 0; }

}  // namespace

void Decoder::Feed(const char* data, size_t len) {
    if (*error_ != '\0' || len == 0) return;
    if (offset_ > 0 && (offset_ == buffer_.size() || offset_ > 4096)) {  // drop what was consumed
        buffer_.erase(0, offset_);
        offset_ = 0;
    }
    buffer_.append(data, len);
}

Status Decoder::Fail(const char* why) {
    error_ = why;
    buffer_.clear();
    offset_ = 0;
    fragment_.clear();
    return Status::kError;
}

Status Decoder::Next(Message* out) {
    for (;;) {
        if (*error_ != '\0') return Status::kError;
        const size_t avail = buffer_.size() - offset_;
        if (avail < 2) return Status::kNeedMore;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(buffer_.data()) + offset_;
        const bool fin = (b[0] & 0x80) != 0;
        const uint8_t op = b[0] & 0x0F;
        if ((b[0] & 0x70) != 0) return Fail("reserved bits");
        if (op != 0x0 && op != 0x1 && op != 0x2 && op != 0x8 && op != 0x9 && op != 0xA) {
            return Fail("unknown opcode");
        }
        const bool masked = (b[1] & 0x80) != 0;
        uint64_t len = b[1] & 0x7F;
        size_t head = 2;
        if (len == 126) {
            if (avail < 4) return Status::kNeedMore;
            len = (static_cast<uint64_t>(b[2]) << 8) | b[3];
            head = 4;
        } else if (len == 127) {
            if (avail < 10) return Status::kNeedMore;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | b[2 + i];
            head = 10;
        }
        if (IsControl(op)) {
            if (!fin) return Fail("fragmented control frame");
            if (len > kMaxControl) return Fail("control frame longer than 125");
        } else {
            const size_t have = (op == 0x0) ? fragment_.size() : 0;
            if (len > max_message_ || have + len > max_message_) return Fail("message over the limit");
            if (op == 0x0 && !fragmenting_) return Fail("continuation without a start");
            if (op != 0x0 && fragmenting_) return Fail("new data frame while fragmenting");
        }
        const size_t mask_len = masked ? 4 : 0;
        if (avail < head + mask_len + len) return Status::kNeedMore;
        const uint8_t* mask = b + head;
        const char* payload = reinterpret_cast<const char*>(b + head + mask_len);
        std::string data(payload, static_cast<size_t>(len));
        if (masked) {
            for (size_t i = 0; i < data.size(); i++) data[i] = static_cast<char>(data[i] ^ mask[i % 4]);
        }
        offset_ += head + mask_len + static_cast<size_t>(len);
        frames_++;

        if (IsControl(op)) {
            out->kind = op == 0x8 ? Kind::kClose : (op == 0x9 ? Kind::kPing : Kind::kPong);
            out->payload = std::move(data);
            return Status::kMessage;
        }
        if (op != 0x0) {
            fragment_kind_ = op == 0x1 ? Kind::kText : Kind::kBinary;
            fragment_.clear();
        }
        fragment_ += data;
        if (!fin) {
            fragmenting_ = true;
            continue;
        }
        fragmenting_ = false;
        out->kind = fragment_kind_;
        out->payload = std::move(fragment_);
        fragment_.clear();
        return Status::kMessage;
    }
}

std::string Encode(Opcode op, const void* data, size_t len, const uint8_t mask[4]) {
    std::string f;
    f.reserve(len + 14);
    f.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(op)));
    if (len < 126) {
        f.push_back(static_cast<char>(0x80 | len));
    } else if (len <= 0xFFFF) {
        f.push_back(static_cast<char>(0x80 | 126));
        f.push_back(static_cast<char>((len >> 8) & 0xFF));
        f.push_back(static_cast<char>(len & 0xFF));
    } else {
        f.push_back(static_cast<char>(0x80 | 127));
        const uint64_t n = len;
        for (int i = 7; i >= 0; i--) f.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
    }
    f.append(reinterpret_cast<const char*>(mask), 4);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) f.push_back(static_cast<char>(p[i] ^ mask[i % 4]));
    return f;
}

std::string BuildHandshake(const std::string& host, uint16_t port, const std::string& path,
                           const std::vector<std::pair<std::string, std::string>>& headers,
                           const std::string& key_base64) {
    std::string r = "GET " + path + " HTTP/1.1\r\n";
    r += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    r += "Upgrade: websocket\r\n";
    r += "Connection: Upgrade\r\n";
    r += "Sec-WebSocket-Version: 13\r\n";
    r += "Sec-WebSocket-Key: " + key_base64 + "\r\n";
    for (const auto& h : headers) r += h.first + ": " + h.second + "\r\n";
    r += "\r\n";
    return r;
}

HandshakeStatus ParseHandshake(std::string* buffer) {
    const size_t end = buffer->find("\r\n\r\n");
    if (end == std::string::npos) {
        return buffer->size() > kMaxHandshakeResponse ? HandshakeStatus::kFailed : HandshakeStatus::kNeedMore;
    }
    if (end + 4 > kMaxHandshakeResponse) return HandshakeStatus::kFailed;
    if (buffer->compare(0, 12, "HTTP/1.1 101") != 0 || ((*buffer)[12] != ' ' && (*buffer)[12] != '\r')) {
        return HandshakeStatus::kFailed;
    }
    buffer->erase(0, end + 4);
    return HandshakeStatus::kOk;
}

std::string Base64(const uint8_t* data, size_t len) {
    static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        const size_t n = len - i < 3 ? len - i : 3;
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (n > 1 ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
                           (n > 2 ? data[i + 2] : 0);
        out.push_back(kChars[(v >> 18) & 0x3F]);
        out.push_back(kChars[(v >> 12) & 0x3F]);
        out.push_back(n > 1 ? kChars[(v >> 6) & 0x3F] : '=');
        out.push_back(n > 2 ? kChars[v & 0x3F] : '=');
    }
    return out;
}

}  // namespace wsframe
