// StackChan FW-A2 plan 2B-1 (design §4.2 change 6): the WebSocket frame parts of the vendored
// esp-ml307, pure (no FreeRTOS, no sockets): decoding server frames, encoding client frames and
// the opening handshake.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ws_frame.h"

using namespace wsframe;

namespace {

const uint8_t kMask[4] = {0x11, 0x22, 0x33, 0x44};

// A server frame (unmasked unless a mask is given)
std::string Frame(uint8_t first, const std::string& payload, const uint8_t* mask = nullptr) {
    std::string f(1, static_cast<char>(first));
    const uint8_t m = mask ? 0x80 : 0x00;
    const size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(m | n));
    } else if (n <= 0xFFFF) {
        f.push_back(static_cast<char>(m | 126));
        f.push_back(static_cast<char>(n >> 8));
        f.push_back(static_cast<char>(n & 0xFF));
    } else {
        f.push_back(static_cast<char>(m | 127));
        for (int i = 7; i >= 0; i--) f.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF));
    }
    if (mask) {
        f.append(reinterpret_cast<const char*>(mask), 4);
        for (size_t i = 0; i < n; i++) f.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    } else {
        f += payload;
    }
    return f;
}

std::vector<Message> Drain(Decoder& d) {
    std::vector<Message> out;
    Message m;
    while (d.Next(&m) == Status::kMessage) out.push_back(m);
    return out;
}

}  // namespace

TEST(WsFrame, DecodesTextBinaryAndControlFrames) {
    Decoder d;
    std::string in = Frame(0x81, "{\"type\":\"tts\"}") + Frame(0x82, std::string("\x01\x02\x03", 3)) +
                     Frame(0x89, "pi") + Frame(0x8A, "po") + Frame(0x88, "");
    d.Feed(in.data(), in.size());
    auto got = Drain(d);
    ASSERT_EQ(got.size(), 5u);
    EXPECT_EQ(got[0].kind, Kind::kText);
    EXPECT_EQ(got[0].payload, "{\"type\":\"tts\"}");
    EXPECT_EQ(got[1].kind, Kind::kBinary);
    EXPECT_EQ(got[1].payload, std::string("\x01\x02\x03", 3));
    EXPECT_EQ(got[2].kind, Kind::kPing);
    EXPECT_EQ(got[2].payload, "pi");
    EXPECT_EQ(got[3].kind, Kind::kPong);
    EXPECT_EQ(got[3].payload, "po");
    EXPECT_EQ(got[4].kind, Kind::kClose);
    Message m;
    EXPECT_EQ(d.Next(&m), Status::kNeedMore);
}

// TCP hands the bytes over in any split: one byte at a time gives the same messages.
TEST(WsFrame, AnySplitOfTheBytesGivesTheSameMessages) {
    const std::string big(300, 'a');            // 126: 16-bit length
    const std::string huge(70000, 'b');         // 127: 64-bit length
    std::string in = Frame(0x81, "x") + Frame(0x82, big, kMask) + Frame(0x82, huge) + Frame(0x89, "");
    Decoder d(80000);
    std::vector<Message> got;
    for (char c : in) {
        d.Feed(&c, 1);
        auto part = Drain(d);
        got.insert(got.end(), part.begin(), part.end());
    }
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[0].payload, "x");
    EXPECT_EQ(got[1].payload, big);  // a masked server frame is unmasked (today's WebSocket accepts it)
    EXPECT_EQ(got[2].payload, huge);
    EXPECT_EQ(got[3].kind, Kind::kPing);
    EXPECT_EQ(got[3].payload, "");
}

// A large message consumed with the start of the next frame in the same read: dropping the
// consumed bytes keeps the partial frame.
TEST(WsFrame, APartialFrameSurvivesDroppingTheConsumedBytes) {
    Decoder d;
    const std::string first = Frame(0x82, std::string(5000, 'L'));
    const std::string second = Frame(0x81, "next");
    std::string part1 = first + second.substr(0, 3);
    d.Feed(part1.data(), part1.size());
    Message m;
    ASSERT_EQ(d.Next(&m), Status::kMessage);
    EXPECT_EQ(m.payload.size(), 5000u);
    EXPECT_EQ(d.Next(&m), Status::kNeedMore);
    const std::string rest = second.substr(3);
    d.Feed(rest.data(), rest.size());
    ASSERT_EQ(d.Next(&m), Status::kMessage);
    EXPECT_EQ(m.payload, "next");
}

// Fragments are joined; a control frame may come between them and is delivered at once.
TEST(WsFrame, FragmentsAreJoinedAroundControlFrames) {
    Decoder d;
    std::string in = Frame(0x01, "he") + Frame(0x89, "p") + Frame(0x00, "ll") + Frame(0x80, "o");
    d.Feed(in.data(), in.size());
    auto got = Drain(d);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].kind, Kind::kPing);
    EXPECT_EQ(got[1].kind, Kind::kText);
    EXPECT_EQ(got[1].payload, "hello");
}

// Every frame is counted as it is decoded, a fragment included (F1 counts frames, not messages:
// Codex review 148 Minor 3).
TEST(WsFrame, EveryFrameIsCountedFragmentsIncluded) {
    Decoder d;
    EXPECT_EQ(d.frames(), 0u);
    const std::string first = Frame(0x01, "he");  // a fragment: no message yet
    d.Feed(first.data(), first.size());
    Message m;
    EXPECT_EQ(d.Next(&m), Status::kNeedMore);
    EXPECT_EQ(d.frames(), 1u);
    const std::string rest = Frame(0x89, "p") + Frame(0x80, "llo");
    d.Feed(rest.data(), rest.size());
    ASSERT_EQ(d.Next(&m), Status::kMessage);  // the ping
    EXPECT_EQ(d.frames(), 2u);
    ASSERT_EQ(d.Next(&m), Status::kMessage);  // "hello"
    EXPECT_EQ(d.frames(), 3u);
    const std::string half = Frame(0x81, "x").substr(0, 1);  // not a whole frame yet
    d.Feed(half.data(), half.size());
    EXPECT_EQ(d.Next(&m), Status::kNeedMore);
    EXPECT_EQ(d.frames(), 3u);
}

// Every broken frame is an error that stays (the link ends; nothing more is decoded).
TEST(WsFrame, BrokenFramesAreErrorsThatStay) {
    struct Case {
        const char* name;
        std::string bytes;
    };
    const std::vector<Case> cases = {
        {"control frame longer than 125", Frame(0x89, std::string(126, 'p'))},
        {"fragmented control frame", Frame(0x09, "p")},
        {"new data frame while fragmenting", Frame(0x01, "a") + Frame(0x81, "b")},
        {"continuation without a start", Frame(0x80, "a")},
        {"reserved bits", Frame(0xC1, "a")},
        {"unknown opcode", Frame(0x83, "a")},
        {"message over the limit", Frame(0x82, std::string(65, 'z'))},
        {"fragments over the limit", Frame(0x02, std::string(40, 'z')) + Frame(0x80, std::string(40, 'z'))},
    };
    for (const auto& c : cases) {
        Decoder d(64);
        d.Feed(c.bytes.data(), c.bytes.size());
        Message m;
        Status s = Status::kMessage;
        while (s == Status::kMessage) s = d.Next(&m);
        EXPECT_EQ(s, Status::kError) << c.name;
        EXPECT_STRNE(d.error(), "") << c.name;
        const std::string ok = Frame(0x81, "after");
        d.Feed(ok.data(), ok.size());
        EXPECT_EQ(d.Next(&m), Status::kError) << c.name;
    }
}

// A 64-bit length with the top bit set, or longer than the limit, is refused before anything is
// buffered for it (today's WebSocket sized a std::vector from it).
TEST(WsFrame, HugeLengthsAreRefusedFromTheHeader) {
    Decoder d(1024);
    std::string head = {static_cast<char>(0x82), static_cast<char>(127)};
    for (int i = 0; i < 8; i++) head.push_back(static_cast<char>(i == 0 ? 0x80 : 0x00));
    d.Feed(head.data(), head.size());
    Message m;
    EXPECT_EQ(d.Next(&m), Status::kError);

    Decoder e(1024);
    std::string head2 = {static_cast<char>(0x82), static_cast<char>(127), 0, 0, 0, 0, 0, 0, 0x10, 0x00};
    e.Feed(head2.data(), head2.size());  // 4096 bytes announced, the limit is 1024
    EXPECT_EQ(e.Next(&m), Status::kError);
}

TEST(WsFrame, EncodesMaskedClientFrames) {
    const std::string p = "hi";
    const std::string f = Encode(Opcode::kText, p.data(), p.size(), kMask);
    ASSERT_EQ(f.size(), 2u + 4 + 2);
    EXPECT_EQ(static_cast<uint8_t>(f[0]), 0x81);
    EXPECT_EQ(static_cast<uint8_t>(f[1]), 0x80 | 2);
    EXPECT_EQ(f.substr(2, 4), std::string(reinterpret_cast<const char*>(kMask), 4));
    EXPECT_EQ(static_cast<uint8_t>(f[6]), 'h' ^ 0x11);
    EXPECT_EQ(static_cast<uint8_t>(f[7]), 'i' ^ 0x22);

    const std::string mid(300, 'm');
    const std::string g = Encode(Opcode::kBinary, mid.data(), mid.size(), kMask);
    EXPECT_EQ(static_cast<uint8_t>(g[0]), 0x82);
    EXPECT_EQ(static_cast<uint8_t>(g[1]), 0x80 | 126);
    EXPECT_EQ(static_cast<uint8_t>(g[2]), 300 >> 8);
    EXPECT_EQ(static_cast<uint8_t>(g[3]), 300 & 0xFF);
    EXPECT_EQ(g.size(), 4u + 4 + 300);

    const std::string edge(65535, 'e');  // the largest 16-bit length
    const std::string k = Encode(Opcode::kBinary, edge.data(), edge.size(), kMask);
    EXPECT_EQ(static_cast<uint8_t>(k[1]), 0x80 | 126);
    EXPECT_EQ(k.size(), 4u + 4 + 65535);

    const std::string big(70000, 'B');
    const std::string h = Encode(Opcode::kBinary, big.data(), big.size(), kMask);
    EXPECT_EQ(static_cast<uint8_t>(h[1]), 0x80 | 127);
    EXPECT_EQ(h.size(), 10u + 4 + 70000);
    EXPECT_EQ(static_cast<uint8_t>(h[7]), (70000 >> 16) & 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(h[8]), (70000 >> 8) & 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(h[9]), 70000 & 0xFF);

    const std::string pong = Encode(Opcode::kPong, "abc", 3, kMask);
    EXPECT_EQ(static_cast<uint8_t>(pong[0]), 0x8A);
    const std::string close = Encode(Opcode::kClose, nullptr, 0, kMask);
    EXPECT_EQ(static_cast<uint8_t>(close[0]), 0x88);
    EXPECT_EQ(static_cast<uint8_t>(close[1]), 0x80);
    EXPECT_EQ(close.size(), 6u);
}

// What the client sends is what the server decodes (the decoder accepts masked frames).
TEST(WsFrame, EncodeThenDecodeRoundTrips) {
    Decoder d(200000);
    for (size_t n : {0u, 1u, 125u, 126u, 65535u, 65536u, 70000u}) {
        std::string p(n, '\0');
        for (size_t i = 0; i < n; i++) p[i] = static_cast<char>(i * 7);
        const std::string f = Encode(Opcode::kBinary, p.data(), p.size(), kMask);
        d.Feed(f.data(), f.size());
        Message m;
        ASSERT_EQ(d.Next(&m), Status::kMessage) << n;
        EXPECT_EQ(m.kind, Kind::kBinary);
        EXPECT_EQ(m.payload, p) << n;
    }
}

TEST(WsFrame, HandshakeRequestCarriesTheHeaders) {
    const std::string r = BuildHandshake("192.168.10.119", 8765, "/ws",
                                         {{"Authorization", "Bearer t"}, {"X-Stackchan-Role", "control"}},
                                         "AAAAAAAAAAAAAAAAAAAAAA==");
    EXPECT_EQ(r.rfind("GET /ws HTTP/1.1\r\n", 0), 0u);
    for (const char* line : {"Host: 192.168.10.119:8765\r\n", "Upgrade: websocket\r\n", "Connection: Upgrade\r\n",
                             "Sec-WebSocket-Version: 13\r\n", "Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n",
                             "Authorization: Bearer t\r\n", "X-Stackchan-Role: control\r\n"}) {
        EXPECT_NE(r.find(line), std::string::npos) << line;
    }
    EXPECT_EQ(r.substr(r.size() - 4), "\r\n\r\n");
    EXPECT_EQ(r.find("\r\n\r\n"), r.size() - 4);  // one blank line, at the end
}

TEST(WsFrame, HandshakeResponseLeavesTheFollowingBytes) {
    std::string buf = "HTTP/1.1 101 Switching";
    EXPECT_EQ(ParseHandshake(&buf), HandshakeStatus::kNeedMore);
    buf += " Protocols\r\nUpgrade: websocket\r\n\r\n";
    buf += Frame(0x81, "first");
    EXPECT_EQ(ParseHandshake(&buf), HandshakeStatus::kOk);
    EXPECT_EQ(buf, Frame(0x81, "first"));  // a frame sent right after the response is kept

    std::string bare = "HTTP/1.1 101\r\n\r\n";  // no reason phrase
    EXPECT_EQ(ParseHandshake(&bare), HandshakeStatus::kOk);
    EXPECT_EQ(bare, "");
    std::string longer = "HTTP/1.1 1010 X\r\n\r\n";
    EXPECT_EQ(ParseHandshake(&longer), HandshakeStatus::kFailed);

    std::string bad = "HTTP/1.1 401 Unauthorized\r\n\r\n";
    EXPECT_EQ(ParseHandshake(&bad), HandshakeStatus::kFailed);
    std::string not_first_line = "HTTP/1.1 200 OK\r\nX: HTTP/1.1 101\r\n\r\n";
    EXPECT_EQ(ParseHandshake(&not_first_line), HandshakeStatus::kFailed);
    std::string endless(5000, 'h');  // no blank line within the header limit
    EXPECT_EQ(ParseHandshake(&endless), HandshakeStatus::kFailed);
}

TEST(WsFrame, Base64OfTheKey) {
    const uint8_t foobar[6] = {'f', 'o', 'o', 'b', 'a', 'r'};  // RFC 4648 §10
    EXPECT_EQ(Base64(foobar, 0), "");
    EXPECT_EQ(Base64(foobar, 1), "Zg==");
    EXPECT_EQ(Base64(foobar, 2), "Zm8=");
    EXPECT_EQ(Base64(foobar, 3), "Zm9v");
    EXPECT_EQ(Base64(foobar, 4), "Zm9vYg==");
    EXPECT_EQ(Base64(foobar, 6), "Zm9vYmFy");
    const uint8_t zeros[16] = {};
    EXPECT_EQ(Base64(zeros, 16).size(), 24u);  // a 16-byte key is 24 characters
}
