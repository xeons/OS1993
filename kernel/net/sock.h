/*
 * sock.h -- sockets, inside the stack (socket.c, tcp.c).
 */
#ifndef OS1993_SOCK_H
#define OS1993_SOCK_H

#include "net.h"

#define SOCK_MAX    32
#define DGRAM_QUEUE 16                  /* datagrams waiting to be read */
#define TCP_SBUF    8192
#define TCP_RBUF    8192
#define TCP_BACKLOG 8

enum { K_FREE, K_UDP, K_RAW, K_TCP };

struct dgram {
    struct dgram *next;
    uint32_t      ip;
    uint16_t      port, len;
    uint8_t       data[];
};

enum tcp_state { CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2,
                 CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT };

struct tcb {
    enum tcp_state state;
    uint32_t iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2, snd_data;
    uint32_t snd_max;                   /* the highest sequence number sent (snd_nxt goes back) */
    uint32_t irs, rcv_nxt, rcv_adv;     /* rcv_adv: the right edge we last advertised */
    uint16_t mss;                       /* the peer's */
    uint32_t cwnd, ssthresh;
    uint8_t *sbuf;                      /* bytes from snd_data: sent unacked, then unsent */
    uint32_t s_head, s_len;
    uint8_t *rbuf;                      /* received, not yet read */
    uint32_t r_head, r_len;
    bool     fin_queued, fin_rcvd;
    bool     fin_sent;                  /* our FIN is out (until a retransmission goes back) */
    bool     fin_ever;                  /* ... has been sent at all */
    uint32_t rto, srtt8, rttvar4;       /* ticks; srtt x8, rttvar x4 */
    bool     rtt_on;
    uint32_t rtt_seq, rtt_start;
    uint32_t rexmt_at, retries;         /* 0: not running */
    uint32_t persist_at, timewait_at;
    int      parent;                    /* a listener's half-open child: the listener */
    int      backlog, aq[TCP_BACKLOG], aq_n;
};

struct sock {
    int      kind;
    int      pid;                       /* 0: closed by its owner, finishing on its own */
    bool     nonblock, reuse, broadcast;
    uint32_t lip, rip;                  /* host order; rip 0: not connected */
    uint16_t lport, rport;
    int      err;                       /* a pending error (positive errno) */
    uint32_t rcv_timeout, snd_timeout;  /* ms, 0: none */
    bool     shut_rd, shut_wr;
    uint8_t  proto;                     /* raw: the protocol */
    struct dgram *rq, *rq_tail;
    int      rq_n;
    struct tcb *tcb;
};

extern struct sock socks[SOCK_MAX];

int  sock_new(int kind, int pid);       /* -> index, or -1 */
void sock_free(int s);
uint16_t sock_ephemeral(int kind);
int  sock_block(int s, uint32_t ms);    /* 0 woken, -ETIMEDOUT, -EINTR */

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b)  ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define SEQ_GE(a, b)  ((int32_t)((a) - (b)) >= 0)

/* tcp.c */
int  tcp_connect(int s);
int  tcp_listen(int s, int backlog);
int  tcp_accept(int s, int pid);
int  tcp_send(int s, const uint8_t *buf, uint32_t len);
int  tcp_recv(int s, uint8_t *buf, uint32_t len, bool peek);
void tcp_shutdown(int s, bool rd, bool wr);
void tcp_close(int s);                  /* the owner lets go */
void tcp_tick(void);
int  tcp_poll(int s);                   /* POLLIN | POLLOUT | ... */
void tcp_icmp_error(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport, int err);

#endif
