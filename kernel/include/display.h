/*
 * display.h -- the GUI's display modes (display.c).
 */
#ifndef OS1993_DISPLAY_H
#define OS1993_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

struct display_mode;
struct display_info;

void display_init(void);                         /* boot: find the VBE adapter */
int  display_modes(struct display_mode *out, int max);
const struct display_info *display_current(void);
bool display_linear(void);                       /* in one of the adapter's modes */

/* In a graphics session's process context (the shared page tables): */
bool display_session_start(void);                /* VGA 640x480x16 */
void display_session_end(void);
int  display_set(int w, int h, int bpp);         /* 0, or -1 */

/* Focus switching (vga.c), also from interrupt handlers: */
void display_suspend(void);                      /* to text mode */
void display_resume(void);                       /* the mode again (the GUI repaints) */
void display_force_off(void);                    /* panic */

#endif
