#include "serial.h"
#include "io.h"

#define COM1 0x3F8

static int present;

void serial_init(void)
{
    outb(COM1 + 7, 0x5A);                /* scratch register probe */
    if (inb(COM1 + 7) != 0x5A)
        return;
    outb(COM1 + 1, 0x00);                /* no UART interrupts */
    outb(COM1 + 3, 0x80);                /* DLAB on */
    outb(COM1 + 0, 0x01);                /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);                /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7);                /* FIFO on (16550), cleared */
    outb(COM1 + 4, 0x03);                /* DTR | RTS */
    present = 1;
}

void serial_putc(char c)
{
    if (!present)
        return;
    if (c == '\n')
        serial_putc('\r');
    for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++)
        ;
    outb(COM1, (uint8_t)c);
}

/* Wait until the transmitter (FIFO and shift register) is empty. */
void serial_flush(void)
{
    if (!present)
        return;
    for (int spin = 0; spin < 1000000 && !(inb(COM1 + 5) & 0x40); spin++)
        ;
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

bool serial_present(void) { return present; }

/* A byte from COM1, or -1 if none has arrived. */
int serial_getc(void)
{
    if (!present || !(inb(COM1 + 5) & 0x01))
        return -1;
    return inb(COM1);
}
