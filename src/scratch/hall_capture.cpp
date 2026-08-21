// hall_capture.cpp — standalone A-axis Hall capture rig. NOT part of the node
// firmware, NOT on the RS485 bus, and deliberately throwaway.
//
// Purpose: collect step-tagged Hall samples from the rotary A axis so the index
// centre-finding algorithm can be chosen from real data on a PC, instead of
// guessed at in firmware. Once an estimator wins, it gets written properly
// against the node core and THIS FILE GETS DELETED. Nothing here is a design
// commitment — not the buffering (there is none), not the transfer format, not
// the command set.
//
// Why standalone: the only thing firmware uniquely provides is the position
// tag — sampling the ADC synchronously with steps, which only the thing issuing
// steps can do. Everything downstream (baseline, threshold, centroid, whatever
// wins) is arithmetic that can happen anywhere. Off-bus, samples stream out
// live over USART as they are acquired, which removes buffer sizing, chunked
// transfers, and any "which window do I capture" trigger decision. Capture
// twenty revolutions straight through and sort it out in Python.
//
// Time-sampled logging (e.g. free-running ADC dumped over USB) does NOT work
// for this: it has to be resampled into the angle domain afterwards, and that
// resampling is acutely sensitive to timing jitter — exactly the axis being
// measured. One sample per step sidesteps it rather than correcting it.
//
// Wiring assumptions (db_node4, AVR128DB32, DRV8825):
//   PD1  A1324 Hall analog output  (AIN1)
//   PD4  STEP   PD5 DIR   PD6 ENABLE (active LOW)
//   PA4/PA6/PA7  DRV8825 M0/M1/M2 microstep select
//   PD2  laser gate on this board (-DNODE_HAS_LASER) — held LOW here, see below
//
// Serial protocol (fixed-width ASCII so per-sample cost is constant — variable
// width would jitter the step interval and smear the speed tests):
//   > r <interval_us> <steps> [preroll]   stream one sample per step; `preroll`
//                                         steps are taken but not emitted, so
//                                         the capture starts at settled speed
//   > d <0|1>                   set direction for subsequent runs
//   > e <0|1>                   driver enable/disable
//   > ?                         status
// Output:
//   # BEGIN interval_us=1000 steps=128000 dir=0 micro=32 accbits=13
//   000000,04091
//   000001,04088
//   # END n=128000
#include <Arduino.h>

#ifndef CAP_SERIAL
#define CAP_SERIAL Serial1          // AVR-DB: USART1 on PC0/PC1 (USART2 is RS485)
#endif
#ifndef CAP_BAUD
#define CAP_BAUD 500000
#endif

// Microstepping. Defaults to 32 to match db_node4's DRV_MICROSTEPPING, so step
// counts captured here are directly comparable to what the real firmware emits.
#ifndef CAP_MICROSTEPPING
#define CAP_MICROSTEPPING 32
#endif

// ADC accumulation. DxCore's analogReadEnh() oversamples in hardware (the Dx
// ADC's SAMPNUM) and rescales to the requested resolution — free noise
// reduction for the cost of extra conversion time. 13 bits = 4x accumulation.
// The averaging is over TIME, so at speed it smears slightly over ANGLE: at
// 1000 steps/s that is well under a tenth of a step, i.e. irrelevant. Raise it
// if the captures look noisy; drop to 12 (no accumulation) if it costs too much
// time at the fastest sweep.
#ifndef CAP_ADC_BITS
#define CAP_ADC_BITS 13
#endif

#define HALL_PIN   PIN_PD1
#define STEP_PIN   PIN_PD4
#define DIR_PIN    PIN_PD5
#define EN_PIN     PIN_PD6
#define M0_PIN     PIN_PA4
#define M1_PIN     PIN_PA6
#define M2_PIN     PIN_PA7

// Direct port access for the step pulse — the pin toggle wants to be two stores,
// not two digitalWrite() calls, so the pulse width is what it says it is.
#define STEP_HIGH() (PORTD.OUTSET = PIN4_bm)
#define STEP_LOW()  (PORTD.OUTCLR = PIN4_bm)

// DRV8825: 1.9µs minimum step high and low. 3µs matches HAL_STEP_PULSE_CCMP in
// the real HAL, so the driver sees an identical pulse either way.
#define STEP_PULSE_US 3

static uint8_t g_dir = 0;
static bool    g_enabled = false;

// ─── Driver plumbing ────────────────────────────────────────────────────────

// DRV8825 microstep truth table (M2,M1,M0). Note 1/32 has three encodings;
// 101 is the canonical one.
static void setMicrostepping(uint8_t micro) {
    uint8_t m;
    switch (micro) {
        case 1:  m = 0b000; break;
        case 2:  m = 0b001; break;
        case 4:  m = 0b010; break;
        case 8:  m = 0b011; break;
        case 16: m = 0b100; break;
        default: m = 0b101; break;   // 32
    }
    digitalWrite(M0_PIN, (m >> 0) & 1);
    digitalWrite(M1_PIN, (m >> 1) & 1);
    digitalWrite(M2_PIN, (m >> 2) & 1);
}

static void driverEnable(bool on) {
    // DRV8825 enable is active LOW (matches HAL_MOTOR_ENABLE in stepper.h).
    digitalWrite(EN_PIN, on ? LOW : HIGH);
    g_enabled = on;
}

// ─── Capture ────────────────────────────────────────────────────────────────

// One sample per step, taken immediately AFTER the step pulse completes, so
// sample i is the field at the position reached by step i. No pipelining: the
// read is blocking, which at any sweep rate worth using costs a small fraction
// of the step interval and keeps the position tag exact rather than
// off-by-one-step.
//
// The loop paces off an absolute micros() deadline rather than delaying by a
// fixed amount, so the time spent converting and printing is absorbed instead
// of accumulating into a drifting, slowly-decreasing sweep speed.
static void runCapture(uint32_t intervalUs, uint32_t steps, uint32_t preroll) {
    if (!g_enabled) {
        CAP_SERIAL.println(F("# ERR driver disabled — 'e 1' first"));
        return;
    }

    digitalWrite(DIR_PIN, g_dir ? HIGH : LOW);
    delayMicroseconds(5);            // DIR setup, generous vs the DRV8825's 650ns

    CAP_SERIAL.print(F("# BEGIN interval_us="));
    CAP_SERIAL.print(intervalUs);
    CAP_SERIAL.print(F(" steps="));
    CAP_SERIAL.print(steps);
    CAP_SERIAL.print(F(" dir="));
    CAP_SERIAL.print(g_dir);
    CAP_SERIAL.print(F(" micro="));
    CAP_SERIAL.print(CAP_MICROSTEPPING);
    CAP_SERIAL.print(F(" accbits="));
    CAP_SERIAL.print(CAP_ADC_BITS);
    CAP_SERIAL.print(F(" preroll="));
    CAP_SERIAL.println(preroll);

    char line[16];
    uint32_t next = micros();

    // Pre-roll: step at the capture rate but emit nothing. The sweep otherwise
    // starts from standstill, so the opening samples carry the belt's start-up
    // transient — and the magnet sits on the LOAD side of that belt, which is
    // precisely the compliance the transient lives in. Pre-rolling means every
    // emitted sample is at settled constant speed.
    //
    // Set it to 0 deliberately when the goal is to MEASURE the transient rather
    // than exclude it — but then start the axis away from the magnet, or the
    // settling is superimposed on a dip and cannot be read.
    for (uint32_t i = 0; i < preroll; i++) {
        STEP_HIGH();
        delayMicroseconds(STEP_PULSE_US);
        STEP_LOW();
        (void)analogReadEnh(HALL_PIN, CAP_ADC_BITS);   // keep ADC cadence identical
        next += intervalUs;
        while ((int32_t)(micros() - next) < 0) { /* pace */ }
    }

    for (uint32_t i = 0; i < steps; i++) {
        STEP_HIGH();
        delayMicroseconds(STEP_PULSE_US);
        STEP_LOW();

        const int32_t v = analogReadEnh(HALL_PIN, CAP_ADC_BITS);

        // Fixed width on purpose: constant formatting cost per sample keeps the
        // step interval constant, which the settling-time and speed-dependence
        // analyses both depend on.
        snprintf(line, sizeof line, "%06lu,%05ld", (unsigned long)i, (long)v);
        CAP_SERIAL.println(line);

        next += intervalUs;
        while ((int32_t)(micros() - next) < 0) { /* pace */ }
    }

    CAP_SERIAL.print(F("# END n="));
    CAP_SERIAL.println(steps);
}

// ─── Command line ───────────────────────────────────────────────────────────

static void handleLine(char* s) {
    while (*s == ' ') s++;
    const char cmd = *s;
    if (!cmd) return;
    s++;

    switch (cmd) {
        case 'r': {
            char* end;
            const uint32_t iv = strtoul(s, &end, 10);
            if (end == s) { CAP_SERIAL.println(F("# ERR usage: r <interval_us> <steps> [preroll]")); return; }
            s = end;
            const uint32_t n = strtoul(s, &end, 10);
            if (end == s || iv == 0 || n == 0) {
                CAP_SERIAL.println(F("# ERR usage: r <interval_us> <steps> [preroll]")); return;
            }
            s = end;
            const uint32_t pre = strtoul(s, &end, 10);   // optional; 0 if absent
            runCapture(iv, n, (end == s) ? 0 : pre);
            break;
        }
        case 'd':
            g_dir = (strtoul(s, nullptr, 10) != 0) ? 1 : 0;
            CAP_SERIAL.print(F("# dir=")); CAP_SERIAL.println(g_dir);
            break;
        case 'e':
            driverEnable(strtoul(s, nullptr, 10) != 0);
            CAP_SERIAL.print(F("# en=")); CAP_SERIAL.println(g_enabled ? 1 : 0);
            break;
        case '?':
            CAP_SERIAL.print(F("# dir=")); CAP_SERIAL.print(g_dir);
            CAP_SERIAL.print(F(" en=")); CAP_SERIAL.print(g_enabled ? 1 : 0);
            CAP_SERIAL.print(F(" hall=")); CAP_SERIAL.println(analogReadEnh(HALL_PIN, CAP_ADC_BITS));
            break;
        default:
            CAP_SERIAL.println(F("# ERR cmds: r <interval_us> <steps> [preroll] | d <0|1> | e <0|1> | ?"));
            break;
    }
}

void setup() {
    CAP_SERIAL.begin(CAP_BAUD);

    pinMode(STEP_PIN, OUTPUT);  STEP_LOW();
    pinMode(DIR_PIN,  OUTPUT);  digitalWrite(DIR_PIN, LOW);
    pinMode(EN_PIN,   OUTPUT);
    pinMode(M0_PIN,   OUTPUT);
    pinMode(M1_PIN,   OUTPUT);
    pinMode(M2_PIN,   OUTPUT);

    // db_node4 is the laser-carrying board (-DNODE_HAS_LASER reclaims PD2 as a
    // digital laser gate). This scratch build has no laser logic at all, so pin
    // it LOW explicitly rather than leaving it floating — a scratch tool must
    // not be able to fire the laser.
    pinMode(PIN_PD2, OUTPUT);
    digitalWrite(PIN_PD2, LOW);

    setMicrostepping(CAP_MICROSTEPPING);
    driverEnable(false);

    // Reference the ADC to VDD, not the internal reference. The A1324 is
    // ratiometric — its quiescent output is 50% of ITS supply and its
    // sensitivity scales with supply too, so the output is a fixed FRACTION of
    // VDD rather than a fixed voltage. With VDD as the ADC reference that
    // fraction is what gets measured and supply variation cancels outright;
    // against a fixed internal reference, every supply wobble lands directly in
    // the reading — on a rail shared with stepper drivers. See docs/homing.md.
    analogReference(VDD);
    pinMode(HALL_PIN, INPUT);

    // Throw away the first conversions. The first read after a reference change
    // is taken before the reference has settled and comes back low — it showed
    // up in the first capture as a single sample ~90 counts BELOW the true
    // minimum of a real dip, which is exactly the sort of outlier that poisons a
    // global depth estimate and every fraction-of-depth threshold derived from it.
    for (uint8_t i = 0; i < 4; i++) (void)analogReadEnh(HALL_PIN, CAP_ADC_BITS);

    CAP_SERIAL.println();
    CAP_SERIAL.println(F("# hall_capture ready — r <interval_us> <steps> | d <0|1> | e <0|1> | ?"));
}

void loop() {
    static char buf[48];
    static uint8_t n = 0;

    while (CAP_SERIAL.available()) {
        const char c = (char)CAP_SERIAL.read();
        if (c == '\n' || c == '\r') {
            if (n) { buf[n] = '\0'; handleLine(buf); n = 0; }
        } else if (n < sizeof buf - 1) {
            buf[n++] = c;
        }
    }
}
