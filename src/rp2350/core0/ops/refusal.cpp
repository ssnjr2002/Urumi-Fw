#include <stdarg.h>
#include <stdio.h>
#include "refusal.h"

static char refusal[48];

const char* refuse(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(refusal, sizeof refusal, fmt, ap);
    va_end(ap);
    return refusal;
}
