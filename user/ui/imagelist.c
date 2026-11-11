/*
 * imagelist.c -- image lists: pictures of one size, one byte per pixel
 * (theme palette indices, FFh transparent), in the calling process's heap.
 * Tree and list views draw their items' images from them. ILD_SELECTED
 * draws every other pixel in the selection colour, as Windows blends.
 */
#include "uipriv.h"

#define IL_MAGIC 0x31534D49u    /* "IMS1" */

struct imagelist {
    uint32_t magic;
    int      cx, cy, n, cap, grow;
    uint8_t *px;
};

static struct imagelist *il_from(HIMAGELIST h)
{
    return h && h->magic == IL_MAGIC ? h : NULL;
}

HIMAGELIST ImageList_Create(int cx, int cy, unsigned flags, int initial, int grow)
{
    (void)flags;
    if (cx <= 0 || cy <= 0 || cx > 64 || cy > 64)
        return NULL;
    struct imagelist *il = LocalAlloc(LMEM_ZEROINIT, sizeof *il);
    if (!il)
        return NULL;
    *il = (struct imagelist){ IL_MAGIC, cx, cy, 0, initial > 0 ? initial : 4, grow > 0 ? grow : 4, NULL };
    if (!(il->px = LocalAlloc(LMEM_FIXED, (uint32_t)(il->cap * cx * cy)))) {
        LocalFree(il);
        return NULL;
    }
    return il;
}

bool ImageList_Destroy(HIMAGELIST h)
{
    struct imagelist *il = il_from(h);
    if (!il)
        return false;
    il->magic = 0;
    LocalFree(il->px);
    LocalFree(il);
    return true;
}

int ImageList_AddPixels(HIMAGELIST h, const uint8_t *pixels)
{
    struct imagelist *il = il_from(h);
    if (!il || !pixels)
        return -1;
    uint32_t one = (uint32_t)(il->cx * il->cy);
    if (il->n == il->cap) {
        uint8_t *p = LocalReAlloc(il->px, (uint32_t)(il->cap + il->grow) * one, 0);
        if (!p)
            return -1;
        il->px = p;
        il->cap += il->grow;
    }
    memcpy(il->px + (uint32_t)il->n * one, pixels, one);
    return il->n++;
}

int ImageList_GetImageCount(HIMAGELIST h)
{
    struct imagelist *il = il_from(h);
    return il ? il->n : 0;
}

bool ImageList_GetIconSize(HIMAGELIST h, int *cx, int *cy)
{
    struct imagelist *il = il_from(h);
    if (!il)
        return false;
    if (cx) *cx = il->cx;
    if (cy) *cy = il->cy;
    return true;
}

bool ImageList_Draw(HIMAGELIST h, int i, HDC dc, int x, int y, unsigned style)
{
    struct imagelist *il = il_from(h);
    if (!il || i < 0 || i >= il->n || !dc)
        return false;
    uint32_t one = (uint32_t)(il->cx * il->cy);
    const uint8_t *src = il->px + (uint32_t)i * one;
    uint8_t tmp[64 * 64];
    if (style & ILD_DISABLED) {                     /* white shadow, then the grey shape */
        for (int k = 0; k < il->cx * il->cy; k++)
            tmp[k] = src[k] == 0xFF ? 0xFF : C_WHITE;
        DrawPixels(dc, x + 1, y + 1, il->cx, il->cy, tmp, il->cx);
        for (int k = 0; k < il->cx * il->cy; k++)
            tmp[k] = src[k] == 0xFF || src[k] == C_WHITE || src[k] == C_LIGHT || src[k] == C_FACE ? 0xFF : C_GRAY4;
        DrawPixels(dc, x, y, il->cx, il->cy, tmp, il->cx);
        return true;
    }
    if (style & ILD_BLEND25) {                      /* faded: every other pixel white */
        for (int r = 0; r < il->cy; r++)
            for (int c = 0; c < il->cx; c++) {
                uint8_t p = src[r * il->cx + c];
                tmp[r * il->cx + c] = p != 0xFF && ((r + c) & 1) ? C_WHITE : p;
            }
        DrawPixels(dc, x, y, il->cx, il->cy, tmp, il->cx);
        return true;
    }
    if (!(style & ILD_SELECTED)) {
        DrawPixels(dc, x, y, il->cx, il->cy, src, il->cx);
        return true;
    }
    for (int r = 0; r < il->cy; r++)
        for (int c = 0; c < il->cx; c++) {
            uint8_t p = src[r * il->cx + c];
            tmp[r * il->cx + c] = p != 0xFF && ((r + c) & 1) ? C_ACCENT : p;
        }
    DrawPixels(dc, x, y, il->cx, il->cy, tmp, il->cx);
    return true;
}
