/*
 * shellpriv.h -- between SHELL.DLL and the taskbar (SHELL.BIN).
 */
#ifndef OS1993_SHELLPRIV_H
#define OS1993_SHELLPRIV_H

#include "os1993.h"

#define TRAY_CLASS      "TaskbarWindow"     /* the taskbar */
#define DESKTOP_CLASS   "DesktopWindow"         /* the desktop */
#define TRAY_COPYDATA   0x54524159u         /* "TRAY": WM_COPYDATA dwData */

struct tray_msg {                           /* Shell_NotifyIcon's message */
    unsigned       msg;                     /* NIM_* */
    NOTIFYICONDATA nid;
};

#endif
