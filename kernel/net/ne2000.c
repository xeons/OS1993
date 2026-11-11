/*
 * ne2000.c -- the NE2000 (National DP8390) Ethernet card, at port 300h,
 * IRQ 9: the ISA card most 1993 networks had, and the one QEMU's ISA
 * machine emulates (-nic user,model=ne2k_isa).
 *
 * The card has 16 KB of its own memory (pages of 256 bytes, 4000h-7FFFh):
 * a transmit buffer of six pages, then a ring the card receives into. The
 * host moves bytes in and out through the data port ("remote DMA"), a
 * word at a time. The interrupt copies every received frame into a pool
 * buffer for the network thread; sending waits for the previous frame to
 * go, with interrupts off, so the two never share the remote DMA.
 */
#include "kernel.h"
#include "interrupts.h"
#include "io.h"
#include "net.h"

#define NE_BASE     0x300
#define NE_IRQ      9
#define NE_DATA     (NE_BASE + 0x10)
#define NE_RESET    (NE_BASE + 0x1F)

/* page 0 registers */
#define CR      0x00
#define PSTART  0x01
#define PSTOP   0x02
#define BNRY    0x03
#define TPSR    0x04
#define TBCR0   0x05
#define TBCR1   0x06
#define ISR     0x07
#define RSAR0   0x08
#define RSAR1   0x09
#define RBCR0   0x0A
#define RBCR1   0x0B
#define RCR     0x0C
#define TCR     0x0D
#define DCR     0x0E
#define IMR     0x0F
/* page 1 */
#define PAR0    0x01
#define CURR    0x07
#define MAR0    0x08

#define CR_STP  0x01
#define CR_STA  0x02
#define CR_TXP  0x04
#define CR_RD_READ  0x08
#define CR_RD_WRITE 0x10
#define CR_RD_ABORT 0x20
#define CR_PAGE1    0x40

#define ISR_PRX 0x01
#define ISR_PTX 0x02
#define ISR_RXE 0x04
#define ISR_TXE 0x08
#define ISR_OVW 0x10
#define ISR_RDC 0x40
#define ISR_RST 0x80

#define TX_PAGE     0x40                /* 6 pages: one full frame */
#define RX_START    0x46
#define RX_STOP     0x80

static bool present;

static void wr(int reg, uint8_t v) { outb(NE_BASE + reg, v); }
static uint8_t rd(int reg)         { return inb(NE_BASE + reg); }

static void dma_setup(uint16_t addr, uint16_t count, uint8_t dir)
{
    wr(ISR, ISR_RDC);
    wr(RBCR0, (uint8_t)count);
    wr(RBCR1, (uint8_t)(count >> 8));
    wr(RSAR0, (uint8_t)addr);
    wr(RSAR1, (uint8_t)(addr >> 8));
    wr(CR, dir | CR_STA);
}

static void dma_done(void)
{
    for (int i = 0; i < 100000 && !(rd(ISR) & ISR_RDC); i++)
        ;
    wr(ISR, ISR_RDC);
}

static void card_read(uint16_t addr, uint8_t *buf, uint16_t len)   /* len even */
{
    dma_setup(addr, len, CR_RD_READ);
    for (uint16_t i = 0; i < len; i += 2) {
        uint16_t w = inw(NE_DATA);
        buf[i] = (uint8_t)w;
        buf[i + 1] = (uint8_t)(w >> 8);
    }
    dma_done();
}

static void receive(void)
{
    for (;;) {
        wr(CR, CR_PAGE1 | CR_RD_ABORT | CR_STA);
        uint8_t curr = rd(CURR);
        wr(CR, CR_RD_ABORT | CR_STA);
        uint8_t next = (uint8_t)(rd(BNRY) + 1);
        if (next >= RX_STOP)
            next = RX_START;
        if (next == curr)
            return;
        uint8_t hdr[4];                 /* status, next page, length (with the header) */
        card_read((uint16_t)(next << 8), hdr, 4);
        uint16_t len = (uint16_t)(hdr[2] | hdr[3] << 8);
        uint8_t after = hdr[1];
        bool ok = (hdr[0] & 0x01) && len >= 4 + ETH_HLEN && len <= 4 + ETH_MTU + ETH_HLEN + 4 &&
                  after >= RX_START && after < RX_STOP;
        if (!ok) {                      /* the ring is confused: start it again */
            wr(CR, CR_PAGE1 | CR_RD_ABORT | CR_STA);
            wr(CURR, RX_START + 1);
            wr(CR, CR_RD_ABORT | CR_STA);
            wr(BNRY, RX_START);
            net_cfg.rx_dropped++;
            return;
        }
        len -= 4;
        struct pkt *p = pkt_alloc();
        if (p) {
            uint16_t at = (uint16_t)((next << 8) + 4), even = (uint16_t)((len + 1) & ~1);
            uint16_t to_end = (uint16_t)((RX_STOP << 8) - at);
            if (even <= to_end) {
                card_read(at, p->data, even);
            } else {                    /* it wraps round the ring */
                card_read(at, p->data, to_end);
                card_read(RX_START << 8, p->data + to_end, (uint16_t)(even - to_end));
            }
            p->len = len;
            net_rx(p);
        } else {
            net_cfg.rx_dropped++;
        }
        wr(BNRY, after == RX_START ? RX_STOP - 1 : (uint8_t)(after - 1));
    }
}

static void ne_irq(struct regs *r)
{
    (void)r;
    uint8_t isr;
    while ((isr = rd(ISR) & (ISR_PRX | ISR_PTX | ISR_RXE | ISR_TXE | ISR_OVW)) != 0) {
        wr(ISR, isr);
        if (isr & ISR_OVW)              /* the ring filled: take what's there */
            net_cfg.rx_dropped++;
        if (isr & (ISR_PRX | ISR_OVW))
            receive();
    }
}

bool ne2000_send(const uint8_t *frame, uint32_t len)
{
    if (!present || len > ETH_HLEN + ETH_MTU)
        return false;
    uint32_t f = irq_save();
    for (int i = 0; i < 200000 && (rd(CR) & CR_TXP); i++)
        ;                               /* the previous frame is still going */
    uint16_t n = (uint16_t)(len < 60 ? 60 : len), even = (uint16_t)((n + 1) & ~1);
    dma_setup(TX_PAGE << 8, even, CR_RD_WRITE);
    for (uint16_t i = 0; i < even; i += 2) {
        uint8_t lo = i < len ? frame[i] : 0, hi = i + 1u < len ? frame[i + 1] : 0;
        outw(NE_DATA, (uint16_t)(lo | hi << 8));
    }
    dma_done();
    wr(TPSR, TX_PAGE);
    wr(TBCR0, (uint8_t)n);
    wr(TBCR1, (uint8_t)(n >> 8));
    wr(CR, CR_RD_ABORT | CR_TXP | CR_STA);
    irq_restore(f);
    return true;
}

bool ne2000_init(uint8_t mac[6])
{
    outb(NE_RESET, inb(NE_RESET));      /* reset */
    int i;
    for (i = 0; i < 10000 && !(rd(ISR) & ISR_RST); i++)
        ;
    if (i == 10000 || rd(CR) == 0xFF)
        return false;
    wr(ISR, 0xFF);
    wr(CR, CR_RD_ABORT | CR_STP);
    if ((rd(CR) & (CR_STP | CR_STA | CR_TXP)) != CR_STP)
        return false;
    wr(DCR, 0x48);                      /* bytes, FIFO 8: the address PROM first */
    wr(RBCR0, 0);
    wr(RBCR1, 0);
    wr(IMR, 0);
    wr(ISR, 0xFF);
    wr(RCR, 0x20);                      /* monitor: receive nothing yet */
    wr(TCR, 0x02);                      /* internal loopback */
    uint8_t prom[32];
    dma_setup(0, 32, CR_RD_READ);
    for (i = 0; i < 32; i++)
        prom[i] = inb(NE_DATA);
    dma_done();
    for (i = 0; i < 6; i++)
        mac[i] = prom[i * 2];           /* each byte is there twice */
    if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) || (mac[0] & 1))
        return false;

    wr(CR, CR_RD_ABORT | CR_STP);
    wr(DCR, 0x49);                      /* words from now on */
    wr(TPSR, TX_PAGE);
    wr(PSTART, RX_START);
    wr(PSTOP, RX_STOP);
    wr(BNRY, RX_START);
    wr(CR, CR_PAGE1 | CR_RD_ABORT | CR_STP);
    for (i = 0; i < 6; i++)
        wr(PAR0 + i, mac[i]);
    for (i = 0; i < 8; i++)
        wr(MAR0 + i, 0);
    wr(CURR, RX_START + 1);
    wr(CR, CR_RD_ABORT | CR_STA);
    wr(ISR, 0xFF);
    wr(IMR, ISR_PRX | ISR_PTX | ISR_RXE | ISR_TXE | ISR_OVW);
    wr(TCR, 0x00);                      /* normal */
    wr(RCR, 0x04);                      /* our address and broadcasts */
    present = true;
    irq_install(NE_IRQ, ne_irq);
    return true;
}

void ne2000_stop(void)
{
    if (!present)
        return;
    wr(IMR, 0);
    wr(CR, CR_RD_ABORT | CR_STP);
    irq_uninstall(NE_IRQ);
    present = false;
}
