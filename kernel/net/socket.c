/*
 * socket.c -- sockets: the table, waiting, UDP and raw (ICMP) sockets, and
 * SYS_SOCKET. TCP is in tcp.c.
 *
 * A socket belongs to a process and goes when it does. A thread waits on a
 * socket (or, polling, on any) until the stack wakes it, its timeout, or
 * its end. Datagram sockets queue what arrives, with the sender; raw ICMP
 * sockets get every ICMP packet, IP header and all, as on BSD (ping).
 */
#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "random.h"
#include "user.h"
#include "sock.h"

struct sock socks[SOCK_MAX];

/* ---- the table ------------------------------------------------------------- */

int sock_new(int kind, int pid)
{
    for (int i = 0; i < SOCK_MAX; i++)
        if (socks[i].kind == K_FREE) {
            memset(&socks[i], 0, sizeof socks[i]);
            socks[i].kind = kind;
            socks[i].pid = pid;
            return i;
        }
    return -1;
}

void sock_free(int s)
{
    struct sock *k = &socks[s];
    while (k->rq) {
        struct dgram *d = k->rq;
        k->rq = d->next;
        kfree(d);
    }
    if (k->tcb) {
        kfree(k->tcb->sbuf);
        kfree(k->tcb->rbuf);
        kfree(k->tcb);
    }
    memset(k, 0, sizeof *k);
    net_wake(s);
}

static uint16_t next_port;

uint16_t sock_ephemeral(int kind)
{
    if (!next_port)
        next_port = (uint16_t)(49152 + random_u32() % 16384);
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_port;
        next_port = next_port == 65535 ? 49152 : (uint16_t)(next_port + 1);
        bool used = false;
        for (int i = 0; i < SOCK_MAX && !used; i++)
            used = socks[i].kind == kind && socks[i].lport == p;
        if (!used)
            return p;
    }
    return 0;
}

/* ---- waiting --------------------------------------------------------------------- */

#define WAITERS 32
static struct waiter { struct thread *t; int s; uint32_t deadline; } waiters[WAITERS];

void net_wake(int s)
{
    for (int i = 0; i < WAITERS; i++)
        if (waiters[i].t && (waiters[i].s == s || waiters[i].s == -1 || s == -1))
            thread_wake(waiters[i].t);
}

/* Wait (interrupts off) until the stack wakes socket s, ms pass (0: no
 * limit), or the thread must end. */
int sock_block(int s, uint32_t ms)
{
    struct waiter *w = NULL;
    for (int i = 0; i < WAITERS && !w; i++)
        if (!waiters[i].t)
            w = &waiters[i];
    if (!w)
        return -ENOBUFS;
    if (ms > 0x7FFFFFF)
        ms = 0x7FFFFFF;
    w->t = current;
    w->s = s;
    w->deadline = ms ? (net_now() + (ms * 10 + 548) / 549) | 1 : 0;
    uint32_t deadline = w->deadline;
    current->state = T_BLOCKED;
    schedule();
    w->t = NULL;
    if (user_thread_stopping())
        return -EINTR;
    if (deadline && (int32_t)(net_now() - deadline) >= 0)
        return -ETIMEDOUT;
    return 0;
}

void sock_tick(void)
{
    uint32_t now = net_now();
    for (int i = 0; i < WAITERS; i++)
        if (waiters[i].t && waiters[i].deadline && (int32_t)(now - waiters[i].deadline) >= 0)
            thread_wake(waiters[i].t);
    tcp_tick();
}

/* ---- datagrams ---------------------------------------------------------------- */

static void queue(int s, uint32_t ip, uint16_t port, const uint8_t *data, uint32_t len)
{
    struct sock *k = &socks[s];
    if (k->rq_n >= DGRAM_QUEUE || k->shut_rd)
        return;
    struct dgram *d = kmalloc(sizeof *d + len);
    if (!d)
        return;
    d->next = NULL;
    d->ip = ip;
    d->port = port;
    d->len = (uint16_t)len;
    memcpy(d->data, data, len);
    if (k->rq_tail)
        k->rq_tail->next = d;
    else
        k->rq = d;
    k->rq_tail = d;
    k->rq_n++;
    net_wake(s);
}

void udp_input(const uint8_t *ip, uint32_t src, uint32_t dst, const uint8_t *udp, uint32_t len)
{
    uint16_t sport = get16(udp), dport = get16(udp + 2);
    bool broadcast = !net_is_local(dst);
    int best = -1;
    for (int i = 0; i < SOCK_MAX; i++) {
        struct sock *k = &socks[i];
        if (k->kind != K_UDP || k->lport != dport || k->shut_rd)
            continue;
        if (k->lip && k->lip != dst && !broadcast)
            continue;
        if (k->rip && (k->rip != src || k->rport != sport))
            continue;
        if (broadcast && !k->broadcast)
            continue;
        if (best < 0 || k->rip)                     /* a connected one first */
            best = i;
    }
    if (best >= 0)
        queue(best, src, sport, udp + 8, len - 8);
    else if (!broadcast)
        icmp_unreachable(ip, 3);                    /* port unreachable */
}

void raw_input(const uint8_t *ip, uint32_t len)
{
    uint32_t src = get32(ip + 12);
    for (int i = 0; i < SOCK_MAX; i++) {
        struct sock *k = &socks[i];
        if (k->kind == K_RAW && k->proto == ip[9] && (!k->rip || k->rip == src))
            queue(i, src, 0, ip, len);
    }
}

/* An ICMP error about a packet we sent (orig: its IP header and more). */
void sock_icmp_error(uint8_t type, uint8_t code, const uint8_t *orig, uint32_t len)
{
    uint32_t ihl = (orig[0] & 15u) * 4;
    if (len < ihl + 8)
        return;
    uint32_t src = get32(orig + 12), dst = get32(orig + 16);
    uint16_t sport = get16(orig + ihl), dport = get16(orig + ihl + 2);
    int err = type == 11 ? EHOSTUNREACH : code == 3 ? ECONNREFUSED : code == 0 ? ENETUNREACH : EHOSTUNREACH;
    if (orig[9] == PROTO_UDP) {
        for (int i = 0; i < SOCK_MAX; i++) {
            struct sock *k = &socks[i];
            if (k->kind == K_UDP && k->lport == sport && k->rip == dst && k->rport == dport) {
                k->err = err;                       /* only connected ones hear of it */
                net_wake(i);
            }
        }
    } else if (orig[9] == PROTO_TCP) {
        tcp_icmp_error(src, sport, dst, dport, err);
    }
}

static int dgram_send(int s, const uint8_t *buf, uint32_t len, uint32_t ip, uint16_t port)
{
    struct sock *k = &socks[s];
    static uint8_t pkt[ETH_MTU];
    if (k->kind == K_RAW) {
        if (len > ETH_MTU - IP_HLEN)
            return -EMSGSIZE;
        int rc = ip_send(ip, k->proto, buf, len, k->lip);
        return rc < 0 ? rc : (int)len;
    }
    if (len > ETH_MTU - IP_HLEN - 8)
        return -EMSGSIZE;
    if (!port)
        return -EADDRNOTAVAIL;
    if ((ip == 0xFFFFFFFFu || (net_cfg.ip && ip == (net_cfg.ip | ~net_cfg.mask))) && !k->broadcast)
        return -EACCES;                             /* SO_BROADCAST first */
    if (!k->lport && !(k->lport = sock_ephemeral(K_UDP)))
        return -EADDRINUSE;
    uint32_t src = k->lip ? k->lip : net_local_ip(ip);
    uint16_t n = (uint16_t)(8 + len);
    put16(pkt, k->lport);
    put16(pkt + 2, port);
    put16(pkt + 4, n);
    put16(pkt + 6, 0);
    memcpy(pkt + 8, buf, len);
    uint16_t sum = ip_checksum(pkt, n, ip_pseudo_sum(src, ip, PROTO_UDP, n));
    put16(pkt + 6, sum ? sum : 0xFFFF);
    int rc = ip_send(ip, PROTO_UDP, pkt, n, src);
    return rc < 0 ? rc : (int)len;
}

static int dgram_recv(int s, struct sock_args *a, bool peek)
{
    struct sock *k = &socks[s];
    for (;;) {
        if (k->err) {
            int e = k->err;
            k->err = 0;
            return -e;
        }
        if (k->rq) {
            struct dgram *d = k->rq;
            uint32_t n = d->len < a->len ? d->len : a->len;
            if (!user_writable((uint32_t)(uintptr_t)a->buf, n))
                return -EFAULT;
            memcpy(a->buf, d->data, n);
            a->ip = d->ip;
            a->port = d->port;
            if (!peek) {
                k->rq = d->next;
                if (!k->rq)
                    k->rq_tail = NULL;
                k->rq_n--;
                kfree(d);
            }
            return (int)n;                          /* the rest of a long one is lost */
        }
        if (k->shut_rd)
            return 0;
        if (k->nonblock)
            return -EWOULDBLOCK;
        int rc = sock_block(s, k->rcv_timeout);
        if (rc < 0)
            return rc;
    }
}

/* ---- options, names, polling --------------------------------------------------------- */

#define SOL_SOCKET      0xFFFF                      /* Winsock's numbers */
#define SO_REUSEADDR    0x0004
#define SO_KEEPALIVE    0x0008
#define SO_BROADCAST    0x0020
#define SO_LINGER       0x0080
#define SO_SNDBUF       0x1001
#define SO_RCVBUF       0x1002
#define SO_SNDTIMEO     0x1005
#define SO_RCVTIMEO     0x1006
#define SO_ERROR        0x1007
#define SO_TYPE         0x1008
#define FIONBIO_OPT     0x667E                      /* ioctlsocket(FIONBIO), as an option */
#define IPPROTO_TCP_L   6
#define TCP_NODELAY     0x0001

static int setopt(struct sock *k, int level, int name, int value)
{
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_REUSEADDR: k->reuse = value != 0; return 0;
        case SO_BROADCAST: k->broadcast = value != 0; return 0;
        case SO_RCVTIMEO:  k->rcv_timeout = (uint32_t)value; return 0;
        case SO_SNDTIMEO:  k->snd_timeout = (uint32_t)value; return 0;
        case SO_KEEPALIVE: case SO_LINGER: case SO_SNDBUF: case SO_RCVBUF: return 0;
        case FIONBIO_OPT:  k->nonblock = value != 0; return 0;
        }
    } else if (level == IPPROTO_TCP_L && name == TCP_NODELAY) {
        return 0;                                   /* segments go at once anyway */
    }
    return -ENOPROTOOPT;
}

static int getopt(struct sock *k, int level, int name)
{
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_REUSEADDR: return k->reuse;
        case SO_BROADCAST: return k->broadcast;
        case SO_RCVTIMEO:  return (int)k->rcv_timeout;
        case SO_SNDTIMEO:  return (int)k->snd_timeout;
        case SO_ERROR: { int e = k->err; k->err = 0; return e; }
        case SO_TYPE:      return k->kind == K_TCP ? NET_SOCK_STREAM : k->kind == K_UDP ? NET_SOCK_DGRAM : NET_SOCK_RAW;
        case SO_SNDBUF:    return TCP_SBUF;
        case SO_RCVBUF:    return TCP_RBUF;
        case SO_KEEPALIVE: case SO_LINGER: return 0;
        }
    } else if (level == IPPROTO_TCP_L && name == TCP_NODELAY) {
        return 1;
    }
    return -ENOPROTOOPT;
}

static int ready(int s)
{
    struct sock *k = &socks[s];
    if (k->kind == K_TCP)
        return tcp_poll(s);
    int r = NET_POLLOUT;
    if (k->rq || k->shut_rd)
        r |= NET_POLLIN;
    if (k->err)
        r |= NET_POLLERR;
    return r;
}

static int sock_poll(int pid, struct sock_args *a)
{
    int n = a->a;
    if (n <= 0 || n > SOCK_MAX || !user_writable((uint32_t)(uintptr_t)a->buf, (uint32_t)n * sizeof(struct sock_poll)))
        return -EINVAL;
    struct sock_poll *list = a->buf;
    uint32_t start = net_now(), ms = a->b < 0 ? 0 : (uint32_t)a->b;
    for (;;) {
        int count = 0;
        for (int i = 0; i < n; i++) {
            int s = list[i].s;
            list[i].revents = 0;
            if (s < 0 || s >= SOCK_MAX || socks[s].kind == K_FREE || socks[s].pid != pid) {
                list[i].revents = NET_POLLERR;
                count++;
                continue;
            }
            int r = ready(s) & (list[i].events | NET_POLLERR | NET_POLLHUP);
            list[i].revents = (int16_t)r;
            count += r != 0;
        }
        if (count || a->b == 0)
            return count;
        uint32_t left = 0;
        if (a->b > 0) {
            uint32_t gone = (net_now() - start) * 549 / 10;
            if (gone >= ms)
                return 0;
            left = ms - gone;
        }
        int rc = sock_block(-1, left);
        if (rc == -EINTR)
            return rc;
    }
}

/* ---- SYS_SOCKET ------------------------------------------------------------------------- */

static int bind_to(int s, uint32_t ip, uint16_t port)
{
    struct sock *k = &socks[s];
    if (k->lport)
        return -EINVAL;
    if (ip && !net_is_local(ip))
        return -EADDRNOTAVAIL;
    if (!port) {
        if (!(port = sock_ephemeral(k->kind)))
            return -EADDRINUSE;
    } else {
        for (int i = 0; i < SOCK_MAX; i++) {
            struct sock *o = &socks[i];
            if (i == s || o->kind != k->kind || o->lport != port || (o->lip && ip && o->lip != ip))
                continue;
            bool lingering = o->kind == K_TCP && o->tcb && (o->tcb->state == TIME_WAIT || !o->pid);
            if (!(k->reuse && lingering))
                return -EADDRINUSE;
        }
    }
    k->lip = ip;
    k->lport = port;
    return 0;
}

int sock_syscall(int pid, uint32_t op, struct sock_args *a)
{
    if (op == SOP_SOCKET) {
        int kind = a->a == NET_SOCK_STREAM ? K_TCP : a->a == NET_SOCK_DGRAM ? K_UDP : a->a == NET_SOCK_RAW ? K_RAW : 0;
        if (!kind)
            return -EPROTONOSUPPORT;
        if ((kind == K_TCP && a->b && a->b != PROTO_TCP) || (kind == K_UDP && a->b && a->b != PROTO_UDP) ||
            (kind == K_RAW && a->b != PROTO_ICMP))
            return -EPROTONOSUPPORT;
        int s = sock_new(kind, pid);
        if (s < 0)
            return -EMFILE;
        socks[s].proto = (uint8_t)a->b;
        return s;
    }
    if (op == SOP_CONFIG) {
        if (!user_writable((uint32_t)(uintptr_t)a->buf, sizeof net_cfg) || a->len < sizeof net_cfg)
            return -EFAULT;
        memcpy(a->buf, &net_cfg, sizeof net_cfg);
        return 0;
    }
    if (op == SOP_POLL)
        return sock_poll(pid, a);
    int s = a->s;
    if (s < 0 || s >= SOCK_MAX || socks[s].kind == K_FREE || socks[s].pid != pid)
        return -ENOTSOCK;
    struct sock *k = &socks[s];
    switch (op) {
    case SOP_BIND:
        return bind_to(s, a->ip, a->port);
    case SOP_CONNECT:
        if (!a->ip || (k->kind != K_RAW && !a->port))
            return -EADDRNOTAVAIL;
        if (k->kind == K_TCP) {
            if (k->tcb && k->tcb->state != CLOSED)
                return k->tcb->state == SYN_SENT ? -EALREADY : -EISCONN;
            if (!k->lport) {
                int rc = bind_to(s, 0, 0);
                if (rc < 0)
                    return rc;
            }
            k->rip = a->ip;
            k->rport = a->port;
            return tcp_connect(s);
        }
        k->rip = a->ip;
        k->rport = a->port;
        if (k->kind == K_UDP && !k->lport && !(k->lport = sock_ephemeral(K_UDP)))
            return -EADDRINUSE;
        return 0;
    case SOP_LISTEN:
        if (k->kind != K_TCP)
            return -EOPNOTSUPP;
        if (!k->lport)
            return -EINVAL;
        return tcp_listen(s, a->a);
    case SOP_ACCEPT: {
        if (k->kind != K_TCP)
            return -EOPNOTSUPP;
        int c = tcp_accept(s, pid);
        if (c >= 0) {
            a->ip = socks[c].rip;
            a->port = socks[c].rport;
        }
        return c;
    }
    case SOP_SEND:
        if (k->shut_wr)
            return -ESHUTDOWN;
        if (a->len && !user_range_ok((uint32_t)(uintptr_t)a->buf, a->len))
            return -EFAULT;
        if (k->kind == K_TCP)
            return tcp_send(s, a->buf, a->len);
        if (a->has_addr)
            return dgram_send(s, a->buf, a->len, a->ip, a->port);
        if (!k->rip)
            return -ENOTCONN;
        return dgram_send(s, a->buf, a->len, k->rip, k->rport);
    case SOP_RECV:
        if (a->len && !user_writable((uint32_t)(uintptr_t)a->buf, a->len))
            return -EFAULT;
        if (k->kind == K_TCP) {
            int n = tcp_recv(s, a->buf, a->len, (a->a & NET_MSG_PEEK) != 0);
            a->ip = k->rip;
            a->port = k->rport;
            return n;
        }
        return dgram_recv(s, a, (a->a & NET_MSG_PEEK) != 0);
    case SOP_SHUTDOWN:
        if (a->a < 0 || a->a > 2)
            return -EINVAL;
        if (k->kind == K_TCP)
            tcp_shutdown(s, a->a != 1, a->a != 0);
        if (a->a != 1)
            k->shut_rd = true;
        if (a->a != 0)
            k->shut_wr = true;
        net_wake(s);
        return 0;
    case SOP_CLOSE:
        if (k->kind == K_TCP)
            tcp_close(s);
        else
            sock_free(s);
        return 0;
    case SOP_SETOPT:
        return setopt(k, a->a, a->b, a->c);
    case SOP_GETOPT:
        return getopt(k, a->a, a->b);
    case SOP_NAME:
        if (a->a) {
            if (!k->rip)
                return -ENOTCONN;
            a->ip = k->rip;
            a->port = k->rport;
        } else {
            a->ip = k->lip ? k->lip : (k->rip ? net_local_ip(k->rip) : 0);
            a->port = k->lport;
        }
        return 0;
    }
    return -EINVAL;
}

void sock_process_end(int pid)
{
    for (int s = 0; s < SOCK_MAX; s++) {
        if (socks[s].kind == K_FREE || socks[s].pid != pid)
            continue;
        if (socks[s].kind == K_TCP)
            tcp_close(s);
        else
            sock_free(s);
    }
}
