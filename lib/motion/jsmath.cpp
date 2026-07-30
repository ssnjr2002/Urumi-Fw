/**
 * jsmath.cpp — the transcendentals, owned rather than borrowed.
 *
 * WHY THIS FILE EXISTS
 *
 * The platform libm is not a shared reference. Measured over 200,000 inputs
 * spanning the magnitudes the planner works in, mingw's libm disagrees with
 * V8 on:
 *
 *     atan2   35,247 / 200,000   (17.6%)   max 1 ULP
 *     acos    15,329 / 200,000   ( 7.7%)   max 1 ULP
 *     hypot        44 / 200,000   ( 0.02%)  max 1 ULP
 *     cos       5,596 / 200,000   ( 2.8%)   max 26 ULP
 *     sqrt          0             (IEEE-754 mandates correct rounding)
 *
 * cos is the outlier and worth reading twice: 26 ULP is not a last-place
 * rounding difference, it is a WORSE ANSWER. The x87 fcos reduces its argument
 * against a 66-bit approximation of pi, so accuracy degrades with magnitude;
 * fdlibm reduces against a multi-word pi and stays correct. That is not a
 * reason to prefer fdlibm here — V8 is the reference because the TypeScript is
 * the reference — but it does mean this one was never merely cosmetic.
 *
 * The rest are 1 ULP. That is not a testing inconvenience — it means the same
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
 *   - cos: fdlibm, argument reduction plus the even kernel polynomial.
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
#include <limits>

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

/** Replace the high 32 bits, fdlibm's SET_HIGH_WORD. */
inline void setHiWord(double& x, int32_t hi) {
    uint64_t b;
    std::memcpy(&b, &x, sizeof b);
    b = (b & 0x00000000ffffffffULL) | (static_cast<uint64_t>(static_cast<uint32_t>(hi)) << 32);
    std::memcpy(&x, &b, sizeof x);
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

// ── fdlibm cos ───────────────────────────────────────────────────────────────

const double C1 = 4.16666666666666019037e-02;
const double C2 = -1.38888888888741095749e-03;
const double C3 = 2.48015872894767294178e-05;
const double C4 = -2.75573143513906633035e-07;
const double C5 = 2.08757232129817482790e-09;
const double C6 = -1.13596475577881948265e-11;

const double S1 = -1.66666666666666324348e-01;
const double S2 = 8.33333333332248946124e-03;
const double S3 = -1.98412698298579493134e-04;
const double S4 = 2.75573137070700676789e-06;
const double S5 = -2.50507602534068634195e-08;
const double S6 = 1.58969099521155010221e-10;

/** __kernel_cos, valid for |x| <= pi/4; y is the low half of the reduced arg. */
double kernelCos(double x, double y) {
    const int32_t ix = hiWord(x) & 0x7fffffff;
    if (ix < 0x3e400000) {                    // |x| < 2^-27
        if (static_cast<int>(x) == 0) return one;
    }
    const double z = x * x;
    const double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ix < 0x3FD33333) return one - (0.5 * z - (z * r - x * y)); // |x| < 0.3

    // The 1 - 0.5*z split loses bits near pi/4, so fdlibm shifts the split
    // point. `qx` is deliberately built by bit-twiddling, not arithmetic: it
    // must be exactly representable for `a - iz` to be error-free.
    double qx;
    if (ix > 0x3fe90000) {                    // |x| > 0.78125
        qx = 0.28125;
    } else {
        qx = 0.0;
        setHiWord(qx, ix - 0x00200000);       // x/4
    }
    const double iz = 0.5 * z - qx;
    const double a = one - qx;
    return a - (iz - (z * r - x * y));
}

/** __kernel_sin, valid for |x| <= pi/4. iy != 0 means y is a real correction. */
double kernelSin(double x, double y, int iy) {
    const int32_t ix = hiWord(x) & 0x7fffffff;
    if (ix < 0x3e400000) {                    // |x| < 2^-27
        if (static_cast<int>(x) == 0) return x;
    }
    const double z = x * x;
    const double v = z * x;
    const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0) return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

const double invpio2 = 6.36619772367581382433e-01;
const double pio2_1 = 1.57079632673412561417e+00;
const double pio2_1t = 6.07710050650619224932e-11;
const double pio2_2 = 6.07710050630396597660e-11;
const double pio2_2t = 2.02226624879595063154e-21;
const double pio2_3 = 2.02226624871116645580e-21;
const double pio2_3t = 8.47842766036889956997e-32;

const int32_t npio2_hw[] = {
    0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C,
    0x4025FDBB, 0x402921FB, 0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C,
    0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB, 0x403AB41B, 0x403C463A,
    0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
    0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB,
    0x404858EB, 0x404921FB,
};

/**
 * __ieee754_rem_pio2, MEDIUM RANGE ONLY: |x| < 2^20 * pi/2 ~ 1.65e6.
 *
 * fdlibm's full version falls through to __kernel_rem_pio2, a 150-line
 * multi-precision reduction against a table of 2/pi. That path is not ported,
 * because the only caller is the junction cap, whose argument is
 * (|turnDeg| * pi/180) / 2 with |turnDeg| <= 180 — i.e. [0, pi/2], which does
 * not even leave the first branch. Rather than let an unported branch return a
 * quietly wrong number, the huge case returns NaN: if a future caller reaches
 * it the differential test fails loudly and this comment is the fix list.
 */
int remPio2(double x, double* y) {
    const int32_t hx = hiWord(x);
    const int32_t ix = hx & 0x7fffffff;

    if (ix <= 0x3fe921fb) { // |x| <= pi/4, no reduction
        y[0] = x;
        y[1] = 0;
        return 0;
    }
    if (ix < 0x4002d97c) { // |x| < 3pi/4, one round of reduction
        double z;
        if (hx > 0) {
            z = x - pio2_1;
            if (ix != 0x3ff921fb) { // 33+53 bit pi is good enough
                y[0] = z - pio2_1t;
                y[1] = (z - y[0]) - pio2_1t;
            } else { // near pi/2, use the next two terms
                z -= pio2_2;
                y[0] = z - pio2_2t;
                y[1] = (z - y[0]) - pio2_2t;
            }
            return 1;
        }
        z = x + pio2_1;
        if (ix != 0x3ff921fb) {
            y[0] = z + pio2_1t;
            y[1] = (z - y[0]) + pio2_1t;
        } else {
            z += pio2_2;
            y[0] = z + pio2_2t;
            y[1] = (z - y[0]) + pio2_2t;
        }
        return -1;
    }
    if (ix <= 0x413921fb) { // |x| < 2^20 * pi/2
        double t = std::fabs(x);
        const int32_t n = static_cast<int32_t>(t * invpio2 + 0.5);
        const double fn = static_cast<double>(n);
        double r = t - fn * pio2_1;
        double w = fn * pio2_1t;
        // Cancellation check: if the first reduction lost too many bits, redo
        // it carrying the next term, and again after that. The npio2_hw guard
        // skips the check when x is nowhere near a multiple of pi/2.
        if (n < 32 && ix != npio2_hw[n - 1]) {
            y[0] = r - w;
        } else {
            const int32_t j = ix >> 20;
            y[0] = r - w;
            int32_t i = j - ((hiWord(y[0]) >> 20) & 0x7ff);
            if (i > 16) { // 2nd iteration, 24+24+24 bit pi
                t = r;
                w = fn * pio2_2;
                r = t - w;
                w = fn * pio2_2t - ((t - r) - w);
                y[0] = r - w;
                i = j - ((hiWord(y[0]) >> 20) & 0x7ff);
                if (i > 49) { // 3rd iteration, 72 bits is the last resort
                    t = r;
                    w = fn * pio2_3;
                    r = t - w;
                    w = fn * pio2_3t - ((t - r) - w);
                    y[0] = r - w;
                }
            }
        }
        y[1] = (r - y[0]) - w;
        if (hx < 0) {
            y[0] = -y[0];
            y[1] = -y[1];
            return -n;
        }
        return n;
    }

    // Not ported (see above) — and NaN rather than a plausible wrong answer.
    y[0] = y[1] = std::numeric_limits<double>::quiet_NaN();
    return 0;
}

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

double jsCos(double x) {
    const int32_t ix = hiWord(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) return kernelCos(x, 0.0); // |x| <= pi/4
    if (ix >= 0x7ff00000) return x - x;             // inf or NaN

    double y[2];
    const int n = remPio2(x, y);
    switch (n & 3) {
        case 0: return kernelCos(y[0], y[1]);
        case 1: return -kernelSin(y[0], y[1], 1);
        case 2: return -kernelCos(y[0], y[1]);
        default: return kernelSin(y[0], y[1], 1);
    }
}

} // namespace motion
