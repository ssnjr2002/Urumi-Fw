/**
 * Tests for wire/format/status — STATUS_RSP (0xA7, 30B), the operational
 * enums, MachineStatus, parseGetstate / parseStatusRsp / packStatusRsp.
 *
 * The reference byte sequences below are the actual output of
 * host.protocol.packets.pack_status_rsp — captured from the Python, so a
 * passing round-trip here means the TS packer produces byte-identical
 * frames and the parser reads them back exactly as Python does.
 */

import { describe, it, expect } from "vitest";
import {
    MachineState,
    AlarmReason,
    RunningReason,
    AXIS_BITS,
    axisMask,
    MachineStatus,
    parseGetstate,
    parseStatusRsp,
    packStatusRsp,
} from "../../../src/wire/format/status.js";
import { MAGIC_STATUS_RSP, STATUS_RSP_SIZE } from "../../../src/wire/format/constants.js";

// ── reference vectors from Python host.protocol.packets ───────────────────────
// pack_status_rsp(state=1, axes_enabled=0x0f, axes_homed=0x0f, alarm=0,
//   running=0, buf_count=5, pos=[100,-200,0,0], expected_seq=42, queued_us=1234567)
const REF_RUNNING = new Uint8Array([
    167, 1, 15, 15, 0, 0, 5, 0, 100, 0, 0, 0, 56, 255, 255, 255,
    0, 0, 0, 0, 0, 0, 0, 0, 42, 135, 214, 18, 0, 219,
]);
// pack_status_rsp(state=0, axes_enabled=0, axes_homed=0, alarm=0, running=0,
//   buf_count=0, pos=[0,0,0,0], expected_seq=0, queued_us=0)
const REF_IDLE = new Uint8Array([
    167, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 191,
]);

describe("wire/format/status: enums", () => {
    it("MachineState mirrors the firmware values", () => {
        expect(MachineState.IDLE).toBe(0);
        expect(MachineState.RUNNING).toBe(1);
        expect(MachineState.ESTOP).toBe(2);
        expect(MachineState.ALARM).toBe(3);
        expect(MachineState.PAUSED).toBe(4);
        expect(MachineState.HOMING).toBe(5);
    });

    it("AlarmReason mirrors the firmware values", () => {
        expect(AlarmReason.NONE).toBe(0);
        expect(AlarmReason.ESTOP).toBe(1);
        expect(AlarmReason.CONFIG).toBe(2);
        expect(AlarmReason.SOFT_LIMIT).toBe(3);
        expect(AlarmReason.HOMING_FAIL).toBe(4);
    });

    it("RunningReason mirrors the firmware values (incl. ABORT_DECEL §4.5)", () => {
        expect(RunningReason.JOB).toBe(0);
        expect(RunningReason.JOG).toBe(1);
        expect(RunningReason.ABORT_DECEL).toBe(2);
    });
});

describe("wire/format/status: axisMask", () => {
    it("'xyza' -> 0x0f", () => {
        expect(axisMask("xyza")).toBe(0x0f);
    });
    it("'xy' -> 0b0011", () => {
        expect(axisMask("xy")).toBe(0x03);
    });
    it("unknown letters contribute no bits", () => {
        expect(axisMask("xp")).toBe(AXIS_BITS.x);
    });
    it("empty -> 0", () => {
        expect(axisMask("")).toBe(0);
    });
});

describe("wire/format/status: packStatusRsp", () => {
    it("produces a 30-byte frame", () => {
        const pkt = packStatusRsp({
            state: 0,
            axesEnabled: 0,
            axesHomed: 0,
            alarm: 0,
            running: 0,
        });
        expect(pkt.length).toBe(STATUS_RSP_SIZE);
    });

    it("is byte-identical to Python pack_status_rsp for the RUNNING vector", () => {
        const pkt = packStatusRsp({
            state: MachineState.RUNNING,
            axesEnabled: 0x0f,
            axesHomed: 0x0f,
            alarm: AlarmReason.NONE,
            running: RunningReason.JOB,
            bufCount: 5,
            pos: [100, -200, 0, 0],
            expectedSeq: 42,
            queuedUs: 1234567,
        });
        expect([...pkt]).toEqual([...REF_RUNNING]);
    });

    it("is byte-identical to Python pack_status_rsp for the all-zero IDLE vector", () => {
        const pkt = packStatusRsp({
            state: MachineState.IDLE,
            axesEnabled: 0,
            axesHomed: 0,
            alarm: AlarmReason.NONE,
            running: RunningReason.JOB,
        });
        expect([...pkt]).toEqual([...REF_IDLE]);
    });

    it("packs negative pos values as two's-complement int32 LE", () => {
        const pkt = packStatusRsp({
            state: 0,
            axesEnabled: 0,
            axesHomed: 0,
            alarm: 0,
            running: 0,
            pos: [-1, 0, 0, 0],
        });
        expect(pkt[8]).toBe(0xff);
        expect(pkt[9]).toBe(0xff);
        expect(pkt[10]).toBe(0xff);
        expect(pkt[11]).toBe(0xff);
    });
});

describe("wire/format/status: parseStatusRsp", () => {
    it("round-trips the RUNNING vector exactly", () => {
        const st = parseStatusRsp(REF_RUNNING);
        expect(st.state).toBe(MachineState.RUNNING);
        expect(st.axesEnabled).toBe(0x0f);
        expect(st.axesHomed).toBe(0x0f);
        expect(st.alarm).toBe(AlarmReason.NONE);
        expect(st.running).toBe(RunningReason.JOB);
        expect(st.bufCount).toBe(5);
        expect(st.pos).toEqual([100, -200, 0, 0]);
        expect(st.expectedSeq).toBe(42);
        expect(st.queuedUs).toBe(1234567);
    });

    it("round-trips the IDLE vector exactly", () => {
        const st = parseStatusRsp(REF_IDLE);
        expect(st.state).toBe(MachineState.IDLE);
        expect(st.bufCount).toBe(0);
        expect(st.pos).toEqual([0, 0, 0, 0]);
        expect(st.expectedSeq).toBe(0);
        expect(st.queuedUs).toBe(0);
    });

    it("rejects a v1 (0xA6) frame with the firmware-reflash message", () => {
        const v1 = new Uint8Array(STATUS_RSP_SIZE);
        v1[0] = 0xa6;
        expect(() => parseStatusRsp(v1)).toThrow(/retired 9-byte STATUS_RSP/);
        expect(() => parseStatusRsp(v1)).toThrow(/Reflash/);
    });

    it("rejects an unknown magic", () => {
        const bad = packStatusRsp({ state: 0, axesEnabled: 0, axesHomed: 0, alarm: 0, running: 0 });
        bad[0] = 0xcf;
        expect(() => parseStatusRsp(bad)).toThrow(/Bad magic: 0xcf/);
    });

    it("rejects a wrong-size buffer", () => {
        const short = new Uint8Array(9);
        short[0] = MAGIC_STATUS_RSP;
        expect(() => parseStatusRsp(short)).toThrow(/Expected 30 bytes, got 9/);
    });

    it("rejects a CRC mismatch", () => {
        const pkt = packStatusRsp({ state: 0, axesEnabled: 0, axesHomed: 0, alarm: 0, running: 0 });
        pkt[10] = pkt[10]! ^ 0xff; // flip a payload byte
        expect(() => parseStatusRsp(pkt)).toThrow(/CRC mismatch/);
    });

    it("falls back to IDLE on an out-of-range state byte (forward-compat)", () => {
        const pkt = packStatusRsp({ state: 0, axesEnabled: 0, axesHomed: 0, alarm: 0, running: 0 });
        // 7 is not a known MachineState — keep CRC valid by recomputing via pack
        const forged = packStatusRsp({
            state: 7,
            axesEnabled: 0,
            axesHomed: 0,
            alarm: 9, // also out of range
            running: 99, // also out of range
        });
        void pkt;
        const st = parseStatusRsp(forged);
        expect(st.state).toBe(MachineState.IDLE);
        expect(st.alarm).toBe(AlarmReason.NONE);
        expect(st.running).toBe(RunningReason.JOB);
    });
});

describe("wire/format/status: parseGetstate (text plane)", () => {
    it("parses a standard reply", () => {
        const st = parseGetstate("state=0 enabled=0x0f homed=0x0f alarm=0 running=0");
        expect(st.state).toBe(MachineState.IDLE);
        expect(st.axesEnabled).toBe(0x0f);
        expect(st.axesHomed).toBe(0x0f);
        expect(st.alarm).toBe(AlarmReason.NONE);
        expect(st.running).toBe(RunningReason.JOB);
    });

    it("leaves binary-only fields undefined (distinguishing text from real zero)", () => {
        const st = parseGetstate("state=0 enabled=0x0f homed=0x0f alarm=0 running=0");
        expect(st.bufCount).toBeUndefined();
        expect(st.pos).toBeUndefined();
        expect(st.expectedSeq).toBeUndefined();
        expect(st.queuedUs).toBeUndefined();
    });

    it("defaults enabled to 0 when absent", () => {
        const st = parseGetstate("state=1 homed=0x03");
        expect(st.state).toBe(MachineState.RUNNING);
        expect(st.axesEnabled).toBe(0);
        expect(st.axesHomed).toBe(0x03);
    });

    it("tolerates unknown trailing tokens (forward-compat)", () => {
        const st = parseGetstate("state=4 homed=0x0f future=7");
        expect(st.state).toBe(MachineState.PAUSED);
    });

    it("falls back to NONE/JOB on unknown enum values", () => {
        const st = parseGetstate("state=42 homed=0x0f alarm=99 running=99");
        expect(st.state).toBe(MachineState.IDLE);
        expect(st.alarm).toBe(AlarmReason.NONE);
        expect(st.running).toBe(RunningReason.JOB);
    });

    it("throws on a non-status line", () => {
        expect(() => parseGetstate("pong")).toThrow(/not a getstate reply/);
    });

    it("throws on a line missing homed", () => {
        expect(() => parseGetstate("state=0 enabled=0x0f")).toThrow(/not a getstate reply/);
    });
});

describe("wire/format/status: MachineStatus helpers", () => {
    // axesHomed=0x06 (Y+Z homed, X+A not), axesEnabled=0x0f (all energised)
    const st = new MachineStatus(
        MachineState.IDLE,
        0x06,
        0x0f,
        AlarmReason.NONE,
        RunningReason.JOB,
    );

    it("homed(axis) reads the homed mask", () => {
        expect(st.homed("x")).toBe(false); // bit0 not in 0x06
        expect(st.homed("y")).toBe(true); // bit1 in 0x06
        expect(st.homed("z")).toBe(true); // bit2 in 0x06
        expect(st.homed("a")).toBe(false); // bit3 not in 0x06
    });

    it("enabled(axis) reads the enabled mask", () => {
        expect(st.enabled("x")).toBe(true); // bit0 in 0x0f
        expect(st.enabled("a")).toBe(true); // bit3 in 0x0f
    });

    it("allHomed requires every bit", () => {
        expect(st.allHomed(0x06)).toBe(true);
        expect(st.allHomed(0x0f)).toBe(false); // x + a missing from 0x06
    });

    it("allEnabled requires every bit", () => {
        expect(st.allEnabled(0x0f)).toBe(true);
        expect(st.allEnabled(0x10)).toBe(false); // bit4 not in 0x0f
    });
});