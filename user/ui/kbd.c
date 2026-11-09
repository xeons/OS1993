/* user/ui/kbd.c -- scancode set 1 -> virtual key -> character (US layout). */
#include "uipriv.h"

#define OEM_1      0xBA     /* ;: */
#define OEM_PLUS   0xBB     /* =+ */
#define OEM_COMMA  0xBC
#define OEM_MINUS  0xBD
#define OEM_PERIOD 0xBE
#define OEM_2      0xBF     /* /? */
#define OEM_3      0xC0     /* `~ */
#define OEM_4      0xDB     /* [{ */
#define OEM_5      0xDC     /* \| */
#define OEM_6      0xDD     /* ]} */
#define OEM_7      0xDE     /* '" */

static const uint8_t vk_of[0x59] = {
    [0x01] = VK_ESCAPE, [0x02] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    [0x0C] = OEM_MINUS, [0x0D] = OEM_PLUS, [0x0E] = VK_BACK, [0x0F] = VK_TAB,
    [0x10] = 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    [0x1A] = OEM_4, [0x1B] = OEM_6, [0x1C] = VK_RETURN, [0x1D] = VK_CONTROL,
    [0x1E] = 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
    [0x27] = OEM_1, [0x28] = OEM_7, [0x29] = OEM_3, [0x2A] = VK_SHIFT, [0x2B] = OEM_5,
    [0x2C] = 'Z', 'X', 'C', 'V', 'B', 'N', 'M',
    [0x33] = OEM_COMMA, [0x34] = OEM_PERIOD, [0x35] = OEM_2, [0x36] = VK_SHIFT,
    [0x37] = 0x6A, [0x38] = VK_MENU, [0x39] = VK_SPACE, [0x3A] = 0x14,
    [0x3B] = VK_F1, 0x71, 0x72, VK_F4, 0x74, 0x75, 0x76, 0x77, 0x78, VK_F10,
    [0x47] = VK_HOME, [0x48] = VK_UP, [0x49] = VK_PRIOR, [0x4B] = VK_LEFT,
    [0x4D] = VK_RIGHT, [0x4F] = VK_END, [0x50] = VK_DOWN, [0x51] = VK_NEXT,
    [0x52] = 0x2D, [0x53] = VK_DELETE, [0x57] = 0x7A, [0x58] = 0x7B,
};

int kbd_vk(int sc)
{
    return (sc > 0 && sc < (int)sizeof vk_of) ? vk_of[sc] : 0;
}

char kbd_char(int vk, bool shift)
{
    if (vk >= 'A' && vk <= 'Z')
        return shift ? (char)vk : (char)(vk + 32);
    if (vk >= '0' && vk <= '9')
        return shift ? ")!@#$%^&*("[vk - '0'] : (char)vk;
    switch (vk) {
    case VK_SPACE:  return ' ';
    case VK_BACK:   return 8;
    case VK_TAB:    return 9;
    case VK_RETURN: return 13;
    case VK_ESCAPE: return 27;
    case OEM_1:      return shift ? ':' : ';';
    case OEM_PLUS:   return shift ? '+' : '=';
    case OEM_COMMA:  return shift ? '<' : ',';
    case OEM_MINUS:  return shift ? '_' : '-';
    case OEM_PERIOD: return shift ? '>' : '.';
    case OEM_2:      return shift ? '?' : '/';
    case OEM_3:      return shift ? '~' : '`';
    case OEM_4:      return shift ? '{' : '[';
    case OEM_5:      return shift ? '|' : '\\';
    case OEM_6:      return shift ? '}' : ']';
    case OEM_7:      return shift ? '"' : '\'';
    case 0x6A:       return '*';
    }
    return 0;
}
