#ifndef OS1993_SERIAL_H
#define OS1993_SERIAL_H

/* COM1 debug terminal, 115200 8N1 -- the moral equivalent of WDEB386. */
void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_flush(void);
int  serial_getc(void);          /* -1: nothing there */
#include <stdbool.h>
bool serial_present(void);

#endif
