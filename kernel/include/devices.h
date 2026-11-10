#ifndef OS1993_DEVICES_H
#define OS1993_DEVICES_H

#include <stdbool.h>
#include <stdint.h>

/* timer.c -- PIT channel 0, left at the BIOS rate of ~18.2 Hz */
#define TIMER_HZ_X10 182
void timer_init(void);
uint32_t timer_ticks(void);

/* keyboard.c -- raw set-1 scancodes from the 8042 */
void keyboard_init(void);
int  keyboard_read(void);            /* next scancode, or -1 */
bool keyboard_pending(void);
int  keyboard_poll_key(void);        /* interrupts off: wait for a key press */

/* mouse.c -- PS/2 mouse on IRQ 12 */
struct user_event;
void mouse_init(void);
void mouse_shutdown(void);           /* restore the 8042 before exit */
void mouse_byte(uint8_t b);          /* a byte from the auxiliary port */
bool mouse_present(void);
void mouse_set_delivery(bool on);    /* queue events for the GUI */
bool mouse_get_event(struct user_event *e);
bool mouse_pending(void);
void mouse_set_screen(int w, int h);  /* the pointer's range: the display mode */

#endif
