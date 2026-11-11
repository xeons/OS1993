/*
 * net.c -- the stack's core: packet buffers, the network thread,
 * Ethernet, ARP, IPv4, ICMP, loopback, and the DHCP client.
 *
 * The NIC's interrupt queues received frames and wakes the network
 * thread; so does every timer tick. The thread takes each frame up the
 * stack and runs the timers (ARP, DHCP, TCP), all with interrupts off.
 * Packets to our own address or 127.0.0.0/8 go round through the same
 * queue, so nothing is processed inside the call that sent it.
 *
 * IP: no fragments (we send with Don't Fragment, and drop fragments we
 * get), no options, one interface plus loopback.
 */
#include "kernel.h"
#include "io.h"
#include "mm.h"
#include "net.h"
#include "random.h"
#include "sched.h"
#include "devices.h"

struct net_config net_cfg;

/* ---- packet buffers ---------------------------------------------------------- */

#define PKT_POOL 48

static struct pkt *pool;

struct pkt *pkt_alloc(void)
{
    uint32_t f = irq_save();
    struct pkt *p = pool;
    if (p) {
        pool = p->next;
        p->next = NULL;
        p->len = 0;
        p->is_ip = false;
    }
    irq_restore(f);
    return p;
}

void pkt_free(struct pkt *p)
{
    if (!p)
        return;
    uint32_t f = irq_save();
    p->next = pool;
    pool = p;
    irq_restore(f);
}

/* ---- the network thread --------------------------------------------------------- */

static struct thread *net_thread;
static struct pkt *rxq, *rxq_tail;
static uint32_t last_tick;

static void enqueue(struct pkt *p)
{
    uint32_t f = irq_save();
    p->next = NULL;
    if (rxq_tail)
        rxq_tail->next = p;
    else
        rxq = p;
    rxq_tail = p;
    thread_wake(net_thread);
    irq_restore(f);
}

void net_rx(struct pkt *p)
{
    net_cfg.rx_packets++;
    enqueue(p);
}

uint32_t net_now(void) { return timer_ticks(); }

void net_tick(void)                                 /* the timer interrupt */
{
    if (net_thread)
        thread_wake(net_thread);
}

static void eth_input(const uint8_t *f, uint32_t len);
static void ip_input(const uint8_t *ip, uint32_t len);
static void arp_tick(void);
static void dhcp_tick(void);

static void net_main(void *arg)
{
    (void)arg;
    for (;;) {
        cpu_cli();
        while (rxq) {
            struct pkt *p = rxq;
            rxq = p->next;
            if (!rxq)
                rxq_tail = NULL;
            if (p->is_ip)
                ip_input(p->data, p->len);
            else
                eth_input(p->data, p->len);
            pkt_free(p);
        }
        uint32_t now = timer_ticks();
        if (now != last_tick) {
            last_tick = now;
            arp_tick();
            dhcp_tick();
            sock_tick();
        }
        if (!rxq) {
            current->state = T_BLOCKED;
            schedule();
        }
        cpu_sti();
    }
}

/* ---- checksums -------------------------------------------------------------------- */

static uint32_t sum16(const uint8_t *p, uint32_t len, uint32_t sum)
{
    for (; len > 1; len -= 2, p += 2)
        sum += (uint32_t)(p[0] << 8 | p[1]);
    if (len)
        sum += (uint32_t)p[0] << 8;
    return sum;
}

uint16_t ip_checksum(const void *data, uint32_t len, uint32_t sum)
{
    sum = sum16(data, len, sum);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t ip_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len)
{
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
}

/* ---- Ethernet ------------------------------------------------------------------- */

#define ETH_IP  0x0800
#define ETH_ARP 0x0806

static const uint8_t broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* p->data holds the payload from offset ETH_HLEN; fill in the header, send. */
static void eth_send(struct pkt *p, const uint8_t *dst, uint16_t type)
{
    memcpy(p->data, dst, 6);
    memcpy(p->data + 6, net_cfg.mac, 6);
    put16(p->data + 12, type);
    if (ne2000_send(p->data, p->len))
        net_cfg.tx_packets++;
    else
        net_cfg.tx_dropped++;
}

/* ---- ARP ------------------------------------------------------------------------- */

#define ARP_N       16
#define ARP_LIFE    (10u * 60 * 182 / 10)       /* 10 minutes */
#define ARP_RETRY   18                          /* 1 second */
#define ARP_TRIES   3

static struct arp_entry {
    uint32_t    ip;
    uint8_t     mac[6];
    uint8_t     state;                          /* 0 free, 1 asking, 2 known */
    uint8_t     tries;
    uint32_t    time;                           /* known since / last asked */
    struct pkt *waiting;                        /* a frame to send when it answers */
} arp[ARP_N];

static void arp_send(uint16_t op, const uint8_t *tmac, uint32_t tip)
{
    struct pkt *p = pkt_alloc();
    if (!p)
        return;
    uint8_t *a = p->data + ETH_HLEN;
    put16(a, 1);                                /* Ethernet */
    put16(a + 2, ETH_IP);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, net_cfg.mac, 6);
    put32(a + 14, net_cfg.ip);
    memcpy(a + 18, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tmac, 6);
    put32(a + 24, tip);
    p->len = ETH_HLEN + 28;
    eth_send(p, op == 1 ? broadcast_mac : tmac, ETH_ARP);
    pkt_free(p);
}

static struct arp_entry *arp_find(uint32_t ip)
{
    for (int i = 0; i < ARP_N; i++)
        if (arp[i].state && arp[i].ip == ip)
            return &arp[i];
    return NULL;
}

static void arp_learn(uint32_t ip, const uint8_t *mac, bool create)
{
    struct arp_entry *e = arp_find(ip);
    if (!e && !create)
        return;
    if (!e) {
        struct arp_entry *oldest = &arp[0];
        for (int i = 0; i < ARP_N && !e; i++) {
            if (!arp[i].state)
                e = &arp[i];
            else if (arp[i].time < oldest->time)
                oldest = &arp[i];
        }
        if (!e) {
            e = oldest;
            pkt_free(e->waiting);
            e->waiting = NULL;
        }
        e->ip = ip;
    }
    memcpy(e->mac, mac, 6);
    e->state = 2;
    e->time = timer_ticks();
    if (e->waiting) {
        struct pkt *p = e->waiting;
        e->waiting = NULL;
        eth_send(p, e->mac, ETH_IP);
        pkt_free(p);
    }
}

static void arp_input(const uint8_t *a, uint32_t len)
{
    if (len < 28 || get16(a) != 1 || get16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4)
        return;
    uint16_t op = get16(a + 6);
    uint32_t sip = get32(a + 14), tip = get32(a + 24);
    bool for_us = net_cfg.ip && tip == net_cfg.ip;
    if (sip)
        arp_learn(sip, a + 8, for_us);
    if (op == 1 && for_us)
        arp_send(2, a + 8, sip);
}

static void arp_tick(void)
{
    uint32_t now = timer_ticks();
    for (int i = 0; i < ARP_N; i++) {
        struct arp_entry *e = &arp[i];
        if (e->state == 2 && now - e->time > ARP_LIFE) {
            e->state = 0;
        } else if (e->state == 1 && now - e->time >= ARP_RETRY) {
            if (++e->tries >= ARP_TRIES) {      /* nobody there */
                pkt_free(e->waiting);
                e->waiting = NULL;
                e->state = 0;
                net_cfg.tx_dropped++;
            } else {
                e->time = now;
                arp_send(1, NULL, e->ip);
            }
        }
    }
}

/* ---- IP ---------------------------------------------------------------------------- */

static uint16_t ip_ident;

bool net_is_local(uint32_t ip)
{
    return (ip >> 24) == 127 || (net_cfg.ip && ip == net_cfg.ip);
}

uint32_t net_local_ip(uint32_t dst)
{
    return (dst >> 24) == 127 ? dst : net_cfg.ip;
}

static uint32_t subnet_broadcast(void) { return net_cfg.ip | ~net_cfg.mask; }

int ip_send(uint32_t dst, uint8_t proto, const uint8_t *payload, uint32_t len, uint32_t src)
{
    if (len > ETH_MTU - IP_HLEN)
        return -EMSGSIZE;
    bool local = net_is_local(dst);
    if (!local && !net_cfg.up)
        return -ENETDOWN;
    if (!src)
        src = net_local_ip(dst);
    struct pkt *p = pkt_alloc();
    if (!p)
        return -ENOBUFS;
    uint8_t *ip = p->data + ETH_HLEN;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (uint16_t)(IP_HLEN + len));
    put16(ip + 4, ip_ident++);
    put16(ip + 6, 0x4000);                      /* don't fragment */
    ip[8] = 64;
    ip[9] = proto;
    put16(ip + 10, 0);
    put32(ip + 12, src);
    put32(ip + 16, dst);
    put16(ip + 10, ip_checksum(ip, IP_HLEN, 0));
    memcpy(ip + IP_HLEN, payload, len);
    if (local) {                                /* round through the queue */
        memmove(p->data, ip, IP_HLEN + len);
        p->len = (uint16_t)(IP_HLEN + len);
        p->is_ip = true;
        enqueue(p);
        return 0;
    }
    p->len = (uint16_t)(ETH_HLEN + IP_HLEN + len);
    if (dst == 0xFFFFFFFFu || (net_cfg.ip && dst == subnet_broadcast())) {
        eth_send(p, broadcast_mac, ETH_IP);
        pkt_free(p);
        return 0;
    }
    uint32_t hop = (dst & net_cfg.mask) == (net_cfg.ip & net_cfg.mask) ? dst : net_cfg.gateway;
    if (!net_cfg.ip || !hop) {
        pkt_free(p);
        return -ENETUNREACH;
    }
    struct arp_entry *e = arp_find(hop);
    if (e && e->state == 2) {
        eth_send(p, e->mac, ETH_IP);
        pkt_free(p);
        return 0;
    }
    if (!e) {                                   /* ask, and keep this frame for the answer */
        uint8_t none[6] = { 0 };
        arp_learn(hop, none, true);
        e = arp_find(hop);
        e->state = 1;
        e->tries = 0;
        e->time = timer_ticks();
        arp_send(1, NULL, hop);
    }
    pkt_free(e->waiting);                       /* only the latest waits */
    e->waiting = p;
    return 0;
}

static void icmp_input(const uint8_t *ip, uint32_t len, uint32_t src, uint32_t dst);
static void dhcp_input(const uint8_t *msg, uint32_t len);

static void ip_input(const uint8_t *ip, uint32_t len)
{
    if (len < IP_HLEN || (ip[0] >> 4) != 4)
        return;
    uint32_t ihl = (ip[0] & 15u) * 4, total = get16(ip + 2);
    if (ihl < IP_HLEN || total < ihl || total > len || ip_checksum(ip, ihl, 0) != 0)
        return;
    if (get16(ip + 6) & 0x3FFF)                 /* a fragment */
        return;
    uint32_t src = get32(ip + 12), dst = get32(ip + 16);
    const uint8_t *data = ip + ihl;
    uint32_t n = total - ihl;
    bool broadcast = dst == 0xFFFFFFFFu || (net_cfg.ip && dst == subnet_broadcast());
    if (ip[9] == PROTO_UDP && n >= 8 && get16(data + 2) == 68 && get16(data) == 67) {
        dhcp_input(data + 8, n - 8);            /* before we have an address, too */
        return;
    }
    if (!net_is_local(dst) && !broadcast)
        return;
    switch (ip[9]) {
    case PROTO_ICMP:
        icmp_input(ip, total, src, dst);
        break;
    case PROTO_UDP:
        if (n >= 8 && get16(data + 4) >= 8 && get16(data + 4) <= n &&
            (get16(data + 6) == 0 ||
             ip_checksum(data, get16(data + 4), ip_pseudo_sum(src, dst, PROTO_UDP, get16(data + 4))) == 0))
            udp_input(ip, src, dst, data, get16(data + 4));
        break;
    case PROTO_TCP:
        if (!broadcast && n >= 20 && ip_checksum(data, n, ip_pseudo_sum(src, dst, PROTO_TCP, (uint16_t)n)) == 0)
            tcp_input(src, dst, data, n);
        break;
    default:
        if (!broadcast)
            icmp_unreachable(ip, 2);            /* protocol unreachable */
    }
}

static void eth_input(const uint8_t *f, uint32_t len)
{
    if (len < ETH_HLEN)
        return;
    uint16_t type = get16(f + 12);
    if (type == ETH_ARP)
        arp_input(f + ETH_HLEN, len - ETH_HLEN);
    else if (type == ETH_IP)
        ip_input(f + ETH_HLEN, len - ETH_HLEN);
}

/* ---- ICMP ---------------------------------------------------------------------- */

static void icmp_input(const uint8_t *ip, uint32_t len, uint32_t src, uint32_t dst)
{
    uint32_t ihl = (ip[0] & 15u) * 4, n = len - ihl;
    const uint8_t *m = ip + ihl;
    if (n < 8 || ip_checksum(m, n, 0) != 0)
        return;
    if (m[0] == 8 && net_is_local(dst)) {       /* echo request: answer it */
        static uint8_t reply[ETH_MTU];
        memcpy(reply, m, n);
        reply[0] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, ip_checksum(reply, n, 0));
        ip_send(src, PROTO_ICMP, reply, n, dst);
    } else if ((m[0] == 3 || m[0] == 11) && n >= 8 + IP_HLEN) {
        sock_icmp_error(m[0], m[1], m + 8, n - 8);  /* about a packet we sent */
    }
    raw_input(ip, len);                         /* and for raw sockets (ping) */
}

/* Destination unreachable about the packet at ip (its header and 8 bytes). */
void icmp_unreachable(const uint8_t *ip, uint8_t code)
{
    uint32_t ihl = (ip[0] & 15u) * 4, src = get32(ip + 12);
    if (!src || src == 0xFFFFFFFFu)
        return;
    uint8_t m[8 + 60 + 8];
    uint32_t n = 8 + ihl + 8;
    memset(m, 0, 8);
    m[0] = 3;
    m[1] = code;
    memcpy(m + 8, ip, ihl + 8);
    put16(m + 2, ip_checksum(m, n, 0));
    ip_send(src, PROTO_ICMP, m, n, 0);
}

/* ---- DHCP (RFC 2131): a client for the one interface ------------------------------- */

enum { D_OFF, D_DISCOVER, D_REQUEST, D_BOUND };
static struct {
    int      state;
    uint32_t xid, offered, server;
    uint32_t sent, tries;
    uint32_t renew_at;                          /* tick: ask again (half the lease) */
} dh;

static void dhcp_send(uint8_t type)
{
    static uint8_t msg[8 + 300];
    uint8_t *b = msg + 8;
    memset(msg, 0, sizeof msg);
    b[0] = 1;                                   /* BOOTREQUEST */
    b[1] = 1;
    b[2] = 6;
    put32(b + 4, dh.xid);
    put16(b + 10, 0x8000);                      /* answer by broadcast: we have no address yet */
    memcpy(b + 28, net_cfg.mac, 6);
    put32(b + 236, 0x63825363);                 /* the magic cookie */
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 61; *o++ = 7; *o++ = 1;
    memcpy(o, net_cfg.mac, 6);
    o += 6;
    if (type == 3) {                            /* REQUEST: the address offered, from whom */
        *o++ = 50; *o++ = 4; put32(o, dh.offered); o += 4;
        *o++ = 54; *o++ = 4; put32(o, dh.server); o += 4;
    }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    *o++ = 12; *o++ = 6; memcpy(o, "OS1993", 6); o += 6;
    *o++ = 255;
    uint16_t len = 8 + 300;
    put16(msg, 68);
    put16(msg + 2, 67);
    put16(msg + 4, len);
    put16(msg + 6, 0);
    put16(msg + 6, ip_checksum(msg, len, ip_pseudo_sum(0, 0xFFFFFFFFu, PROTO_UDP, len)));
    uint32_t keep = net_cfg.ip;                 /* from 0.0.0.0 */
    net_cfg.ip = 0;
    ip_send(0xFFFFFFFFu, PROTO_UDP, msg, len, 0);
    net_cfg.ip = keep;
    dh.sent = timer_ticks();
}

static void dhcp_start(void)
{
    dh.state = D_DISCOVER;
    dh.xid = random_u32();
    dh.tries = 0;
    net_cfg.dhcp = 1;
    dhcp_send(1);
}

static void dhcp_input(const uint8_t *b, uint32_t len)
{
    if (dh.state == D_OFF || len < 240 || b[0] != 2 || get32(b + 4) != dh.xid ||
        memcmp(b + 28, net_cfg.mac, 6) || get32(b + 236) != 0x63825363)
        return;
    uint8_t type = 0;
    uint32_t mask = 0, router = 0, dns = 0, lease = 0, server = 0;
    for (uint32_t i = 240; i < len && b[i] != 255; ) {
        if (b[i] == 0) { i++; continue; }
        if (i + 2 > len || i + 2 + b[i + 1] > len)
            break;
        const uint8_t *v = b + i + 2;
        uint8_t n = b[i + 1];
        switch (b[i]) {
        case 53: if (n >= 1) type = v[0]; break;
        case 1:  if (n >= 4) mask = get32(v); break;
        case 3:  if (n >= 4) router = get32(v); break;
        case 6:  if (n >= 4) dns = get32(v); break;
        case 51: if (n >= 4) lease = get32(v); break;
        case 54: if (n >= 4) server = get32(v); break;
        }
        i += 2u + n;
    }
    if (type == 2 && dh.state == D_DISCOVER) {  /* OFFER */
        dh.offered = get32(b + 16);
        dh.server = server;
        dh.state = D_REQUEST;
        dh.tries = 0;
        dhcp_send(3);
    } else if (type == 5 && dh.state == D_REQUEST) {    /* ACK */
        bool first = !net_cfg.ip;
        net_cfg.ip = get32(b + 16);
        net_cfg.mask = mask ? mask : 0xFFFFFF00u;
        net_cfg.gateway = router;
        net_cfg.dns = dns;
        net_cfg.dhcp_server = server;
        net_cfg.lease_secs = lease;
        net_cfg.dhcp = 2;
        dh.state = D_BOUND;
        uint32_t half = lease / 2 > 86400 || !lease ? 86400 : lease / 2;
        dh.renew_at = timer_ticks() + half * 182 / 10;
        if (first)
            klog("NET: DHCP: %u.%u.%u.%u/%u.%u.%u.%u, gateway %u.%u.%u.%u, DNS %u.%u.%u.%u (lease %u s)\n",
                 net_cfg.ip >> 24, net_cfg.ip >> 16 & 255, net_cfg.ip >> 8 & 255, net_cfg.ip & 255,
                 net_cfg.mask >> 24, net_cfg.mask >> 16 & 255, net_cfg.mask >> 8 & 255, net_cfg.mask & 255,
                 router >> 24, router >> 16 & 255, router >> 8 & 255, router & 255,
                 dns >> 24, dns >> 16 & 255, dns >> 8 & 255, dns & 255, lease);
    } else if (type == 6) {                     /* NAK: from the start */
        net_cfg.ip = 0;
        dhcp_start();
    }
}

static void dhcp_tick(void)
{
    uint32_t now = timer_ticks();
    if (dh.state == D_BOUND && (int32_t)(now - dh.renew_at) >= 0) {
        dh.state = D_REQUEST;                   /* renew: ask for the same address */
        dh.offered = net_cfg.ip;
        dh.tries = 0;
        dhcp_send(3);
    } else if ((dh.state == D_DISCOVER || dh.state == D_REQUEST) &&
               now - dh.sent >= (dh.tries < 4 ? 36u : 182u)) {     /* 2 s, later 10 s */
        if (++dh.tries == 4 && !net_cfg.ip)
            klog("NET: DHCP: no answer yet; still asking\n");
        if (dh.state == D_REQUEST && dh.tries > 3)
            dhcp_start();
        else
            dhcp_send(dh.state == D_DISCOVER ? 1 : 3);
    }
}

/* ---- start --------------------------------------------------------------------- */

void net_init(void)
{
    static struct pkt buffers[PKT_POOL];
    for (int i = 0; i < PKT_POOL; i++)
        pkt_free(&buffers[i]);
    ip_ident = (uint16_t)random_u32();
    net_thread = thread_create("Network", net_main, NULL, NULL);
    if (!net_thread)
        return;
    if (ne2000_init(net_cfg.mac)) {
        net_cfg.up = 1;
        klog("NET: NE2000 at 300h, IRQ 9, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", net_cfg.mac[0],
             net_cfg.mac[1], net_cfg.mac[2], net_cfg.mac[3], net_cfg.mac[4], net_cfg.mac[5]);
        uint32_t f = irq_save();
        dhcp_start();
        irq_restore(f);
    } else {
        klog("NET: no network card (loopback only)\n");
    }
    thread_wake(net_thread);
}
