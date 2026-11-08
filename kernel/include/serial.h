#ifndef OS1993_SERIAL_H
#define OS1993_SERIAL_H

/* COM1 debug terminal, 115200 8N1 -- the moral equivalent of WDEB386. */
void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_flush(void);

#endif
