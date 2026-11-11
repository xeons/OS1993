/*
 * wsock.c -- WSOCK.DLL: Winsock over the VMM's TCP/IP stack.
 *
 * Each call fills a struct sock_args and goes through KERNEL.DLL's
 * NetCall. Addresses become host byte order on the way in and network
 * order on the way out. gethostbyname asks the DNS server DHCP gave.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os1993.h"
#include "winsock.h"

/* ---- errors ------------------------------------------------------------------ */

static int wsa_of(int e)
{
    switch (e) {
    case EINTR: return WSAEINTR;            case EBADF: return WSAEBADF;
    case EACCES: return WSAEACCES;          case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;          case EMFILE: return WSAEMFILE;
    case EAGAIN: return WSAEWOULDBLOCK;     case EINPROGRESS: return WSAEINPROGRESS;
    case EALREADY: return WSAEALREADY;      case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return WSAEDESTADDRREQ; case EMSGSIZE: return WSAEMSGSIZE;
    case ENOPROTOOPT: return WSAENOPROTOOPT; case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;  case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;  case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;      case ENETUNREACH: return WSAENETUNREACH;
    case ECONNABORTED: return WSAECONNABORTED; case ECONNRESET: return WSAECONNRESET;
    case ENOBUFS: return WSAENOBUFS;        case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;      case 108: return WSAESHUTDOWN;
    case ETIMEDOUT: return WSAETIMEDOUT;    case ECONNREFUSED: return WSAECONNREFUSED;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    }
    return WSAEINVAL;
}

int  WSAGetLastError(void)      { return (int)GetLastError(); }
void WSASetLastError(int error) { SetLastError((uint32_t)error); }

static int fail(int neg_errno)
{
    errno = -neg_errno;
    WSASetLastError(wsa_of(-neg_errno));
    return SOCKET_ERROR;
}

static int call(uint32_t op, struct sock_args *a)
{
    int rc = NetCall(op, a);
    if (rc < 0)
        return fail(rc);
    WSASetLastError(0);
    return rc;
}

int WSAStartup(uint16_t version, WSADATA *data)
{
    if (data) {
        memset(data, 0, sizeof *data);
        data->wVersion = version;
        data->wHighVersion = MAKEWORD(2, 2);
        strlcpy(data->szDescription, "OS1993 TCP/IP (WSOCK.DLL)", sizeof data->szDescription);
        strlcpy(data->szSystemStatus, "Running", sizeof data->szSystemStatus);
        data->iMaxSockets = 32;
        data->iMaxUdpDg = 1472;
    }
    return 0;
}

int WSACleanup(void) { return 0; }

/* ---- byte order, addresses ------------------------------------------------------- */

u_short htons(u_short v) { return (u_short)(v << 8 | v >> 8); }
u_short ntohs(u_short v) { return htons(v); }
u_long  htonl(u_long v)  { return v << 24 | (v & 0xFF00) << 8 | (v >> 8 & 0xFF00) | v >> 24; }
u_long  ntohl(u_long v)  { return htonl(v); }

static bool parse_ipv4(const char *s, uint32_t *out)   /* host order */
{
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9')
            return false;
        uint32_t n = 0;
        for (int d = 0; *s >= '0' && *s <= '9'; d++, s++) {
            n = n * 10 + (uint32_t)(*s - '0');
            if (d > 2 || n > 255)
                return false;
        }
        v = v << 8 | n;
        if (part < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *out = v;
    return true;
}

uint32_t inet_addr(const char *dotted)
{
    uint32_t v;
    return dotted && parse_ipv4(dotted, &v) ? htonl(v) : INADDR_NONE;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    if (af != AF_INET) {
        fail(-EAFNOSUPPORT);
        return NULL;
    }
    const uint8_t *b = src;
    if (snprintf(dst, (size_t)size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]) >= size) {
        fail(-ENOBUFS);
        return NULL;
    }
    return dst;
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af != AF_INET) {
        fail(-EAFNOSUPPORT);
        return -1;
    }
    uint32_t v;
    if (!parse_ipv4(src, &v))
        return 0;
    v = htonl(v);
    memcpy(dst, &v, 4);
    return 1;
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[16];
    inet_ntop(AF_INET, &in.s_addr, buf, sizeof buf);
    return buf;
}

static int addr_in(const struct sockaddr *addr, int len, struct sock_args *a)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    if (!addr || len < (int)sizeof *in)
        return -EFAULT;
    if (in->sin_family != AF_INET)
        return -EAFNOSUPPORT;
    a->ip = ntohl(in->sin_addr.s_addr);
    a->port = ntohs(in->sin_port);
    return 0;
}

static void addr_out(const struct sock_args *a, struct sockaddr *addr, int *len)
{
    if (!addr || !len)
        return;
    struct sockaddr_in in = { AF_INET, htons(a->port), { htonl(a->ip) }, { 0 } };
    int n = *len < (int)sizeof in ? *len : (int)sizeof in;
    if (n > 0)
        memcpy(addr, &in, (size_t)n);
    *len = (int)sizeof in;
}

/* ---- sockets ------------------------------------------------------------------ */

SOCKET socket(int af, int type, int protocol)
{
    if (af != AF_INET) {
        fail(-EAFNOSUPPORT);
        return INVALID_SOCKET;
    }
    struct sock_args a = { .a = type, .b = protocol };
    int s = call(SOP_SOCKET, &a);
    return s < 0 ? INVALID_SOCKET : (SOCKET)s;
}

int bind(SOCKET s, const struct sockaddr *addr, int len)
{
    struct sock_args a = { .s = (int32_t)s };
    int rc = addr_in(addr, len, &a);
    return rc < 0 ? fail(rc) : call(SOP_BIND, &a) < 0 ? SOCKET_ERROR : 0;
}

int connect(SOCKET s, const struct sockaddr *addr, int len)
{
    struct sock_args a = { .s = (int32_t)s };
    int rc = addr_in(addr, len, &a);
    if (rc < 0)
        return fail(rc);
    rc = NetCall(SOP_CONNECT, &a);
    if (rc == -EINPROGRESS) {                   /* a non-blocking connect: Winsock says "would block" */
        errno = EINPROGRESS;
        WSASetLastError(WSAEWOULDBLOCK);
        return SOCKET_ERROR;
    }
    return rc < 0 ? fail(rc) : 0;
}

int listen(SOCKET s, int backlog)
{
    struct sock_args a = { .s = (int32_t)s, .a = backlog };
    return call(SOP_LISTEN, &a) < 0 ? SOCKET_ERROR : 0;
}

SOCKET accept(SOCKET s, struct sockaddr *addr, int *len)
{
    struct sock_args a = { .s = (int32_t)s };
    int c = call(SOP_ACCEPT, &a);
    if (c < 0)
        return INVALID_SOCKET;
    addr_out(&a, addr, len);
    return (SOCKET)c;
}

int send(SOCKET s, const char *buf, int len, int flags)
{
    if (len < 0)
        return fail(-EINVAL);
    struct sock_args a = { .s = (int32_t)s, .a = flags, .buf = (void *)buf, .len = (uint32_t)len };
    return call(SOP_SEND, &a);
}

int sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    if (len < 0)
        return fail(-EINVAL);
    struct sock_args a = { .s = (int32_t)s, .a = flags, .buf = (void *)buf, .len = (uint32_t)len };
    if (to) {
        int rc = addr_in(to, tolen, &a);
        if (rc < 0)
            return fail(rc);
        a.has_addr = 1;
    }
    return call(SOP_SEND, &a);
}

int recv(SOCKET s, char *buf, int len, int flags)
{
    if (len < 0)
        return fail(-EINVAL);
    struct sock_args a = { .s = (int32_t)s, .a = flags, .buf = buf, .len = (uint32_t)len };
    return call(SOP_RECV, &a);
}

int recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    if (len < 0)
        return fail(-EINVAL);
    struct sock_args a = { .s = (int32_t)s, .a = flags, .buf = buf, .len = (uint32_t)len };
    int n = call(SOP_RECV, &a);
    if (n >= 0)
        addr_out(&a, from, fromlen);
    return n;
}

int shutdown(SOCKET s, int how)
{
    struct sock_args a = { .s = (int32_t)s, .a = how };
    return call(SOP_SHUTDOWN, &a) < 0 ? SOCKET_ERROR : 0;
}

int closesocket(SOCKET s)
{
    struct sock_args a = { .s = (int32_t)s };
    return call(SOP_CLOSE, &a) < 0 ? SOCKET_ERROR : 0;
}

int setsockopt(SOCKET s, int level, int name, const char *value, int len)
{
    if (!value || len < (int)sizeof(int))
        return fail(-EFAULT);
    int v;
    memcpy(&v, value, sizeof v);
    struct sock_args a = { .s = (int32_t)s, .a = level, .b = name, .c = v };
    return call(SOP_SETOPT, &a) < 0 ? SOCKET_ERROR : 0;
}

int getsockopt(SOCKET s, int level, int name, char *value, int *len)
{
    if (!value || !len || *len < (int)sizeof(int))
        return fail(-EFAULT);
    struct sock_args a = { .s = (int32_t)s, .a = level, .b = name };
    int v = call(SOP_GETOPT, &a);
    if (v < 0)
        return SOCKET_ERROR;
    memcpy(value, &v, sizeof v);
    *len = sizeof v;
    return 0;
}

int ioctlsocket(SOCKET s, long cmd, u_long *arg)
{
    if (!arg)
        return fail(-EFAULT);
    if ((u_long)cmd == FIONBIO) {
        struct sock_args a = { .s = (int32_t)s, .a = SOL_SOCKET, .b = 0x667E, .c = *arg != 0 };
        return call(SOP_SETOPT, &a) < 0 ? SOCKET_ERROR : 0;
    }
    if ((u_long)cmd == FIONREAD) {              /* bytes waiting: peek at them */
        static char peek[8192];
        static CRITICAL_SECTION peek_lock;
        WSAPOLLFD p = { s, POLLIN, 0 };
        *arg = 0;
        if (WSAPoll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
            EnterCriticalSection(&peek_lock);
            struct sock_args a = { .s = (int32_t)s, .a = MSG_PEEK, .buf = peek, .len = sizeof peek };
            int n = call(SOP_RECV, &a);
            LeaveCriticalSection(&peek_lock);
            if (n < 0)
                return SOCKET_ERROR;
            *arg = (u_long)n;
        }
        return 0;
    }
    return fail(-EINVAL);
}

static int name_of(SOCKET s, int which, struct sockaddr *addr, int *len)
{
    struct sock_args a = { .s = (int32_t)s, .a = which };
    if (call(SOP_NAME, &a) < 0)
        return SOCKET_ERROR;
    addr_out(&a, addr, len);
    return 0;
}

int getsockname(SOCKET s, struct sockaddr *addr, int *len) { return name_of(s, 0, addr, len); }
int getpeername(SOCKET s, struct sockaddr *addr, int *len) { return name_of(s, 1, addr, len); }

/* ---- waiting on several ------------------------------------------------------------ */

int WSAPoll(WSAPOLLFD *fds, u_long n, int timeout_ms)
{
    struct sock_poll list[32];
    if (!fds || n > 32)
        return fail(-EINVAL);
    for (u_long i = 0; i < n; i++)
        list[i] = (struct sock_poll){ (int32_t)fds[i].fd, fds[i].events, 0 };
    struct sock_args a = { .a = (int32_t)n, .b = timeout_ms < 0 ? -1 : timeout_ms, .buf = list,
                           .len = (uint32_t)(n * sizeof list[0]) };
    int count = n ? call(SOP_POLL, &a) : 0;
    if (count < 0)
        return SOCKET_ERROR;
    for (u_long i = 0; i < n; i++)
        fds[i].revents = list[i].revents;
    return count;
}

int __WSAFDIsSet(SOCKET s, fd_set *set)
{
    for (u_int i = 0; set && i < set->fd_count; i++)
        if (set->fd_array[i] == s)
            return 1;
    return 0;
}

void __WSAFDClr(SOCKET s, fd_set *set)
{
    for (u_int i = 0; set && i < set->fd_count; i++)
        if (set->fd_array[i] == s) {
            set->fd_array[i] = set->fd_array[--set->fd_count];
            return;
        }
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timeval *timeout)
{
    (void)nfds;
    WSAPOLLFD p[32];
    u_long n = 0;
    fd_set *sets[3] = { readfds, writefds, exceptfds };
    static const short ev[3] = { POLLIN, POLLOUT, POLLERR };
    for (int k = 0; k < 3; k++)
        for (u_int i = 0; sets[k] && i < sets[k]->fd_count; i++) {
            u_long j = 0;
            while (j < n && p[j].fd != sets[k]->fd_array[i])
                j++;
            if (j == n) {
                if (n == 32)
                    return fail(-EINVAL);
                p[n++] = (WSAPOLLFD){ sets[k]->fd_array[i], 0, 0 };
            }
            p[j].events |= ev[k];
        }
    int ms = timeout ? (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000) : -1;
    if (!n) {                                   /* only a timeout: sleep */
        if (ms > 0)
            Sleep((uint32_t)ms);
        return 0;
    }
    if (WSAPoll(p, n, ms) < 0)
        return SOCKET_ERROR;
    int count = 0;
    for (int k = 0; k < 3; k++) {
        if (!sets[k])
            continue;
        fd_set keep = { 0, { 0 } };
        for (u_int i = 0; i < sets[k]->fd_count; i++)
            for (u_long j = 0; j < n; j++)
                if (p[j].fd == sets[k]->fd_array[i] &&
                    (p[j].revents & (k == 0 ? POLLIN | POLLHUP | POLLERR : k == 1 ? POLLOUT : POLLERR))) {
                    keep.fd_array[keep.fd_count++] = p[j].fd;
                    count++;
                }
        *sets[k] = keep;
    }
    return count;
}

/* ---- names ------------------------------------------------------------------------- */

bool GetNetworkConfig(struct net_config *cfg)
{
    struct sock_args a = { .buf = cfg, .len = sizeof *cfg };
    return call(SOP_CONFIG, &a) >= 0;
}

int gethostname(char *name, int len)
{
    if (!name || len < 7)
        return fail(-EFAULT);
    strlcpy(name, "os1993", (size_t)len);
    return 0;
}

/* One A-record query to the DNS server; -> the address (host order), or 0. */
static uint32_t dns_query(const char *name)
{
    struct net_config cfg;
    if (!GetNetworkConfig(&cfg) || !cfg.dns)
        return 0;
    uint8_t q[300], r[512];
    uint16_t id = (uint16_t)rand();
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01;                                /* recursion desired */
    q[5] = 1;                                   /* one question */
    size_t n = 12;
    for (const char *p = name; *p; ) {          /* www.example.com -> 3www7example3com0 */
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (!len || len > 63 || n + len + 6 > sizeof q)
            return 0;
        q[n++] = (uint8_t)len;
        memcpy(q + n, p, len);
        n += len;
        p += len + (dot ? 1 : 0);
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;                     /* type A */
    q[n++] = 0; q[n++] = 1;                     /* class IN */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return 0;
    int timeout = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    struct sockaddr_in to = { AF_INET, htons(53), { htonl(cfg.dns) }, { 0 } };
    uint32_t found = 0;
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        if (sendto(s, (const char *)q, (int)n, 0, (struct sockaddr *)&to, sizeof to) < 0)
            break;
        int got = recv(s, (char *)r, sizeof r, 0);
        if (got < 12 || r[0] != q[0] || r[1] != q[1] || !(r[2] & 0x80))
            continue;
        if ((r[3] & 0x0F) == 3)                 /* no such name */
            break;
        int answers = r[6] << 8 | r[7], at = (int)n;
        for (int i = 0; i < answers && at + 12 <= got; i++) {
            while (at < got && r[at] && !(r[at] & 0xC0))     /* the name */
                at += r[at] + 1;
            at += at < got && (r[at] & 0xC0) ? 2 : 1;
            if (at + 10 > got)
                break;
            int type = r[at] << 8 | r[at + 1], len = r[at + 8] << 8 | r[at + 9];
            at += 10;
            if (type == 1 && len == 4 && at + 4 <= got) {
                found = (uint32_t)r[at] << 24 | (uint32_t)r[at + 1] << 16 | (uint32_t)r[at + 2] << 8 | r[at + 3];
                break;
            }
            at += len;
        }
    }
    closesocket(s);
    return found;
}

static uint32_t resolve(const char *name)       /* host order, or 0 */
{
    uint32_t v;
    if (parse_ipv4(name, &v))
        return v;
    if (!strcasecmp(name, "localhost"))
        return INADDR_LOOPBACK;
    return dns_query(name);
}

struct hostent *gethostbyname(const char *name)
{
    static struct hostent he;
    static char hname[256], *aliases[1], *addrs[2];
    static uint32_t addr;
    if (!name) {
        fail(-EFAULT);
        return NULL;
    }
    uint32_t v = resolve(name);
    if (!v) {
        WSASetLastError(WSAHOST_NOT_FOUND);
        return NULL;
    }
    strlcpy(hname, name, sizeof hname);
    addr = htonl(v);
    addrs[0] = (char *)&addr;
    addrs[1] = NULL;
    aliases[0] = NULL;
    he = (struct hostent){ hname, aliases, AF_INET, 4, addrs };
    return &he;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    int socktype = hints ? hints->ai_socktype : 0, flags = hints ? hints->ai_flags : 0;
    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
        return WSAEAFNOSUPPORT;
    uint32_t ip = 0;
    if (node) {
        if ((flags & AI_NUMERICHOST) ? !parse_ipv4(node, &ip) : !(ip = resolve(node)))
            return WSAHOST_NOT_FOUND;
    } else {
        ip = (flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK;
    }
    uint16_t port = 0;
    if (service) {
        char *end;
        long p = strtol(service, &end, 10);
        if (*end || p < 0 || p > 65535)
            return WSATYPE_NOT_FOUND;
        port = (uint16_t)p;
    }
    struct { struct addrinfo ai; struct sockaddr_in sin; } *r = calloc(1, sizeof *r);
    if (!r)
        return WSAENOBUFS;
    r->sin = (struct sockaddr_in){ AF_INET, htons(port), { htonl(ip) }, { 0 } };
    r->ai.ai_family = AF_INET;
    r->ai.ai_socktype = socktype ? socktype : SOCK_STREAM;
    r->ai.ai_protocol = r->ai.ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    r->ai.ai_addrlen = sizeof r->sin;
    r->ai.ai_addr = (struct sockaddr *)&r->sin;
    *res = &r->ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *ai) { free(ai); }
