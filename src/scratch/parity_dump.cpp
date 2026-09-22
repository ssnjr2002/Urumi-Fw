#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdio>
#include <cstring>
#include <cstdint>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#endif

#include "motion/geometry.h"
#include "motion/plan.h"
#include "motion/discretize.h"
#include "support/curves.h"
#include "support/machine.h"

using motion::MicroSegment;

void dumpSegments(const std::vector<MicroSegment>& segs) {
    for (const auto& s : segs) {
        uint8_t buf[44];
        memcpy(buf + 0, &s.dx, 8);
        memcpy(buf + 8, &s.dy, 8);
        memcpy(buf + 16, &s.dz, 8);
        memcpy(buf + 24, &s.da, 8);
        memcpy(buf + 32, &s.interval, 8);
        memcpy(buf + 40, &s.flags, 4);

#ifdef ARDUINO
        Serial.write(buf, 44);
#else
        fwrite(buf, 1, 44, stdout);
#endif
    }
}

void runAndDump() {
    for (const curves::Case& c : curves::casesWithCusp()) {
        const std::vector<MicroSegment> segs = machine::prep({*c.second}, machine::knife());
        dumpSegments(segs);
    }
}

#ifdef ARDUINO

void setup() {
    Serial.begin(115200);
    while (!Serial) {
        delay(10);
    }
    
    // Wait for a trigger character from the host script so we don't dump prematurely
    while (Serial.available() == 0) {
        delay(10);
    }
    Serial.read(); // consume trigger
    
    runAndDump();
    Serial.flush();
}

void loop() {
    delay(100);
}

#else

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    runAndDump();
    return 0;
}

#endif
