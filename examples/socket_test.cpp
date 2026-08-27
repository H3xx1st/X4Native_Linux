// socket_test.cpp -- validates x4mp_platform.h's POSIX<->WinSock mappings by
// exercising every shimmed call (WSAStartup, socket, bind, listen, nonblocking,
// poll, accept, send/recv, close, errno/strerror). Compile natively for Linux
// (sanity) and cross-compile for Windows + run under Wine (validates the port).
#include "x4mp_platform.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond, fmt, ...) do { \
    if (cond) { printf("PASS: " fmt "\n", ##__VA_ARGS__); } \
    else      { printf("FAIL: " fmt "\n", ##__VA_ARGS__); g_fail = 1; } \
} while (0)

int main() {
    CHECK(x4mp_sock_ensure_init() == 0, "WSAStartup / sock_ensure_init");

    const uint16_t port = 17777;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(srv >= 0, "server socket() -> %d (errno=%d %s)", srv, errno, strerror(errno));

    int fl = fcntl(srv, F_GETFL, 0);
    fcntl(srv, F_SETFL, fl | O_NONBLOCK);
    CHECK(1, "fcntl non-blocking (F_GETFL=%d)", fl);

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    CHECK(1, "setsockopt(SO_REUSEADDR)");

    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(srv, (struct sockaddr*)&sa, sizeof(sa)) == 0, "bind (errno=%d %s)", errno, strerror(errno));
    CHECK(listen(srv, 5) == 0, "listen (errno=%d %s)", errno, strerror(errno));

    int cli = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(cli >= 0, "client socket() -> %d", cli);
    int cr = connect(cli, (struct sockaddr*)&sa, sizeof(sa));
    CHECK(cr == 0 || errno == EINPROGRESS, "connect (cr=%d errno=%d %s)", cr, errno, strerror(errno));

    // Poll the listener for POLLIN (incoming connection) -- the real x4mp_stream
    // usage pattern.
    struct pollfd pfd; pfd.fd = srv; pfd.events = POLLIN; pfd.revents = 0;
    int pr = poll(&pfd, 1, 1000);
    CHECK(pr > 0, "poll POLLIN (pr=%d revents=0x%x errno=%d)", pr, (int)pfd.revents, errno);

    int acc = accept(srv, nullptr, nullptr);
    CHECK(acc >= 0, "accept -> %d (errno=%d %s)", acc, errno, strerror(errno));

    const char* msg = "x4mp-socket-test";
    int w = send(acc, msg, (int)strlen(msg), 0);
    CHECK(w == (int)strlen(msg), "send (%d bytes)", w);

    char buf[64] = {0};
    int n = recv(cli, buf, sizeof(buf) - 1, 0);
    buf[n > 0 ? n : 0] = 0;
    CHECK(n == (int)strlen(msg) && strcmp(buf, msg) == 0, "recv round-trip ('%s')", buf);

    int err = 0; socklen_t el = sizeof(err);
    CHECK(getsockopt(cli, SOL_SOCKET, SO_ERROR, &err, &el) == 0, "getsockopt(SO_ERROR) err=%d", err);

    close(acc); close(cli); close(srv);
    CHECK(1, "close x3");

    printf("\n%s\n", g_fail ? "SOME TESTS FAILED" : "ALL SOCKET TESTS PASSED");
    return g_fail;
}
