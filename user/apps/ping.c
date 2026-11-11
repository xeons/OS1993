/*
 * ping.c -- PING.BIN: ICMP echo, as Windows' PING does it.
 *
 *     ping [-n count] [-l size] [-w timeout] [-t] host
 *
 * A raw ICMP socket sends echo requests (one a second) and reads the
 * replies, IP header and all. Times are to the timer's 55 ms.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os1993.h"
#include "winsock.h"

static uint16_t checksum(const uint8_t *p, int len)
{
    uint32_t sum = 0;
    for (; len > 1; len -= 2, p += 2)
        sum += (uint32_t)(p[0] << 8 | p[1]);
    if (len)
        sum += (uint32_t)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void usage(void)
{
    printf("Usage: ping [-t] [-n count] [-l size] [-w timeout] host\n\n");
    printf("Options:\n");
    printf("    -t          Ping the host until the window is closed.\n");
    printf("    -n count    Number of echo requests to send (4).\n");
    printf("    -l size     Send buffer size (32, at most 1472).\n");
    printf("    -w timeout  Timeout in milliseconds to wait for each reply (1000).\n");
}

static int ping_main(int argc, char **argv)
{
    int count = 4, size = 32, timeout = 1000;
    bool forever = false;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t"))
            forever = true;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc)
            size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc)
            timeout = atoi(argv[++i]);
        else if (argv[i][0] != '-' && !host)
            host = argv[i];
        else {
            usage();
            return 1;
        }
    }
    if (!host || count < 1 || size < 0 || size > 1472 || timeout < 1) {
        usage();
        return 1;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    struct hostent *he = gethostbyname(host);
    if (!he) {
        printf("Ping request could not find host %s. Please check the name and try again.\n", host);
        return 1;
    }
    struct in_addr to_addr;
    memcpy(&to_addr.s_addr, he->h_addr, 4);
    char ip[16];
    inet_ntop(AF_INET, &to_addr.s_addr, ip, sizeof ip);
    SOCKET s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s == INVALID_SOCKET) {
        printf("PING: can't open an ICMP socket (error %d).\n", WSAGetLastError());
        return 1;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    if (strcmp(ip, host))
        printf("\nPinging %s [%s] with %d bytes of data:\n\n", host, ip, size);
    else
        printf("\nPinging %s with %d bytes of data:\n\n", ip, size);

    static uint8_t req[8 + 1472], reply[1600];
    uint16_t id = (uint16_t)GetCurrentProcessId() ^ 0x5A00;
    int sent = 0, received = 0;
    uint32_t tmin = 0xFFFFFFFFu, tmax = 0, tsum = 0;
    struct sockaddr_in to = { AF_INET, 0, to_addr, { 0 } };
    for (int n = 0; forever || n < count; n++) {
        uint16_t seq = (uint16_t)(n + 1);
        memset(req, 0, 8);
        req[0] = 8;                                     /* echo request */
        req[4] = (uint8_t)(id >> 8);
        req[5] = (uint8_t)id;
        req[6] = (uint8_t)(seq >> 8);
        req[7] = (uint8_t)seq;
        for (int i = 0; i < size; i++)
            req[8 + i] = (uint8_t)('a' + i % 23);       /* abcdefghijklmnopqrstuvw... */
        uint16_t sum = checksum(req, 8 + size);
        req[2] = (uint8_t)(sum >> 8);
        req[3] = (uint8_t)sum;
        uint32_t start = GetTickCount();
        if (sendto(s, (const char *)req, 8 + size, 0, (struct sockaddr *)&to, sizeof to) < 0) {
            printf("PING: transmit failed. General failure (error %d).\n", WSAGetLastError());
            sent++;
            Sleep(1000);
            continue;
        }
        sent++;
        bool answered = false;
        for (;;) {
            struct sockaddr_in from;
            int flen = sizeof from;
            int got = recvfrom(s, (char *)reply, sizeof reply, 0, (struct sockaddr *)&from, &flen);
            uint32_t elapsed = GetTickCount() - start;
            if (got < 0) {
                if (WSAGetLastError() != WSAETIMEDOUT)
                    printf("PING: receive failed (error %d).\n", WSAGetLastError());
                break;
            }
            int ihl = (reply[0] & 15) * 4;
            if (got < ihl + 8)
                continue;
            const uint8_t *m = reply + ihl;
            char who[16];
            inet_ntop(AF_INET, &from.sin_addr.s_addr, who, sizeof who);
            if (m[0] == 0 && (m[4] << 8 | m[5]) == id && (m[6] << 8 | m[7]) == seq) {
                int bytes = got - ihl - 8;
                if (elapsed)
                    printf("Reply from %s: bytes=%d time=%lums TTL=%d\n", who, bytes, (unsigned long)elapsed, reply[8]);
                else
                    printf("Reply from %s: bytes=%d time<55ms TTL=%d\n", who, bytes, reply[8]);
                received++;
                tsum += elapsed;
                if (elapsed < tmin) tmin = elapsed;
                if (elapsed > tmax) tmax = elapsed;
                answered = true;
                break;
            }
            if ((m[0] == 3 || m[0] == 11) && got >= ihl + 8 + 28) {     /* about our request? */
                const uint8_t *orig = m + 8, *oicmp = orig + (orig[0] & 15) * 4;
                if (orig[9] == IPPROTO_ICMP && (oicmp[4] << 8 | oicmp[5]) == id) {
                    printf("Reply from %s: %s.\n", who, m[0] == 11 ? "TTL expired in transit" :
                           m[1] == 1 ? "Destination host unreachable" : "Destination net unreachable");
                    answered = true;
                    break;
                }
            }
            if (elapsed >= (uint32_t)timeout)
                break;
        }
        if (!answered)
            printf("Request timed out.\n");
        uint32_t took = GetTickCount() - start;
        if ((forever || n + 1 < count) && took < 1000)
            Sleep(1000 - took);
    }
    int lost = sent - received;
    printf("\nPing statistics for %s:\n", ip);
    printf("    Packets: Sent = %d, Received = %d, Lost = %d (%d%% loss),\n", sent, received, lost,
           sent ? lost * 100 / sent : 0);
    if (received) {
        printf("Approximate round trip times in milli-seconds:\n");
        printf("    Minimum = %lums, Maximum = %lums, Average = %lums\n", (unsigned long)tmin,
               (unsigned long)tmax, (unsigned long)(tsum / (uint32_t)received));
    }
    closesocket(s);
    return received ? 0 : 1;
}

int main(int argc, char **argv)
{
    return RunConsoleProgram("Ping", ping_main, argc, argv);
}
