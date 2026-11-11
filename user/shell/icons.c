/*
 * icons.c -- SHELL.DLL's stock icons: 16x16 pictures in the theme's
 * colours, drawn as they are for small icons and doubled for large ones.
 *
 *   .  transparent   k  black      1-5  greys (dark to light)
 *   f  face          l  light      w    white
 *   a  accent        A  pale accent d   dark accent   D  desktop
 *   r  red           g  green      y    yellow
 */
#include "os1993.h"

static const char *const art[ICON_COUNT][16] = {
    [ICON_APP] = {
        "................",
        ".kkkkkkkkkkkkkk.",
        ".kaaaaaaaaaaaak.",
        ".kaaaaaaaaaaaak.",
        ".kkkkkkkkkkkkkk.",
        ".kwwwwwwwwwwwwk.",
        ".kwwwwwwwwwwwwk.",
        ".kwwwwwwwwwwwwk.",
        ".kwwwwwwwwwwwwk.",
        ".kwwwwwwwwwwwwk.",
        ".kwwwwwwwwwwwwk.",
        ".kwwwwwwwwwwwwk.",
        ".kwwwwwwwwwwwwk.",
        ".kkkkkkkkkkkkkk.",
        "................",
        "................",
    },
    [ICON_CALC] = {
        "...kkkkkkkkkk...",
        "...k44444444k...",
        "...k4kkkkkk4k...",
        "...k4kggggk4k...",
        "...k4kkkkkk4k...",
        "...k44444444k...",
        "...kww4ww4rrk...",
        "...k44444444k...",
        "...kww4ww4rrk...",
        "...k44444444k...",
        "...kww4ww4rrk...",
        "...k44444444k...",
        "...kww4ww4ggk...",
        "...k44444444k...",
        "...kkkkkkkkkk...",
        "................",
    },
    [ICON_PAINT] = {
        "................",
        "....kkkkkk......",
        "...kwwwwwwkk....",
        "..kwrrwwggwwk...",
        "..kwrrwwggwwk...",
        ".kwwwwwwwwwwwk..",
        ".kwaawwwwwyywk..",
        ".kwaawwwwwyywk..",
        ".kwwwwkkwwwwwk..",
        "..kwwk..kwwwk...",
        "...kkk...kkk....",
        "...........kk...",
        "..........k3k...",
        ".........k3k....",
        "........kkk.....",
        "................",
    },
    [ICON_CLOCK] = {
        ".....kkkkkk.....",
        "...kkwwwwwwkk...",
        "..kwwwwkwwwwwk..",
        ".kwwwwwkwwwwwwk.",
        ".kwwwwwkwwwwwwk.",
        "kwwwwwwkwwwwwwwk",
        "kwwwwwwkwwwwwwwk",
        "kkwwwwwkkkkkwwkk",
        "kwwwwwwwwwwwwwwk",
        "kwwwwwwwwwwwwwwk",
        ".kwwwwwwwwwwwwk.",
        ".kwwwwwwwwwwwwk.",
        "..kwwwwwkwwwwk..",
        "...kkwwwwwwkk...",
        ".....kkkkkk.....",
        "................",
    },
    [ICON_DEMO] = {
        "................",
        "kkkkkkkkkkkkkkkk",
        "k3l3l3l3l3l3l3lk",
        "kkkkkkkkkkkkkkkk",
        "kffffffffffffffk",
        "kfkkkfwwwwwwwwfk",
        "kfkkkffffffffffk",
        "kffffffffffffffk",
        "kfkkkfwwwwwwwwfk",
        "kfkkkffffffffffk",
        "kffffffffffffffk",
        "kffffkkkkkkkfffk",
        "kffffkwwwwwkfffk",
        "kffffkkkkkkkfffk",
        "kkkkkkkkkkkkkkkk",
        "................",
    },
    [ICON_DOS] = {
        "................",
        "kkkkkkkkkkkkkkkk",
        "k55555555555555k",
        "kkkkkkkkkkkkkkkk",
        "kkkkkkkkkkkkkkkk",
        "kkwwkkkkwkkwkkkk",
        "kwkkkwkkkwkkwkkk",
        "kwkkkkkkkkwkkwkk",
        "kwkkkwkkkkwkwkkk",
        "kkwwkkkkkkkwkkkk",
        "kkkkkkkkkkkkkkkk",
        "kkwwwwkkkkkkkkkk",
        "kkkkkkkkkkkkkkkk",
        "kkkkkkkkkkkkkkkk",
        "kkkkkkkkkkkkkkkk",
        "................",
    },
    [ICON_COMPUTER] = {
        "..kkkkkkkkkkkk..",
        "..k5555555555k..",
        "..k5kkkkkkkk5k..",
        "..k5kDDDDDDk5k..",
        "..k5kDDDDDDk5k..",
        "..k5kDDDDDDk5k..",
        "..k5kkkkkkkk5k..",
        "..k5555555555k..",
        "..kkkkkkkkkkkk..",
        "......k55k......",
        "....kkkkkkkk....",
        ".kkkkkkkkkkkkkk.",
        ".k555555555555k.",
        ".k5555555gg555k.",
        ".kkkkkkkkkkkkkk.",
        "................",
    },
    [ICON_INFO] = {
        ".....kkkkkk.....",
        "...kkaaaaaakk...",
        "..kaaaawwaaaak..",
        ".kaaaaawwaaaaak.",
        ".kaaaaaaaaaaaak.",
        "kaaaaawwwaaaaaak",
        "kaaaaaawwaaaaaak",
        "kaaaaaawwaaaaaak",
        "kaaaaaawwaaaaaak",
        "kaaaaaawwaaaaaak",
        ".kaaaaawwaaaaak.",
        ".kaaaawwwwaaaak.",
        "..kaaaaaaaaaak..",
        "...kkaaaaaakk...",
        ".....kkkkkk.....",
        "................",
    },
    [ICON_NOTEPAD] = {
        "................",
        "..kkkkkkkkkk....",
        "..kwwwwwwwwkk...",
        "..kwkkkkkwwkwk..",
        "..kwwwwwwwwkkkk.",
        "..kwkkkkkkwwwwk.",
        "..kwwwwwwwwwwwk.",
        "..kwkkkkkkkkwwk.",
        "..kwwwwwwwwwwwk.",
        "..kwkkkkkwwwwwk.",
        "..kwwwwwwwwwwwk.",
        "..kwkkkkkkkkwwk.",
        "..kwwwwwwwwwwwk.",
        "..kkkkkkkkkkkkk.",
        "................",
        "................",
    },
    [ICON_FOLDER] = {
        "................",
        "................",
        "..kkkkk.........",
        ".kyyyyyk........",
        "kyyyyyyykkkkkkk.",
        "kylllllllllllyk3",
        "kylyyyyyyyyyyyk3",
        "kyyyyyyyyyyyyyk3",
        "kyyyyyyyyyyyyyk3",
        "kyyyyyyyyyyyyyk3",
        "kyyyyyyyyyyyyyk3",
        "kyyyyyyyyyyyyyk3",
        "kyyyyyyyyyyyyyk3",
        "kkkkkkkkkkkkkkk3",
        ".333333333333333",
        "................",
    },
    [ICON_FOLDER_OPEN] = {
        "................",
        "................",
        "..kkkkk.........",
        ".kyyyyyk........",
        "kyyyyyyykkkkkk..",
        "kylllllllllllk..",
        "kyl...kkkkkkkkkk",
        "kyl..kllllllllyk",
        "kyl.klyyyyyyyyk.",
        "kylklyyyyyyyyyk.",
        "kykllyyyyyyyyk..",
        "kkllyyyyyyyyyk..",
        "kklyyyyyyyyyk...",
        "kkkkkkkkkkkkk...",
        "................",
        "................",
    },
    [ICON_DRIVE] = {
        "................",
        "................",
        "................",
        "................",
        "...kkkkkkkkkkk..",
        "..kwwwwwwwwwwk3.",
        ".kwwwwwwwwwwk33.",
        "kkkkkkkkkkkkk33.",
        "kwwwwwwwwwwwk33.",
        "kw55555555ggk3..",
        "kw555555555wk3..",
        "kwwwwwwwwwwwk...",
        "kkkkkkkkkkkkk...",
        ".3333333333333..",
        "................",
        "................",
    },
    [ICON_FLOPPY] = {
        "................",
        ".kkkkkkkkkkkkk..",
        ".kdk5555555kdk..",
        ".kdk5555555kdk..",
        ".kdk55kk555kdk..",
        ".kdk55kk555kdk..",
        ".kdkkkkkkkkkdk..",
        ".kdddddddddddk..",
        ".kddwwwwwwwddk..",
        ".kddwkkkkkwddk..",
        ".kddwwwwwwwddk..",
        ".kddwkkkkkwddk..",
        ".kddwwwwwwwddk..",
        ".kkkkkkkkkkkkk..",
        "................",
        "................",
    },
    [ICON_DOCUMENT] = {
        "..kkkkkkkk......",
        "..kwwwwwwkk.....",
        "..kwwwwwwk5k....",
        "..kwwwwwwk55k...",
        "..kwwwwwwkkkkk..",
        "..kwwwwwwwwwwk..",
        "..kwwwwwwwwwwk..",
        "..kwwwwwwwwwwk..",
        "..kwwwwwwwwwwk..",
        "..kwwwwwwwwwwk..",
        "..kwwwwwwwwwwk..",
        "..kwwwwwwwwwwk..",
        "..kwwwwwwwwwwk..",
        "..kwwwwwwwwwwk..",
        "..kkkkkkkkkkkk..",
        "................",
    },
    [ICON_TEXT] = {
        "..kkkkkkkk......",
        "..kwwwwwwkk.....",
        "..kwwwwwwk5k....",
        "..kw3333wk55k...",
        "..kwwwwwwkkkkk..",
        "..kw33333333wk..",
        "..kwwwwwwwwwwk..",
        "..kw3333333wwk..",
        "..kwwwwwwwwwwk..",
        "..kw33333333wk..",
        "..kwwwwwwwwwwk..",
        "..kw333333wwwk..",
        "..kwwwwwwwwwwk..",
        "..kwwwwwwwwwwk..",
        "..kkkkkkkkkkkk..",
        "................",
    },
    [ICON_EXPLORER] = {
        "................",
        "..kkkkk.........",
        ".kyyyyyk........",
        "kyyyyyyykkkkkk..",
        "kylllllllllllk..",
        "kylyyyyyyyyyyk..",
        "kyyyyyykkkkyyk..",
        "kyyyyykAAAAkyk..",
        "kyyyykAwAAAAkk..",
        "kyyyykAAAAAAkk..",
        "kyyyykAAAAAAkk..",
        "kkkkkkkAAAAkkk..",
        ".......kkkkkk...",
        "...........kkk..",
        "............kkk.",
        ".............kk.",
    },
    [ICON_SHUTDOWN] = {
        ".....kkkkkk.....",
        "...kkrrrrrrkk...",
        "..krrrrwwrrrrk..",
        ".krrwrrwwrrwrrk.",
        ".krwrrrwwrrrwrk.",
        "krwrrrrwwrrrrwrk",
        "krwrrrrwwrrrrwrk",
        "krwrrrrrrrrrrwrk",
        "krwrrrrrrrrrrwrk",
        "krrwrrrrrrrrwrrk",
        ".krrwrrrrrrwrrk.",
        ".krrrwwwwwwrrrk.",
        "..krrrrrrrrrrk..",
        "...kkrrrrrrkk...",
        ".....kkkkkk.....",
        "................",
    },
};

static uint8_t colour(char c)
{
    switch (c) {
    case 'k': return C_BLACK;
    case '1': return C_GRAY1;
    case '2': return C_GRAY2;
    case '3': return C_GRAY3;
    case '4': return C_GRAY4;
    case '5': return C_GRAY5;
    case 'f': return C_FACE;
    case 'l': return C_LIGHT;
    case 'w': return C_WHITE;
    case 'a': return C_ACCENT;
    case 'A': return C_ACCENT_LT;
    case 'd': return C_ACCENT_DK;
    case 'D': return C_DESKTOP;
    case 'r': return C_RED;
    case 'g': return C_GREEN;
    case 'y': return C_YELLOW;
    }
    return 0xFF;                                    /* transparent */
}

static void draw(HDC dc, int x, int y, int icon, int scale)
{
    if (icon <= ICON_NONE || icon >= ICON_COUNT || !art[icon][0])
        icon = ICON_APP;
    uint8_t px[32 * 32];
    int n = 16 * scale;
    for (int r = 0; r < n; r++)
        for (int c = 0; c < n; c++)
            px[r * n + c] = colour(art[icon][r / scale][c / scale]);
    DrawPixels(dc, x, y, n, n, px, n);
}

void DrawIcon(HDC dc, int x, int y, int icon)      { draw(dc, x, y, icon, 2); }
void DrawSmallIcon(HDC dc, int x, int y, int icon) { draw(dc, x, y, icon, 1); }

int ImageList_AddIcon(HIMAGELIST il, int icon)
{
    int cx, cy;
    if (!ImageList_GetIconSize(il, &cx, &cy) || cx != cy || (cx != 16 && cx != 32))
        return -1;
    if (icon <= ICON_NONE || icon >= ICON_COUNT || !art[icon][0])
        icon = ICON_APP;
    uint8_t px[32 * 32];
    int scale = cx / 16;
    for (int r = 0; r < cx; r++)
        for (int c = 0; c < cx; c++)
            px[r * cx + c] = colour(art[icon][r / scale][c / scale]);
    return ImageList_AddPixels(il, px);
}

bool GetIconPixels(int icon, int size, uint8_t *out)
{
    if (!out || (size != 16 && size != 32))
        return false;
    if (icon <= ICON_NONE || icon >= ICON_COUNT || !art[icon][0])
        icon = ICON_APP;
    int scale = size / 16;
    for (int r = 0; r < size; r++)
        for (int c = 0; c < size; c++)
            out[r * size + c] = colour(art[icon][r / scale][c / scale]);
    return true;
}
