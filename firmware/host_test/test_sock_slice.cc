// StackChan FW-A2 plan 2B-1 (design §4.2 changes 1-3): the socket steps with deadlines of the
// vendored esp-ml307, POSIX only. On the host they run on Linux loopback sockets (lwIP has the
// same calls; how lwIP itself times out is checked on the device, plan 3).
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <string>
#include <vector>

#include "sock_slice.h"

using namespace sockslice;

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A listening socket on 127.0.0.1 (port chosen by the kernel)
struct Listener {
    int fd = -1;
    uint16_t port = 0;
    explicit Listener(int backlog) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        listen(fd, backlog);
        socklen_t n = sizeof(a);
        getsockname(fd, reinterpret_cast<sockaddr*>(&a), &n);
        port = ntohs(a.sin_port);
    }
    ~Listener() { close(fd); }
    int Accept() { return accept(fd, nullptr, nullptr); }
};

// A port with nothing listening (bound and closed again)
uint16_t ClosedPort() {
    Listener l(1);
    const uint16_t p = l.port;
    return p;
}

// A connected loopback pair: {client, server}
std::pair<int, int> Pair() {
    Listener l(4);
    int err = 0;
    const int c = ConnectWithin("127.0.0.1", l.port, 1000, [] { return false; }, &err);
    const int s = l.Accept();
    return {c, s};
}

}  // namespace

TEST(SockSlice, ConnectsToAListener) {
    Listener l(4);
    int err = -1;
    const int fd = ConnectWithin("127.0.0.1", l.port, 1000, [] { return false; }, &err);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(err, 0);
    // the socket is blocking again (the send / receive steps set their own timeouts)
    EXPECT_EQ(fcntl(fd, F_GETFL, 0) & O_NONBLOCK, 0);
    const char b = 'x';
    EXPECT_EQ(send(fd, &b, 1, 0), 1);
    close(fd);
}

TEST(SockSlice, RefusedAndBadHostsFailAtOnce) {
    int err = 0;
    const int64_t t0 = NowMs();
    EXPECT_EQ(ConnectWithin("127.0.0.1", ClosedPort(), 3000, [] { return false; }, &err), -1);
    EXPECT_EQ(err, ECONNREFUSED);
    EXPECT_LT(NowMs() - t0, 1000);
    EXPECT_EQ(ConnectWithin("gateway.local", 80, 3000, [] { return false; }, &err), -1);  // a name
    EXPECT_EQ(err, EINVAL);
}

// A listener whose accept queue is full drops the SYN: the connect stays in progress, so the
// deadline (and the stop request) are what end it.
TEST(SockSlice, AConnectInProgressEndsAtTheDeadlineOrTheStop) {
    Listener l(0);
    int err = 0;
    std::vector<int> held;
    for (int i = 0; i < 4; i++) {  // fill the accept queue (never accepted)
        const int fd = ConnectWithin("127.0.0.1", l.port, 300, [] { return false; }, &err);
        if (fd < 0) break;
        held.push_back(fd);
    }
    ASSERT_FALSE(held.empty());

    int64_t t0 = NowMs();
    EXPECT_EQ(ConnectWithin("127.0.0.1", l.port, 450, [] { return false; }, &err), -1);
    EXPECT_EQ(err, ETIMEDOUT);
    const int64_t took = NowMs() - t0;
    EXPECT_GE(took, 400);
    EXPECT_LT(took, 1500);

    std::atomic<int> polls{0};
    t0 = NowMs();
    EXPECT_EQ(ConnectWithin("127.0.0.1", l.port, 3000, [&] { return ++polls >= 2; }, &err), -1);
    EXPECT_EQ(err, ECANCELED);
    EXPECT_LT(NowMs() - t0, 1000);  // the stop is seen at the next 200 ms slice, not after 3 s
    for (int fd : held) close(fd);
}

TEST(SockSlice, ReceiveSlices) {
    auto [c, s] = Pair();
    char buf[16];
    bool closed = true;
    int64_t t0 = NowMs();
    EXPECT_EQ(RecvSlice(c, buf, sizeof(buf), 150, &closed), 0);  // nothing yet: timeout
    EXPECT_FALSE(closed);
    EXPECT_GE(NowMs() - t0, 100);
    ASSERT_EQ(send(s, "abc", 3, 0), 3);
    EXPECT_EQ(RecvSlice(c, buf, sizeof(buf), 150, &closed), 3);
    EXPECT_EQ(std::string(buf, 3), "abc");
    close(s);
    EXPECT_EQ(RecvSlice(c, buf, sizeof(buf), 150, &closed), -1);  // the peer closed
    EXPECT_TRUE(closed);
    close(c);
}

// A full send buffer: a slice sends what fits (or nothing) and returns at its timeout.
TEST(SockSlice, SendSlicesReturnAtTheirTimeout) {
    auto [c, s] = Pair();
    int small = 4096;
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    const std::vector<uint8_t> big(8 * 1024 * 1024, 0x5A);
    size_t sent = 0;
    int zero_slices = 0;
    const int64_t t0 = NowMs();
    while (sent < big.size() && NowMs() - t0 < 3000) {
        const int n = SendSlice(c, big.data() + sent, big.size() - sent, 100'000);
        ASSERT_GE(n, 0);
        if (n == 0) {
            zero_slices++;
            break;
        }
        sent += static_cast<size_t>(n);
    }
    EXPECT_EQ(zero_slices, 1);  // nobody reads: the buffer fills and a slice times out
    EXPECT_LT(sent, big.size());
    const int64_t t1 = NowMs();
    EXPECT_EQ(SendSlice(c, big.data(), big.size(), 120'000), 0);
    EXPECT_LT(NowMs() - t1, 1000);
    EXPECT_GE(NowMs() - t1, 80);
    close(s);
    close(c);
}

// A timeout under 1 ms is never passed to the socket (0 would mean "block forever" on lwIP).
TEST(SockSlice, ASliceUnderOneMillisecondIsRefused) {
    auto [c, s] = Pair();
    const uint8_t b = 1;
    EXPECT_EQ(SendSlice(c, &b, 1, 999), -1);
    ASSERT_EQ(send(s, "z", 1, 0), 1);  // data is there: only the refused timeout makes it -1
    usleep(50'000);
    char r;
    bool closed = false;
    EXPECT_EQ(RecvSlice(c, &r, 1, 0, &closed), -1);
    EXPECT_FALSE(closed);
    close(s);
    close(c);
}
