/**
 * bits.h — reading generated test data from test/data.
 */

#ifndef TEST_BITS_H
#define TEST_BITS_H

#include <fstream>
#include <string>

namespace testbits {

/**
 * Open a file by name from test/data.
 *
 * PlatformIO's native test runner does not contract a working directory, so
 * walk up from wherever it started rather than assuming one.
 *
 * Test with is_open(): a default-constructed ifstream has no error flags set,
 * so good() is true for a stream that was never opened. The failbit below makes
 * a miss read as false either way.
 */
inline std::ifstream openRef(const std::string& name) {
    static const char* prefixes[] = {"", "../", "../../", "../../../", "../../../../"};
    for (const char* p : prefixes) {
        std::ifstream f(std::string(p) + "test/data/" + name);
        if (f.is_open()) return f;
    }
    std::ifstream dead;
    dead.setstate(std::ios::failbit);
    return dead;
}

/** The command that regenerates every file in test/data. */
inline const char* REGEN_ALL =
    "cd web && GEN_CPP_REF=1 npx vitest run test/port";

} // namespace testbits

#endif // TEST_BITS_H
