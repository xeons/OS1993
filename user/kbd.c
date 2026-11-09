/* user/kbd.c -- scancode set 1 to ASCII, US layout. */
#include "ui.h"

static const char normal[] =
    "\0\0331234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char shifted[] =
    "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

static bool shift;

char kbd_ascii(int sc)
{
    if (sc == 0x2A || sc == 0x36) { shift = true;  return 0; }
    if (sc == 0xAA || sc == 0xB6) { shift = false; return 0; }
    if (sc < 0 || sc >= (int)sizeof normal - 1)
        return 0;
    return shift ? shifted[sc] : normal[sc];
}

bool kbd_shift(void) { return shift; }
