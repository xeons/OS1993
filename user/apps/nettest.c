/*
 * nettest.c -- tests of the TCP/IP stack and WSOCK.DLL.
 *
 *   nettest all TCPPORT UDPPORT   the checks; the host (10.0.2.2) runs TCP and
 *                                 UDP echo servers on those ports
 *   nettest serve                 accept one connection on port 7 and echo it
 *
 * stdout is the debug log: a line per failure and a total
 * (tools/qemu-net-test.py).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os1993.h"
#include "winsock.h"

static int failures, checks;

#define CHECK(cond, what) \
    do { checks++; if (!(cond)) { failures++; printf("NET: FAIL %s (line %d, error %d)\n", what, __LINE__, \
                                                    WSAGetLastError()); } } while (0)

static struct sockaddr_in addr(uint32_t ip, uint16_t port)
{
    struct sockaddr_in a = { AF_INET, htons(port), { htonl(ip) }, { 0 } };
    return a;
}
#define SA(a) ((struct sockaddr *)&(a))

static int recv_all(SOCKET s, uint8_t *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = recv(s, (char *)buf + got, len - got, 0);
        if (n <= 0)
            break;
        got += n;
    }
    return got;
}

static int send_all(SOCKET s, const uint8_t *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(s, (const char *)buf + sent, len - sent, 0);
        if (n <= 0)
            break;
        sent += n;
    }
    return sent;
}

#define BIG (64 * 1024)
static uint8_t out[BIG], back[BIG];

/* ---- a loopback echo server on a second thread ------------------------------------ */

static SOCKET listener;
static volatile int32_t served;

static uint32_t echo_server(void *arg)
{
    (void)arg;
    static uint8_t buf[2048];
    SOCKET c = accept(listener, NULL, NULL);
    if (c == INVALID_SOCKET)
        return 1;
    int n, total = 0;
    while ((n = recv(c, (char *)buf, sizeof buf, 0)) > 0) {
        if (send_all(c, buf, n) != n)
            break;
        total += n;
    }
    served = total;
    closesocket(c);
    return 0;
}

static void test_config(void)
{
    struct net_config c;
    CHECK(GetNetworkConfig(&c) && c.up && c.dhcp == 2 && c.ip == 0x0A00020F && c.gateway == 0x0A000202,
          "DHCP gave 10.0.2.15, gateway 10.0.2.2");
    struct in_addr a;
    char text[16];
    CHECK(inet_pton(AF_INET, "192.168.1.20", &a) == 1 && a.s_addr == htonl(0xC0A80114) &&
          !strcmp(inet_ntop(AF_INET, &a, text, sizeof text), "192.168.1.20") &&
          inet_addr("1.2.3.256") == INADDR_NONE, "inet_pton, inet_ntop, inet_addr");
    struct hostent *he = gethostbyname("localhost");
    CHECK(he && *(uint32_t *)he->h_addr == htonl(INADDR_LOOPBACK), "gethostbyname(localhost)");
    struct addrinfo *ai = NULL, hints = { 0, AF_INET, SOCK_DGRAM, 0, 0, NULL, NULL, NULL };
    CHECK(getaddrinfo("10.0.2.2", "53", &hints, &ai) == 0 && ai &&
          ((struct sockaddr_in *)ai->ai_addr)->sin_port == htons(53), "getaddrinfo");
    freeaddrinfo(ai);
}

static void test_loopback_udp(void)
{
    SOCKET a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in aa = addr(INADDR_LOOPBACK, 6000), ba = addr(INADDR_LOOPBACK, 6001), from;
    CHECK(bind(a, SA(aa), sizeof aa) == 0 && bind(b, SA(ba), sizeof ba) == 0, "bind two UDP sockets");
    struct sockaddr_in again = addr(INADDR_LOOPBACK, 6000);
    SOCKET c = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(bind(c, SA(again), sizeof again) < 0 && WSAGetLastError() == WSAEADDRINUSE, "a port in use");
    closesocket(c);
    CHECK(sendto(a, "hello", 5, 0, SA(ba), sizeof ba) == 5, "sendto");
    char buf[16] = { 0 };
    int flen = sizeof from;
    CHECK(recvfrom(b, buf, sizeof buf, 0, SA(from), &flen) == 5 && !memcmp(buf, "hello", 5) &&
          from.sin_port == htons(6000) && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
          "recvfrom: the datagram and who sent it");
    int ms = 300;
    setsockopt(b, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    uint32_t t0 = GetTickCount();
    int r = recv(b, buf, sizeof buf, 0);
    uint32_t took = GetTickCount() - t0;
    CHECK(r < 0 && WSAGetLastError() == WSAETIMEDOUT && took >= 250 && took < 1000, "SO_RCVTIMEO");
    struct sockaddr_in closed = addr(INADDR_LOOPBACK, 6009);
    connect(a, SA(closed), sizeof closed);
    send(a, "x", 1, 0);
    setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    CHECK(recv(a, buf, sizeof buf, 0) < 0 && WSAGetLastError() == WSAECONNREFUSED,
          "a connected UDP socket hears ICMP port unreachable");
    u_long on = 1;
    ioctlsocket(b, FIONBIO, &on);
    CHECK(recv(b, buf, sizeof buf, 0) < 0 && WSAGetLastError() == WSAEWOULDBLOCK, "FIONBIO: would block");
    closesocket(a);
    closesocket(b);
}

static void test_loopback_tcp(void)
{
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in la = addr(INADDR_LOOPBACK, 7000);
    CHECK(listener != INVALID_SOCKET && bind(listener, SA(la), sizeof la) == 0 && listen(listener, 2) == 0,
          "a TCP listener on 127.0.0.1:7000");
    HANDLE t = CreateThread(NULL, 0, echo_server, NULL, 0, NULL);
    SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(c, SA(la), sizeof la) == 0, "connect over loopback");
    struct sockaddr_in me, peer;
    int len = sizeof me, plen = sizeof peer;
    CHECK(getsockname(c, SA(me), &len) == 0 && getpeername(c, SA(peer), &plen) == 0 &&
          me.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(me.sin_port) >= 49152 &&
          peer.sin_port == htons(7000), "getsockname, getpeername");
    for (int i = 0; i < BIG; i++)
        out[i] = (uint8_t)(i * 7 + (i >> 8));
    int sent = 0, got = 0;
    while (got < BIG) {                         /* send and read in turn: the echo fills the buffers */
        if (sent < BIG) {
            int n = send(c, (const char *)out + sent, BIG - sent > 4000 ? 4000 : BIG - sent, 0);
            if (n <= 0)
                break;
            sent += n;
        }
        int n = recv(c, (char *)back + got, BIG - got, 0);
        if (n <= 0)
            break;
        got += n;
    }
    CHECK(got == BIG && !memcmp(out, back, BIG), "64 KB echoed back over loopback TCP");
    shutdown(c, SD_SEND);
    char tail;
    CHECK(recv(c, &tail, 1, 0) == 0, "shutdown: the server sees the end and closes; we see it too");
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    CHECK(served == BIG, "the server echoed every byte");
    closesocket(c);

    SOCKET r = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in nobody = addr(INADDR_LOOPBACK, 7001);
    CHECK(connect(r, SA(nobody), sizeof nobody) < 0 && WSAGetLastError() == WSAECONNREFUSED,
          "connect to a closed port: refused");
    closesocket(r);

    SOCKET nb = socket(AF_INET, SOCK_STREAM, 0);
    u_long on = 1;
    ioctlsocket(nb, FIONBIO, &on);
    int rc = connect(nb, SA(la), sizeof la);
    CHECK(rc < 0 && WSAGetLastError() == WSAEWOULDBLOCK, "a non-blocking connect");
    fd_set w;
    FD_ZERO(&w);
    FD_SET(nb, &w);
    struct timeval tv = { 2, 0 };
    CHECK(select(0, NULL, &w, NULL, &tv) == 1 && FD_ISSET(nb, &w), "select: it becomes writable");
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(listener, &rd);
    CHECK(select(0, &rd, NULL, NULL, &tv) == 1, "select: the listener has a connection to accept");
    SOCKET ac = accept(listener, NULL, NULL);
    send(nb, "ping", 4, 0);
    char b4[4];
    CHECK(ac != INVALID_SOCKET && recv_all(ac, (uint8_t *)b4, 4) == 4 && !memcmp(b4, "ping", 4),
          "accept, and data on the accepted socket");
    u_long waiting = 0;
    send(ac, "abc", 3, 0);
    Sleep(100);
    CHECK(ioctlsocket(nb, FIONREAD, &waiting) == 0 && waiting == 3, "FIONREAD");
    closesocket(ac);
    closesocket(nb);
    closesocket(listener);
}

static void test_host(uint16_t tcp_port, uint16_t udp_port)
{
    SOCKET u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = addr(0x0A000202, udp_port), from;
    int ms = 2000;
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    char buf[64] = { 0 };
    int flen = sizeof from;
    CHECK(sendto(u, "datagram from OS1993", 20, 0, SA(ua), sizeof ua) == 20 &&
          recvfrom(u, buf, sizeof buf, 0, SA(from), &flen) == 20 && !memcmp(buf, "datagram from OS1993", 20) &&
          from.sin_port == htons(udp_port), "UDP echo from the host, through the NE2000");
    closesocket(u);

    SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ta = addr(0x0A000202, tcp_port);
    CHECK(connect(c, SA(ta), sizeof ta) == 0, "TCP connect to the host");
    for (int i = 0; i < BIG; i++)
        out[i] = (uint8_t)(i ^ (i >> 9));
    int sent = 0, got = 0;
    uint32_t t0 = GetTickCount();
    while (got < BIG) {
        if (sent < BIG) {
            int n = send(c, (const char *)out + sent, BIG - sent > 6000 ? 6000 : BIG - sent, 0);
            if (n <= 0)
                break;
            sent += n;
        }
        int n = recv(c, (char *)back + got, BIG - got, 0);
        if (n <= 0)
            break;
        got += n;
    }
    uint32_t took = GetTickCount() - t0;
    CHECK(got == BIG && !memcmp(out, back, BIG), "64 KB echoed back by the host over TCP");
    printf("NET: 64 KB each way in %lu ms\n", (unsigned long)took);
    closesocket(c);
}

static int serve(void)
{
    SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in la = addr(INADDR_ANY, 7);
    if (bind(l, SA(la), sizeof la) < 0 || listen(l, 1) < 0) {
        printf("NET: can't listen on port 7 (error %d)\n", WSAGetLastError());
        return 1;
    }
    printf("NET: listening on port 7\n");
    struct sockaddr_in from;
    int flen = sizeof from;
    SOCKET c = accept(l, SA(from), &flen);
    char who[16];
    inet_ntop(AF_INET, &from.sin_addr, who, sizeof who);
    printf("NET: connection from %s\n", who);
    static uint8_t buf[4096];
    int n, total = 0;
    while ((n = recv(c, (char *)buf, sizeof buf, 0)) > 0) {
        send_all(c, buf, n);
        total += n;
    }
    printf("NET: served %d bytes\n", total);
    closesocket(c);
    closesocket(l);
    return 0;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (argc > 1 && !strcmp(argv[1], "serve"))
        return serve();
    if (argc < 4 || strcmp(argv[1], "all")) {
        printf("NET: usage: nettest all TCPPORT UDPPORT | nettest serve\n");
        return 2;
    }
    test_config();
    test_loopback_udp();
    test_loopback_tcp();
    test_host((uint16_t)atoi(argv[2]), (uint16_t)atoi(argv[3]));
    printf("NET: %d of %d checks passed\n", checks - failures, checks);
    return failures;
}
