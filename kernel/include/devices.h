#ifndef OS1993_DEVICES_H
#define OS1993_DEVICES_H

#include <stdint.h>

/* timer.c -- PIT channel 0, left at the BIOS rate of ~18.2 Hz */
#define TIMER_HZ_X10 182
void timer_init(void);
uint32_t timer_ticks(void);

/* keyboard.c -- raw set-1 scancodes from the 8042 */
void keyboard_init(void);
int  keyboard_read(void);            /* next scancode, or -1 */
int  keyboard_poll_key(void);        /* interrupts off: wait for a key press */
void keyboard_release(void);         /* hand the keyboard to the DOS VM */
void keyboard_claim(void);           /* take it back */

#endif
