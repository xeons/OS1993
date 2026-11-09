/*
 * theme.h -- the look: a Platinum-inspired grey palette and the metrics of
 * window frames and controls.
 */
#ifndef OS1993_THEME_H
#define OS1993_THEME_H

#include <stdint.h>

/* Palette indices (user/gui.c loads the colours) */
enum {
    C_BLACK,
    C_GRAY1,        /* #333333  deep shadow, inactive outlines */
    C_GRAY2,        /* #555555 */
    C_GRAY3,        /* #777777  disabled text */
    C_GRAY4,        /* #999999  bevel shadow */
    C_GRAY5,        /* #BBBBBB  title stripes, tracks */
    C_FACE,         /* #DDDDDD  window and control faces */
    C_LIGHT,        /* #EEEEEE  raised highlights */
    C_WHITE,
    C_ACCENT,       /* lavender blue: selection, focus ring */
    C_ACCENT_LT,    /* pale lavender: progress, thumbs */
    C_DESKTOP,
    C_RED,
    C_GREEN,
    C_YELLOW,
    C_ACCENT_DK,    /* deep indigo: selection text background */
};

extern const uint8_t theme_palette[16][3];

/* Window frame */
#define FRAME       5       /* border thickness, resizable */
#define TITLE_H     19      /* title bar height inside the frame */
#define CLIENT_TOP  (FRAME + TITLE_H + 1)
#define BOX         13      /* title bar box size */
#define TILE_W      160     /* a minimized window's tile */
#define TILE_H      (FRAME * 2 + TITLE_H)

/* Controls */
#define BTN_H       22
#define CHECK       13      /* check box / radio size */
#define SCROLL_W    16      /* scroll bar thickness */
#define EDIT_H      22

#endif
