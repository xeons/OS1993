/*
 * tcp.c -- TCP (RFC 793, 1122, 6298, 5681), for the sockets in socket.c.
 *
 * Each connection has an 8 KB send buffer (sent but unacknowledged bytes,
 * then unsent ones) and an 8 KB receive buffer, whose free space is the
 * window we advertise. Segments go at once (no Nagle, no delayed ACKs);
 * out-of-order segments are dropped and the peer resends. Retransmission
 * goes back to the first unacknowledged byte after a timeout from
 * Jacobson's estimate (Karn's rule, doubling up to a minute, 8 tries);
 * the congestion window starts at two segments and grows by slow start,
 * then by one segment per window. A zero window is probed. A closed
 * socket whose connection is still open finishes on its own: its FIN goes
 * after the data, and it is freed when the connection ends.
 */
#include "kernel.h"
#include "mm.h"
#include "random.h"
#include "user.h"
#include "sock.h"

#define TCP_HLEN    20
#define F_FIN       0x01
#define F_SYN       0x02
#define F_RST       0x04
#define F_PSH       0x08
#define F_ACK       0x10

#define TICKS(ms)   (((ms) * 10u + 548) / 549)
#define RTO_INIT    TICKS(1000)
#define RTO_MIN     TICKS(300)
#define RTO_MAX     TICKS(60000)
#define MAX_RETRIES 8
#define SYN_RETRIES 5
#define TIME_WAIT_T TICKS(2000)                 /* 2 MSL, short: a LAN */
#define FIN_WAIT_T  TICKS(60000)                /* an orphan's FIN_WAIT_2 */
#define MSS_LOCAL   (ETH_MTU - IP_HLEN - TCP_HLEN)

static uint32_t window_of(const struct tcb *t) { return TCP_RBUF - t->r_len; }

/* ---- sending segments ----------------------------------------------------------- */

static void send_segment(int s, uint32_t seq, uint8_t flags, uint32_t data_off, uint32_t len, bool mss)
{
    struct sock *k = &socks[s];
    struct tcb *t = k->tcb;
    static uint8_t seg[ETH_MTU];
    uint32_t hl = TCP_HLEN + (mss ? 4 : 0);
    put16(seg, k->lport);
    put16(seg + 2, k->rport);
    put32(seg + 4, seq);
    put32(seg + 8, (flags & F_ACK) ? t->rcv_nxt : 0);
    seg[12] = (uint8_t)((hl / 4) << 4);
    seg[13] = flags;
    uint32_t win = window_of(t);
    put16(seg + 14, (uint16_t)(win > 65535 ? 65535 : win));
    put32(seg + 16, 0);
    if (mss) {
        seg[20] = 2;
        seg[21] = 4;
        put16(seg + 22, MSS_LOCAL);
    }
    for (uint32_t i = 0; i < len; i++)          /* from the send buffer's ring */
        seg[hl + i] = t->sbuf[(t->s_head + data_off + i) % TCP_SBUF];
    uint32_t src = k->lip ? k->lip : net_local_ip(k->rip);
    put16(seg + 16, ip_checksum(seg, hl + len, ip_pseudo_sum(src, k->rip, PROTO_TCP, (uint16_t)(hl + len))));
    ip_send(k->rip, PROTO_TCP, seg, hl + len, src);
    if (flags & F_ACK)
        t->rcv_adv = t->rcv_nxt + win;
}

static void send_ack(int s) { send_segment(s, socks[s].tcb->snd_nxt, F_ACK, 0, 0, false); }

/* A reset for a segment that belongs to nothing. */
static void reply_rst(uint32_t src, uint32_t dst, const uint8_t *seg, uint32_t len)
{
    uint8_t h[TCP_HLEN];
    uint8_t flags = seg[13];
    uint32_t off = (seg[12] >> 4) * 4u, seglen = len - off + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0);
    memset(h, 0, sizeof h);
    put16(h, get16(seg + 2));
    put16(h + 2, get16(seg));
    if (flags & F_ACK) {
        put32(h + 4, get32(seg + 8));
        h[13] = F_RST;
    } else {
        put32(h + 8, get32(seg + 4) + seglen);
        h[13] = F_RST | F_ACK;
    }
    h[12] = 5 << 4;
    put16(h + 16, ip_checksum(h, TCP_HLEN, ip_pseudo_sum(dst, src, PROTO_TCP, TCP_HLEN)));
    ip_send(src, PROTO_TCP, h, TCP_HLEN, dst);
}

/* ---- connection state -------------------------------------------------------------- */

static struct tcb *tcb_new(int s)
{
    struct tcb *t = kzalloc(sizeof *t);
    if (!t)
        return NULL;
    t->sbuf = kmalloc(TCP_SBUF);
    t->rbuf = kmalloc(TCP_RBUF);
    if (!t->sbuf || !t->rbuf) {
        kfree(t->sbuf);
        kfree(t->rbuf);
        kfree(t);
        return NULL;
    }
    t->mss = 536;
    t->rto = RTO_INIT;
    t->ssthresh = 65535;
    t->parent = -1;
    socks[s].tcb = t;
    return t;
}

static void start_seq(struct tcb *t)
{
    t->iss = random_u32();
    t->snd_una = t->iss;
    t->snd_nxt = t->iss + 1;                    /* after the SYN */
    t->snd_max = t->iss + 1;
    t->snd_data = t->iss + 1;
}

/* The connection is over: its owner hears err, or (closed already) it goes. */
static void conn_closed(int s, int err)
{
    struct sock *k = &socks[s];
    struct tcb *t = k->tcb;
    t->state = CLOSED;
    t->rexmt_at = t->timewait_at = t->persist_at = 0;
    if (err)
        k->err = err;
    if (!k->pid && t->parent < 0) {             /* nobody holds it */
        sock_free(s);
        return;
    }
    if (!k->pid && t->parent >= 0) {            /* a listener's: unless queued for accept */
        struct tcb *lt = socks[t->parent].tcb;
        bool queued = false;
        for (int i = 0; lt && i < lt->aq_n; i++)
            queued |= lt->aq[i] == s;
        if (!queued) {
            sock_free(s);
            return;
        }
    }
    net_wake(s);
}

static void abort_conn(int s, int err)          /* RST, and done */
{
    struct tcb *t = socks[s].tcb;
    if (t->state != CLOSED && t->state != LISTEN && t->state != SYN_SENT && t->state != TIME_WAIT)
        send_segment(s, t->snd_nxt, F_RST | F_ACK, 0, 0, false);
    conn_closed(s, err);
}

/* Send what the windows allow, then the FIN if one is due. */
static void tcp_output(int s)
{
    struct sock *k = &socks[s];
    struct tcb *t = k->tcb;
    enum tcp_state st = t->state;
    if (st != ESTABLISHED && st != CLOSE_WAIT && st != FIN_WAIT_1 && st != CLOSING && st != LAST_ACK)
        return;
    uint32_t now = net_now();
    for (;;) {
        if (t->fin_sent)
            return;
        uint32_t sent = t->snd_nxt - t->snd_data;
        if (sent > t->s_len)
            sent = t->s_len;
        uint32_t flight = t->snd_nxt - t->snd_una, unsent = t->s_len - sent;
        if (unsent) {
            uint32_t win = t->snd_wnd < t->cwnd ? t->snd_wnd : t->cwnd;
            uint32_t room = win > flight ? win - flight : 0;
            if (!room && !flight && !t->snd_wnd) {  /* a zero window: probe it now and then */
                if (!t->persist_at)
                    t->persist_at = now + t->rto;
                if ((int32_t)(now - t->persist_at) < 0)
                    return;
                t->persist_at = now + (t->rto * 2 < RTO_MAX ? t->rto * 2 : RTO_MAX);
                room = 1;
            }
            if (!room)
                return;
            uint32_t n = unsent < room ? unsent : room;
            if (n > t->mss)
                n = t->mss;
            send_segment(s, t->snd_nxt, F_ACK | (n == unsent ? F_PSH : 0), sent, n, false);
            if (!t->rtt_on) {
                t->rtt_on = true;
                t->rtt_seq = t->snd_nxt;
                t->rtt_start = now;
            }
            t->snd_nxt += n;
            if (SEQ_GT(t->snd_nxt, t->snd_max))
                t->snd_max = t->snd_nxt;
            if (!t->rexmt_at)
                t->rexmt_at = now + t->rto;
            continue;
        }
        if (t->fin_queued) {
            send_segment(s, t->snd_nxt, F_FIN | F_ACK, 0, 0, false);
            t->snd_nxt++;
            if (SEQ_GT(t->snd_nxt, t->snd_max))
                t->snd_max = t->snd_nxt;
            t->fin_sent = t->fin_ever = true;
            if (t->state == ESTABLISHED)
                t->state = FIN_WAIT_1;
            else if (t->state == CLOSE_WAIT)
                t->state = LAST_ACK;
            if (!t->rexmt_at)
                t->rexmt_at = now + t->rto;
        }
        return;
    }
}

/* The peer acknowledged up to ack. */
static void ack_advance(int s, uint32_t ack)
{
    struct tcb *t = socks[s].tcb;
    uint32_t now = net_now();
    if (SEQ_GT(ack, t->snd_data)) {             /* free what it has */
        uint32_t n = ack - t->snd_data;
        if (n > t->s_len)
            n = t->s_len;                       /* the rest: our FIN */
        t->s_head = (t->s_head + n) % TCP_SBUF;
        t->s_len -= n;
        t->snd_data += n;
    }
    if (t->rtt_on && SEQ_GT(ack, t->rtt_seq)) { /* a round trip measured */
        int32_t m = (int32_t)(now - t->rtt_start);
        if (m < 1)
            m = 1;
        if (!t->srtt8) {
            t->srtt8 = (uint32_t)m * 8;
            t->rttvar4 = (uint32_t)m * 2;
        } else {
            int32_t err = m - (int32_t)(t->srtt8 / 8);
            t->srtt8 = (uint32_t)((int32_t)t->srtt8 + err);
            if (err < 0)
                err = -err;
            t->rttvar4 = (uint32_t)((int32_t)t->rttvar4 + err - (int32_t)(t->rttvar4 / 4));
        }
        t->rto = t->srtt8 / 8 + t->rttvar4;
        if (t->rto < RTO_MIN) t->rto = RTO_MIN;
        if (t->rto > RTO_MAX) t->rto = RTO_MAX;
        t->rtt_on = false;
    }
    uint32_t acked = ack - t->snd_una;
    t->snd_una = ack;
    if (SEQ_LT(t->snd_nxt, ack))                /* after going back: it had them already */
        t->snd_nxt = ack;
    if (t->fin_ever && ack == t->snd_max)
        t->fin_sent = true;
    t->retries = 0;
    if (t->cwnd < t->ssthresh)                  /* slow start */
        t->cwnd += acked < t->mss ? acked : t->mss;
    else                                        /* congestion avoidance */
        t->cwnd += t->mss * t->mss / (t->cwnd ? t->cwnd : 1) + 1;
    if (t->cwnd > TCP_SBUF * 2)
        t->cwnd = TCP_SBUF * 2;
    t->rexmt_at = t->snd_una == t->snd_max ? 0 : now + t->rto;
    net_wake(s);                                /* room in the send buffer */
}

static uint16_t parse_mss(const uint8_t *seg, uint32_t off)
{
    for (uint32_t i = TCP_HLEN; i < off; ) {
        uint8_t kind = seg[i];
        if (kind == 0)
            break;
        if (kind == 1) { i++; continue; }
        if (i + 1 >= off || seg[i + 1] < 2)
            break;
        if (kind == 2 && seg[i + 1] == 4 && i + 4 <= off) {
            uint16_t m = get16(seg + i + 2);
            return m < 64 ? 64 : m > MSS_LOCAL ? MSS_LOCAL : m;
        }
        i += seg[i + 1];
    }
    return 536;
}

/* ---- arriving segments ----------------------------------------------------------- */

static int find_conn(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport)
{
    for (int i = 0; i < SOCK_MAX; i++) {
        struct sock *k = &socks[i];
        if (k->kind == K_TCP && k->tcb && k->tcb->state != LISTEN && k->tcb->state != CLOSED &&
            k->lport == lport && k->rport == rport && k->rip == rip && (!k->lip || k->lip == lip))
            return i;
    }
    return -1;
}

static void listen_input(int ls, uint32_t src, uint32_t dst, const uint8_t *seg, uint32_t off)
{
    uint8_t flags = seg[13];
    struct tcb *lt = socks[ls].tcb;
    if (flags & F_RST)
        return;
    if (flags & F_ACK) {
        reply_rst(src, dst, seg, off);
        return;
    }
    if (!(flags & F_SYN))
        return;
    int pending = 0;
    for (int i = 0; i < SOCK_MAX; i++)
        if (socks[i].kind == K_TCP && socks[i].tcb && socks[i].tcb->parent == ls && !socks[i].pid)
            pending++;
    if (pending >= lt->backlog)
        return;                                 /* full: the peer will try again */
    int c = sock_new(K_TCP, 0);
    if (c < 0)
        return;
    struct tcb *t = tcb_new(c);
    if (!t) {
        sock_free(c);
        return;
    }
    struct sock *k = &socks[c];
    k->lip = dst;
    k->lport = get16(seg + 2);
    k->rip = src;
    k->rport = get16(seg);
    k->rcv_timeout = socks[ls].rcv_timeout;
    k->snd_timeout = socks[ls].snd_timeout;
    t->parent = ls;
    t->irs = get32(seg + 4);
    t->rcv_nxt = t->irs + 1;
    start_seq(t);
    t->snd_wnd = get16(seg + 14);
    t->snd_wl1 = t->irs;
    t->mss = parse_mss(seg, off);
    t->state = SYN_RCVD;
    send_segment(c, t->iss, F_SYN | F_ACK, 0, 0, true);
    t->rexmt_at = net_now() + t->rto;
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint32_t len)
{
    uint32_t off = (seg[12] >> 4) * 4u;
    if (off < TCP_HLEN || off > len)
        return;
    uint16_t sport = get16(seg), dport = get16(seg + 2);
    uint32_t seq = get32(seg + 4), ack = get32(seg + 8), win = get16(seg + 14);
    uint8_t flags = seg[13];
    const uint8_t *data = seg + off;
    uint32_t dlen = len - off, seglen = dlen + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0);

    int s = find_conn(dst, dport, src, sport);
    if (s < 0) {
        for (int i = 0; i < SOCK_MAX && s < 0; i++)
            if (socks[i].kind == K_TCP && socks[i].tcb && socks[i].tcb->state == LISTEN &&
                socks[i].lport == dport && (!socks[i].lip || socks[i].lip == dst))
                s = i;
        if (s < 0) {
            if (!(flags & F_RST))
                reply_rst(src, dst, seg, len);
            return;
        }
        listen_input(s, src, dst, seg, off);
        return;
    }
    struct sock *k = &socks[s];
    struct tcb *t = k->tcb;
    uint32_t now = net_now();

    if (t->state == SYN_SENT) {
        bool ack_ok = (flags & F_ACK) && SEQ_GT(ack, t->iss) && SEQ_LE(ack, t->snd_nxt);
        if ((flags & F_ACK) && !ack_ok) {
            if (!(flags & F_RST))
                reply_rst(src, dst, seg, len);
            return;
        }
        if (flags & F_RST) {
            if (ack_ok)
                conn_closed(s, ECONNREFUSED);
            return;
        }
        if (!(flags & F_SYN))
            return;
        t->irs = seq;
        t->rcv_nxt = seq + 1;
        t->mss = parse_mss(seg, off);
        t->snd_wnd = win;
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
        if (ack_ok) {
            t->snd_una = ack;
            t->state = ESTABLISHED;
            t->rexmt_at = 0;
            t->retries = 0;
            t->cwnd = 2u * t->mss;
            send_ack(s);
            net_wake(s);
        } else {                                /* both opened at once */
            t->state = SYN_RCVD;
            send_segment(s, t->iss, F_SYN | F_ACK, 0, 0, true);
        }
        return;
    }

    /* 1. Is it in the window? */
    uint32_t rwin = window_of(t);
    bool ok;
    if (!seglen)
        ok = rwin ? (SEQ_LE(t->rcv_nxt, seq) && SEQ_LT(seq, t->rcv_nxt + rwin)) : seq == t->rcv_nxt;
    else
        ok = rwin && ((SEQ_LE(t->rcv_nxt, seq) && SEQ_LT(seq, t->rcv_nxt + rwin)) ||
                      (SEQ_LE(t->rcv_nxt, seq + seglen - 1) && SEQ_LT(seq + seglen - 1, t->rcv_nxt + rwin)));
    if (!ok && seq == t->rcv_nxt && !rwin && !seglen)
        ok = true;
    if (!ok) {
        if (!(flags & F_RST))
            send_ack(s);
        return;
    }
    /* 2. A reset */
    if (flags & F_RST) {
        bool done = t->state == TIME_WAIT || t->state == LAST_ACK || t->state == CLOSING;
        conn_closed(s, done || (t->state == SYN_RCVD && t->parent >= 0) ? 0 : ECONNRESET);
        return;
    }
    /* 3. A SYN in the window: the peer has lost the connection */
    if (flags & F_SYN) {
        abort_conn(s, ECONNRESET);
        return;
    }
    /* 4. The acknowledgement */
    if (!(flags & F_ACK))
        return;
    if (t->state == SYN_RCVD) {
        if (!(SEQ_GT(ack, t->snd_una) && SEQ_LE(ack, t->snd_nxt))) {
            reply_rst(src, dst, seg, len);
            return;
        }
        t->state = ESTABLISHED;
        t->cwnd = 2u * t->mss;
        t->snd_wnd = win;
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
        if (t->parent >= 0 && socks[t->parent].tcb && socks[t->parent].tcb->state == LISTEN) {
            struct tcb *lt = socks[t->parent].tcb;
            if (lt->aq_n < TCP_BACKLOG) {
                lt->aq[lt->aq_n++] = s;
                net_wake(t->parent);
            }
        }
    }
    if (SEQ_GT(ack, t->snd_max)) {              /* for something we haven't sent */
        send_ack(s);
        return;
    }
    if (SEQ_GT(ack, t->snd_una))
        ack_advance(s, ack);
    if (SEQ_LT(t->snd_wl1, seq) || (t->snd_wl1 == seq && SEQ_LE(t->snd_wl2, ack))) {
        t->snd_wnd = win;
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
        if (win)
            t->persist_at = 0;
    }
    bool fin_acked = t->fin_ever && t->snd_una == t->snd_max && !t->s_len;
    switch (t->state) {
    case FIN_WAIT_1:
        if (fin_acked) {
            t->state = FIN_WAIT_2;
            if (!k->pid)
                t->timewait_at = now + FIN_WAIT_T;
        }
        break;
    case CLOSING:
        if (fin_acked) {
            t->state = TIME_WAIT;
            t->timewait_at = now + TIME_WAIT_T;
        }
        break;
    case LAST_ACK:
        if (fin_acked) {
            conn_closed(s, 0);
            return;
        }
        break;
    case TIME_WAIT:                             /* its FIN again: ACK it, wait longer */
        send_ack(s);
        t->timewait_at = now + TIME_WAIT_T;
        return;
    default:
        break;
    }
    /* 5. The data */
    bool need_ack = false;
    if (dlen && (t->state == ESTABLISHED || t->state == FIN_WAIT_1 || t->state == FIN_WAIT_2)) {
        uint32_t skip = SEQ_LT(seq, t->rcv_nxt) ? t->rcv_nxt - seq : 0;
        if (skip < dlen && seq + skip == t->rcv_nxt && !k->shut_rd) {
            uint32_t n = dlen - skip, room = TCP_RBUF - t->r_len;
            if (n > room)
                n = room;
            for (uint32_t i = 0; i < n; i++)
                t->rbuf[(t->r_head + t->r_len + i) % TCP_RBUF] = data[skip + i];
            t->r_len += n;
            t->rcv_nxt += n;
            if (n)
                net_wake(s);
        } else if (k->shut_rd && skip < dlen) {
            t->rcv_nxt = seq + dlen;            /* nobody will read it */
        }
        need_ack = true;
    }
    /* 6. The FIN, once everything before it is here */
    if ((flags & F_FIN) && seq + dlen == t->rcv_nxt && !t->fin_rcvd) {
        t->rcv_nxt++;
        t->fin_rcvd = true;
        need_ack = true;
        net_wake(s);
        if (t->state == ESTABLISHED || t->state == SYN_RCVD) {
            t->state = CLOSE_WAIT;
        } else if (t->state == FIN_WAIT_1) {
            t->state = fin_acked ? TIME_WAIT : CLOSING;
            if (fin_acked)
                t->timewait_at = now + TIME_WAIT_T;
        } else if (t->state == FIN_WAIT_2) {
            t->state = TIME_WAIT;
            t->timewait_at = now + TIME_WAIT_T;
        }
    }
    if (need_ack)
        send_ack(s);
    tcp_output(s);
}

/* ---- timers ------------------------------------------------------------------------ */

void tcp_tick(void)
{
    uint32_t now = net_now();
    for (int s = 0; s < SOCK_MAX; s++) {
        struct sock *k = &socks[s];
        struct tcb *t = k->tcb;
        if (k->kind != K_TCP || !t || t->state == CLOSED || t->state == LISTEN)
            continue;
        if (t->timewait_at && (int32_t)(now - t->timewait_at) >= 0) {
            conn_closed(s, 0);
            continue;
        }
        if (t->persist_at && (int32_t)(now - t->persist_at) >= 0)
            tcp_output(s);
        if (!t->rexmt_at || (int32_t)(now - t->rexmt_at) < 0)
            continue;
        bool syn = t->state == SYN_SENT || t->state == SYN_RCVD;
        if (++t->retries > (syn ? SYN_RETRIES : MAX_RETRIES)) {
            if (t->state == SYN_RCVD && t->parent >= 0 && !k->pid)
                conn_closed(s, 0);
            else
                abort_conn(s, ETIMEDOUT);
            continue;
        }
        t->rto = t->rto * 2 < RTO_MAX ? t->rto * 2 : RTO_MAX;
        t->rtt_on = false;                      /* Karn: don't time a resent segment */
        uint32_t flight = t->snd_nxt - t->snd_una;
        t->ssthresh = flight / 2 > 2u * t->mss ? flight / 2 : 2u * t->mss;
        t->cwnd = t->mss;
        t->rexmt_at = now + t->rto;
        if (t->state == SYN_SENT) {
            send_segment(s, t->iss, F_SYN, 0, 0, true);
        } else if (t->state == SYN_RCVD) {
            send_segment(s, t->iss, F_SYN | F_ACK, 0, 0, true);
        } else {                                /* go back to the first unacknowledged byte */
            if (t->fin_ever && t->snd_una != t->snd_max)
                t->fin_sent = false;            /* it goes again after the data */
            t->snd_nxt = t->snd_una;
            tcp_output(s);
        }
    }
}

void tcp_icmp_error(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport, int err)
{
    int s = find_conn(lip, lport, rip, rport);
    if (s >= 0 && socks[s].tcb->state == SYN_SENT)
        conn_closed(s, err);                    /* otherwise only a hint: keep trying */
}

/* ---- for socket.c ------------------------------------------------------------------- */

int tcp_connect(int s)
{
    struct sock *k = &socks[s];
    if (!net_is_local(k->rip) && !net_cfg.ip)
        return -ENETUNREACH;
    struct tcb *t = k->tcb ? k->tcb : tcb_new(s);
    if (!t)
        return -ENOBUFS;
    k->err = 0;
    if (!k->lip)
        k->lip = net_local_ip(k->rip);
    start_seq(t);
    t->state = SYN_SENT;
    t->retries = 0;
    t->rto = RTO_INIT;
    send_segment(s, t->iss, F_SYN, 0, 0, true);
    t->rexmt_at = net_now() + t->rto;
    if (k->nonblock)
        return -EINPROGRESS;
    while (t->state == SYN_SENT) {
        int rc = sock_block(s, 0);
        if (rc == -EINTR)
            return rc;
    }
    if (t->state == ESTABLISHED || t->state == CLOSE_WAIT)
        return 0;
    int e = k->err ? k->err : ECONNREFUSED;
    k->err = 0;
    return -e;
}

int tcp_listen(int s, int backlog)
{
    struct sock *k = &socks[s];
    struct tcb *t = k->tcb ? k->tcb : tcb_new(s);
    if (!t)
        return -ENOBUFS;
    if (t->state != CLOSED && t->state != LISTEN)
        return -EISCONN;
    t->state = LISTEN;
    t->backlog = backlog < 1 ? 1 : backlog > TCP_BACKLOG ? TCP_BACKLOG : backlog;
    return 0;
}

int tcp_accept(int s, int pid)
{
    struct sock *k = &socks[s];
    struct tcb *t = k->tcb;
    if (!t || t->state != LISTEN)
        return -EINVAL;
    for (;;) {
        if (t->aq_n) {
            int c = t->aq[0];
            for (int i = 1; i < t->aq_n; i++)
                t->aq[i - 1] = t->aq[i];
            t->aq_n--;
            socks[c].pid = pid;
            socks[c].tcb->parent = -1;
            return c;
        }
        if (k->nonblock)
            return -EWOULDBLOCK;
        int rc = sock_block(s, k->rcv_timeout);
        if (rc < 0)
            return rc;
        if (k->kind != K_TCP || k->tcb != t)
            return -ENOTSOCK;
    }
}

int tcp_send(int s, const uint8_t *buf, uint32_t len)
{
    struct sock *k = &socks[s];
    struct tcb *t = k->tcb;
    uint32_t done = 0;
    for (;;) {
        if (!t || (t->state != ESTABLISHED && t->state != CLOSE_WAIT)) {
            int e = k->err ? k->err : t && t->state == SYN_SENT ? ENOTCONN : t && t->state != CLOSED ? EPIPE : ENOTCONN;
            return done ? (int)done : -e;
        }
        if (t->fin_queued)
            return done ? (int)done : -ESHUTDOWN;
        uint32_t room = TCP_SBUF - t->s_len, n = len - done < room ? len - done : room;
        for (uint32_t i = 0; i < n; i++)
            t->sbuf[(t->s_head + t->s_len + i) % TCP_SBUF] = buf[done + i];
        t->s_len += n;
        done += n;
        if (n)
            tcp_output(s);
        if (done == len)
            return (int)len;
        if (k->nonblock)
            return done ? (int)done : -EWOULDBLOCK;
        int rc = sock_block(s, k->snd_timeout);
        if (rc < 0)
            return done ? (int)done : rc;
        if (k->kind != K_TCP || k->tcb != t)
            return done ? (int)done : -ENOTSOCK;
    }
}

int tcp_recv(int s, uint8_t *buf, uint32_t len, bool peek)
{
    struct sock *k = &socks[s];
    struct tcb *t = k->tcb;
    if (!t || t->state == LISTEN || (t->state == CLOSED && !t->r_len && !t->fin_rcvd && !k->err))
        return -ENOTCONN;
    if (!len)
        return 0;
    for (;;) {
        if (t->r_len) {
            uint32_t n = t->r_len < len ? t->r_len : len;
            for (uint32_t i = 0; i < n; i++)
                buf[i] = t->rbuf[(t->r_head + i) % TCP_RBUF];
            if (!peek) {
                uint32_t before = t->rcv_adv - t->rcv_nxt;
                t->r_head = (t->r_head + n) % TCP_RBUF;
                t->r_len -= n;
                if (t->state != CLOSED && t->state != TIME_WAIT &&
                    (window_of(t) - before >= t->mss || !before))
                    send_ack(s);                /* the window opened: say so */
            }
            return (int)n;
        }
        if (t->fin_rcvd || k->shut_rd)
            return 0;
        if (k->err) {
            int e = k->err;
            k->err = 0;
            return -e;
        }
        if (t->state == CLOSED)
            return 0;
        if (k->nonblock)
            return -EWOULDBLOCK;
        int rc = sock_block(s, k->rcv_timeout);
        if (rc < 0)
            return rc;
        if (k->kind != K_TCP || k->tcb != t)
            return -ENOTSOCK;
    }
}

void tcp_shutdown(int s, bool rd, bool wr)
{
    struct tcb *t = socks[s].tcb;
    (void)rd;
    if (!t || !wr || t->fin_queued)
        return;
    if (t->state == ESTABLISHED || t->state == CLOSE_WAIT) {
        t->fin_queued = true;
        tcp_output(s);
    }
}

void tcp_close(int s)
{
    struct sock *k = &socks[s];
    struct tcb *t = k->tcb;
    k->pid = 0;                                 /* nobody holds it now */
    if (!t) {
        sock_free(s);
        return;
    }
    switch (t->state) {
    case LISTEN:
        for (int i = 0; i < SOCK_MAX; i++)      /* connections nobody accepted */
            if (socks[i].kind == K_TCP && socks[i].tcb && socks[i].tcb->parent == s && !socks[i].pid) {
                socks[i].tcb->parent = -1;
                abort_conn(i, 0);
            }
        sock_free(s);
        return;
    case CLOSED:
    case SYN_SENT:
        sock_free(s);
        return;
    case TIME_WAIT:
    case FIN_WAIT_2:
        if (!t->timewait_at)
            t->timewait_at = net_now() + FIN_WAIT_T;
        return;
    default:
        break;
    }
    if (t->r_len) {                             /* unread data: reset, as BSD does */
        abort_conn(s, 0);
        return;
    }
    if (t->state == ESTABLISHED || t->state == CLOSE_WAIT || t->state == SYN_RCVD) {
        if (t->state == SYN_RCVD)
            t->state = ESTABLISHED;
        t->fin_queued = true;
        tcp_output(s);
    }
}

int tcp_poll(int s)
{
    struct sock *k = &socks[s];
    struct tcb *t = k->tcb;
    int r = k->err ? NET_POLLERR : 0;
    if (!t)
        return r;
    if (t->state == LISTEN)
        return r | (t->aq_n ? NET_POLLIN : 0);
    if (t->r_len || t->fin_rcvd || k->shut_rd || (t->state == CLOSED && t->iss))
        r |= NET_POLLIN;
    if ((t->state == ESTABLISHED || t->state == CLOSE_WAIT) && t->s_len < TCP_SBUF && !t->fin_queued)
        r |= NET_POLLOUT;
    if (t->state == CLOSED && t->iss)
        r |= NET_POLLHUP;
    return r;
}
