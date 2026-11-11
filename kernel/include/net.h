/*
 * net.h -- the TCP/IP stack (kernel/net/): the NE2000 driver, Ethernet,
 * ARP, IPv4, ICMP, UDP, TCP, DHCP, loopback, and the sockets programs use
 * through SYS_SOCKET.
 *
 * All of it runs with interrupts off: the NIC's interrupt only copies
 * frames into the receive queue, and the network thread (or a system
 * call) does the rest, so the stack's state never changes under it. Inside
 * the stack addresses and ports are in host byte order; packets are in
 * network order.
 */
#ifndef OS1993_NET_H
#define OS1993_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "user.h"                       /* struct net_config, struct sock_args */

/* Errors, as negative POSIX numbers (the C runtime's errno.h; WSOCK.DLL
 * maps them to Winsock's) */
#define EPERM            1
#define EINTR            4
#define EBADF            9
#define EAGAIN          11
#define EWOULDBLOCK     EAGAIN
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EINVAL          22
#define EMFILE          24
#define EPIPE           32
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP      95
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ENETDOWN       100
#define ENETUNREACH    101
#define ECONNABORTED   103
#define ECONNRESET     104
#define ENOBUFS        105
#define EISCONN        106
#define ENOTCONN       107
#define ESHUTDOWN      108
#define ETIMEDOUT      110
#define ECONNREFUSED   111
#define EHOSTUNREACH   113
#define EALREADY       114
#define EINPROGRESS    115

bool user_thread_stopping(void);        /* user.c: the calling thread must end */

#define ETH_MTU     1500
#define ETH_HLEN    14
#define PKT_SIZE    1536

struct pkt {                            /* a frame (from the pool) */
    struct pkt *next;
    uint16_t    len;                    /* bytes in data */
    bool        is_ip;                  /* looped back: data is an IP packet, no Ethernet header */
    uint8_t     data[PKT_SIZE];
};

/* ---- ne2000.c ------------------------------------------------------------- */
bool ne2000_init(uint8_t mac[6]);       /* found and running? */
bool ne2000_send(const uint8_t *frame, uint32_t len);
void ne2000_stop(void);

/* ---- net.c: buffers, the network thread, Ethernet, ARP, IP, ICMP ---------- */
struct pkt *pkt_alloc(void);
void        pkt_free(struct pkt *p);
void        net_rx(struct pkt *p);      /* the driver's interrupt: a frame came in */

extern struct net_config net_cfg;

void     net_init(void);
uint16_t ip_checksum(const void *data, uint32_t len, uint32_t sum);
uint32_t ip_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);
/* Send an IP packet: payload (header room before it is ours) -> 0, or -errno */
int      ip_send(uint32_t dst, uint8_t proto, const uint8_t *payload, uint32_t len, uint32_t src);
uint32_t net_local_ip(uint32_t dst);    /* the source address for a destination */
bool     net_is_local(uint32_t ip);     /* ours (or loopback) */
void     net_wake(int sock);            /* threads waiting on a socket (or -1: all) */
uint32_t net_now(void);                 /* timer ticks (55 ms) */

static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

#define IP_HLEN     20
#define PROTO_ICMP  1
#define PROTO_TCP   6
#define PROTO_UDP   17
#define IP_LOOPBACK 0x7F000001u

/* ---- socket.c ---------------------------------------------------------------- */
struct sock_args;
int  sock_syscall(int pid, uint32_t op, struct sock_args *a);   /* -> result, or -errno */
void sock_process_end(int pid);
void udp_input(const uint8_t *ip, uint32_t src, uint32_t dst, const uint8_t *udp, uint32_t len);
void icmp_unreachable(const uint8_t *ip, uint8_t code);         /* net.c: about this packet */
void raw_input(const uint8_t *ip, uint32_t len);               /* ICMP for raw sockets */
void sock_icmp_error(uint8_t type, uint8_t code, const uint8_t *orig, uint32_t len);
void sock_tick(void);                                          /* every timer tick */

/* ---- tcp.c --------------------------------------------------------------------- */
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint32_t len);

#endif
