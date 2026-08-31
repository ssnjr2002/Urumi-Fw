#include "parse.h"
#include "../usb_protocol.h"   // BUS_ADDR_MAX
#include <stdlib.h>

bool parseState(const char* s) {
    if (*s == '1') return true;
    if ((s[0] == 'o' || s[0] == 'O') && (s[1] == 'n' || s[1] == 'N')) return true;
    return false;
}

uint8_t axisMask(const char* s) {
    if (!s || !*s) return 0x0F;
    uint8_t m = 0;
    for (; *s; s++) {
        switch (*s) {
            case 'x': case 'X': m |= 0x01; break;
            case 'y': case 'Y': m |= 0x02; break;
            case 'z': case 'Z': m |= 0x04; break;
            case 'a': case 'A': m |= 0x08; break;
        }
    }
    return m ? m : 0x0F;
}

uint8_t parseNode(const char* p, char** end) {
    char* e;
    unsigned long v = strtoul(p, &e, 10);
    if (end) *end = e;
    if (e == p) return 0;                        // no digits at all
    if (v < 1 || v > BUS_ADDR_MAX) return 0;
    return (uint8_t)v;
}
