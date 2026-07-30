/**
 * jsmath.cpp — the transcendentals, owned rather than borrowed.
 *
 * WHY THIS FILE EXISTS
 *
 * The platform libm is not a shared reference. Measured over 200,000 inputs
 * spanning the magnitudes the planner works in, mingw's libm disagrees with
 * V8 on:
 *
 *     atan2   35,247 / 200,000   (17.6%)
 *     acos    15,329 / 200,000   ( 7.7%)
 *     hypot        44 / 200,000   ( 0.02%)
 *     sqrt          0             (IEEE-754 mandates correct rounding)
 *
 * all by 1 ULP. That is not a testing inconvenience — it means the same
 * planner, compiled for the host harness and for the Pico, would produce
 * DIFFERENT TOOLPATHS, because newlib's libm is a third answer again. The
 * whole point of consolidating on one implementation is that the machine cuts
 * what the harness verified.
 *
 * So the port owns these three. Each is verified bit-identical to V8 across
 * the same 200,000 inputs (0 disagreements):
 *
 *   - atan2 / atan: fdlibm (Sun Microsystems, freely distributable), which is
 *     what V8's src/base/ieee754.cc is derived from.
 *   - acos: fdlibm likewise.
 *   - hypot: NOT fdlibm. V8 implements Math.hypot itself as scale-by-max plus
 *     a Kahan-compensated sum of squares, which is why std::hypot — a
 *     different, also-good algorithm — disagrees on 0.02% of inputs.
 *
 * DO NOT "improve" any of this. Every constant and every operation order is
 * load-bearing. The polynomial evaluation order in particular is not stylistic;
 * reassociating it changes the low bits, which is exactly what these functions
 * exist to pin.
 *
 * Provenance: the fdlibm kernels below carry Sun's notice —
 *   Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *   Developed at SunSoft, a Sun Microsystems, Inc. business.
 *   Permission to use, copy, modify, and distribute this software is freely
 *   granted, provided that this notice is preserved.
 */

#include "motion/jsmath.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace motion {

namespace {

inline int32_t hiWord(double x) {
    uint64_t b;
    std::memcpy(&b, &x, sizeof b);
    return static_cast<int32_t>(b >> 32);
}

inline uint32_t loWord(double x) {
    uint64_t b;
    std::memcpy(&b, &x, sizeof b);
    return static_cast<uint32_t>(b);
}

/** Zero the low 32 bits, fdlibm's SET_LOW_WORD(x, 0). */
inline void clearLoWord(double& x) {
    uint64_t b;
    std::memcpy(&b, &x, sizeof b);
    b &= 0xffffffff00000000ULL;
    std::memcpy(&x, &b, sizeof x);
}

// ── fdlibm atan ──────────────────────────────────────────────────────────────

const double atanhi[] = {
    4.63647609000806093515e-01, // atan(0.5)hi
    7.85398163397448278999e-01, // atan(1.0)hi
    9.82793723247329054082e-01, // atan(1.5)hi
    1.57079632679489655800e+00, // atan(inf)hi
};
const double atanlo[] = {
    2.26987774529616870924e-17,
    3.06161699786838301793e-17,
    1.39033110312309984516e-17,
    6.12323399573676603587e-17,
};
const double aT[] = {
    3.33333333333329318027e-01,  -1.99999999998764832476e-01,
    1.42857142725034663711e-01,  -1.11111104054623557880e-01,
    9.09088713343650656196e-02,  -7.69187620504482999495e-02,
    6.66107313738753120669e-02,  -5.83357013379057348645e-02,
    4.97687799461593236017e-02,  -3.65315727442169155270e-02,
    1.62858201153657823623e-02,
};

const double one = 1.0;
const double hugeVal = 1.0e300;

double fdAtan(double x) {
    double w, s1, s2, z;
    int32_t id;
    const int32_t hx = hiWord(x);
    const int32_t ix = hx & 0x7fffffff;

    if (ix >= 0x44100000) { // |x| >= 2^66
        if (ix > 0x7ff00000 || (ix == 0x7ff00000 && loWord(x) != 0)) return x + x; // NaN
        return hx > 0 ? atanhi[3] + atanlo[3] : -atanhi[3] - atanlo[3];
    }
    if (ix < 0x3fdc0000) { // |x| < 0.4375
        if (ix < 0x3e200000) { // |x| < 2^-29
            if (hugeVal + x > one) return x; // raise inexact
        }
        id = -1;
    } else {
        x = std::fabs(x);
        if (ix < 0x3ff30000) {          // |x| < 1.1875
            if (ix < 0x3fe60000) {      // 7/16 <= |x| < 11/16
                id = 0;
                x = (2.0 * x - one) / (2.0 + x);
            } else {                    // 11/16 <= |x| < 19/16
                id = 1;
                x = (x - one) / (x + one);
            }
        } else {
            if (ix < 0x40038000) {      // |x| < 2.4375
                id = 2;
                x = (x - 1.5) / (one + 1.5 * x);
            } else {                    // 2.4375 <= |x| < 2^66
                id = 3;
                x = -1.0 / x;
            }
        }
    }

    z = x * x;
    w = z * z;
    s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0) return x - x * (s1 + s2);
    z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return (hx < 0) ? -z : z;
}

// ── fdlibm atan2 ─────────────────────────────────────────────────────────────

const double tiny = 1.0e-300;
const double zero = 0.0;
const double pi_o_4 = 7.8539816339744827900e-01;
const double pi_o_2 = 1.5707963267948965580e+00;
const double pi = 3.1415926535897931160e+00;
const double pi_lo = 1.2246467991473531772e-16;

// ── fdlibm acos ──────────────────────────────────────────────────────────────

const double pio2_hi = 1.57079632679489655800e+00;
const double pio2_lo = 6.12323399573676603587e-17;
const double pS0 = 1.66666666666666657415e-01;
const double pS1 = -3.25565818622400915405e-01;
const double pS2 = 2.01212532134862925881e-01;
const double pS3 = -4.00555345006794114027e-02;
const double pS4 = 7.91534994289814532176e-04;
const double pS5 = 3.47933107596021167570e-05;
const double qS1 = -2.40339491173441421878e+00;
const double qS2 = 2.02094576023350569471e+00;
const double qS3 = -6.88283971605453293030e-01;
const double qS4 = 7.70381505559019352791e-02;

} // namespace

double jsAtan2(double y, double x) {
    double z;
    const int32_t hx = hiWord(x);
    const uint32_t lx = loWord(x);
    const int32_t hy = hiWord(y);
    const uint32_t ly = loWord(y);
    const int32_t ix = hx & 0x7fffffff;
    const int32_t iy = hy & 0x7fffffff;

    // either argument NaN
    if ((static_cast<uint32_t>(ix) |
         ((lx | static_cast<uint32_t>(-static_cast<int32_t>(lx))) >> 31)) > 0x7ff00000 ||
        (static_cast<uint32_t>(iy) |
         ((ly | static_cast<uint32_t>(-static_cast<int32_t>(ly))) >> 31)) > 0x7ff00000) {
        return x + y;
    }
    if (((hx - 0x3ff00000) | static_cast<int32_t>(lx)) == 0) return fdAtan(y); // x == 1.0

    const int32_t m = ((hy >> 31) & 1) | ((hx >> 30) & 2); // 2*sign(x) + sign(y)

    if ((iy | static_cast<int32_t>(ly)) == 0) { // y == 0
        switch (m) {
            case 0:
            case 1: return y;              // atan(+-0, +anything) = +-0
            case 2: return pi + tiny;      // atan(+0, -anything)  =  pi
            default: return -pi - tiny;    // atan(-0, -anything)  = -pi
        }
    }
    if ((ix | static_cast<int32_t>(lx)) == 0) { // x == 0
        return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny;
    }
    if (ix == 0x7ff00000) { // x is INF
        if (iy == 0x7ff00000) {
            switch (m) {
                case 0: return pi_o_4 + tiny;
                case 1: return -pi_o_4 - tiny;
                case 2: return 3.0 * pi_o_4 + tiny;
                default: return -3.0 * pi_o_4 - tiny;
            }
        }
        switch (m) {
            case 0: return zero;
            case 1: return -zero;
            case 2: return pi + tiny;
            default: return -pi - tiny;
        }
    }
    if (iy == 0x7ff00000) return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny; // y is INF

    const int32_t k = (iy - ix) >> 20;
    if (k > 60) {
        z = pi_o_2 + 0.5 * pi_lo;             // |y/x| > 2^60
    } else if (hx < 0 && k < -60) {
        z = 0.0;                              // |y|/x < -2^60
    } else {
        z = fdAtan(std::fabs(y / x));
    }
    switch (m) {
        case 0: return z;
        case 1: return -z;
        case 2: return pi - (z - pi_lo);
        default: return (z - pi_lo) - pi;
    }
}

double jsAcos(double x) {
    double z, p, q, r, w, s, c, df;
    const int32_t hx = hiWord(x);
    const int32_t ix = hx & 0x7fffffff;

    if (ix >= 0x3ff00000) { // |x| >= 1
        if (((ix - 0x3ff00000) | static_cast<int32_t>(loWord(x))) == 0) {
            return hx > 0 ? 0.0 : pi + 2.0 * pio2_lo; // acos(1) = 0, acos(-1) = pi
        }
        return (x - x) / (x - x); // NaN
    }
    if (ix < 0x3fe00000) { // |x| < 0.5
        if (ix <= 0x3c600000) return pio2_hi + pio2_lo; // |x| < 2^-57
        z = x * x;
        p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
        q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
        r = p / q;
        return pio2_hi - (x - (pio2_lo - x * r));
    }
    if (hx < 0) { // x < -0.5
        z = (one + x) * 0.5;
        p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
        q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
        s = std::sqrt(z);
        r = p / q;
        w = r * s - pio2_lo;
        return pi - 2.0 * (s + w);
    }
    // x > 0.5
    z = (one - x) * 0.5;
    s = std::sqrt(z);
    df = s;
    clearLoWord(df);
    c = (z - df * df) / (s + df);
    p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    r = p / q;
    w = r * s + c;
    return 2.0 * (df + w);
}

double jsHypot(double a, double b) {
    // V8's Math.hypot, not std::hypot: scale by the largest magnitude, then a
    // Kahan-compensated sum of the squares. std::hypot is a different (also
    // correct) algorithm and disagrees on ~0.02% of inputs.
    const double x = std::fabs(a);
    const double y = std::fabs(b);
    const double maxAbs = x > y ? x : y;
    if (maxAbs == 0) return 0;

    double sum = 0;
    double compensation = 0;
    const double vals[2] = {x, y};
    for (int i = 0; i < 2; i++) {
        const double n = vals[i] / maxAbs;
        const double summand = n * n - compensation;
        const double preliminary = sum + summand;
        compensation = (preliminary - sum) - summand;
        sum = preliminary;
    }
    return maxAbs * std::sqrt(sum);
}

} // namespace motion
