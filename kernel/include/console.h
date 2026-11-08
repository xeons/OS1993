#ifndef OS1993_CONSOLE_H
#define OS1993_CONSOLE_H

#include <stdint.h>

/* CGA attribute colours */
enum {
    C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LGRAY,
    C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE
};
#define ATTR(fg, bg) ((uint8_t)(((bg) << 4) | (fg)))

#define CON_COLS 80
#define CON_ROWS 25

void con_init(void);
void con_set_attr(uint8_t attr);
void con_clear(void);
void con_set_window(int top, int bottom);       /* scrolling region, inclusive */
void con_goto(int row, int col);
void con_putc(char c);
void con_puts(const char *s);
void con_write_at(int row, int col, uint8_t attr, const char *s);
void con_fill_row(int row, uint8_t attr, char c);
void con_show_cursor(int on);
void con_use_klow(void);        /* reach video RAM through the KLOW window */
void con_detach(void);          /* a VM takes the screen: draw into a shadow */
void con_attach(void);          /* back on screen: restore from the shadow */

#endif
