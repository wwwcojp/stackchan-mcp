// StackChan FW-A2: socket steps with deadlines (see include/sock_slice.h).
#include "sock_slice.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>

namespace sockslice {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

timeval Tv(int64_t us) {
    timeval tv;
    tv.tv_sec = static_cast<time_t>(us / 1'000'000);
    tv.tv_usec = static_cast<suseconds_t>(us % 1'000'000);
    return tv;
}

int Fail(int fd, int code, int* err) {
    if (fd >= 0) close(fd);
    *err = code;
    return -1;
}

}  // namespace

int ConnectWithin(const char* ipv4, uint16_t port, int timeout_ms, const std::function<bool()>& stop, int* err) {
    *err = 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ipv4, &addr.sin_addr) != 1) return Fail(-1, EINVAL, err);
    const int64_t deadline = NowMs() + timeout_ms;

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return Fail(-1, errno, err);
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return Fail(fd, errno, err);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errno != EINPROGRESS) return Fail(fd, errno, err);
        for (;;) {
            if (stop()) return Fail(fd, ECANCELED, err);
            const int64_t left = deadline - NowMs();
            if (left <= 0) return Fail(fd, ETIMEDOUT, err);
            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(fd, &wr);
            timeval tv = Tv((left < kSliceMs ? left : kSliceMs) * 1000);
            const int r = select(fd + 1, nullptr, &wr, nullptr, &tv);
            if (r < 0) {
                if (errno == EINTR) continue;
                return Fail(fd, errno, err);
            }
            if (r == 0) continue;  // this slice passed: check stop and the deadline again
            int so_error = 0;
            socklen_t n = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &n) != 0) return Fail(fd, errno, err);
            if (so_error != 0) return Fail(fd, so_error, err);
            break;
        }
    }
    if (fcntl(fd, F_SETFL, flags) < 0) return Fail(fd, errno, err);  // blocking again
    return fd;
}

int SendSlice(int fd, const uint8_t* data, size_t len, int64_t timeout_us) {
    if (timeout_us < 1000) return -1;
    timeval tv = Tv(timeout_us);
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return -1;
    const ssize_t n = send(fd, data, len, 0);
    if (n > 0) return static_cast<int>(n);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
}

int RecvSlice(int fd, char* buf, size_t len, int timeout_ms, bool* closed) {
    *closed = false;
    if (timeout_ms < 1) return -1;
    timeval tv = Tv(static_cast<int64_t>(timeout_ms) * 1000);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return -1;
    const ssize_t n = recv(fd, buf, len, 0);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) {
        *closed = true;
        return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

}  // namespace sockslice
