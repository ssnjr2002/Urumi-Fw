/**
 * bits.h — bit-level plumbing shared by the differential tests.
 *
 * The port's criterion is bit-equality with the TypeScript, so these tests
 * compare IEEE-754 payloads, never values. There is deliberately no
 * "approximately equal" helper in this file: an epsilon would hide exactly the
 * transcription slips the differential tests exist to catch.
 */

#ifndef MOTION_TEST_BITS_H
#define MOTION_TEST_BITS_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace testbits {

inline double fromHex(const std::string& h) {
    uint64_t bits = 0;
    for (char ch : h) {
        bits <<= 4;
        bits |= static_cast<uint64_t>(ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10);
    }
    double d = 0;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

inline std::string toHex(double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof bits);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(bits));
    return std::string(buf);
}

/** Bit-level identity. NaN == NaN here, and +0 != -0 — both intended. */
inline bool sameBits(double a, double b) {
    uint64_t x = 0, y = 0;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return x == y;
}

/**
 * Open a reference file by name from test/data.
 *
 * PlatformIO's native test runner does not contract a working directory, so
 * walk up from wherever it started rather than assuming one.
 */
inline std::ifstream openRef(const std::string& name) {
    static const char* prefixes[] = {"", "../", "../../", "../../../", "../../../../"};
    for (const char* p : prefixes) {
        std::ifstream f(std::string(p) + "test/data/" + name);
        if (f.good()) return f;
    }
    return std::ifstream();
}

} // namespace testbits

#endif // MOTION_TEST_BITS_H
