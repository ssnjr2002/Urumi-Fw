/**
 * Tests for wire/format/status — STATUS_RSP (0xA7, 31B), the operational
 * enums, MachineStatus, the get reply / parseStatusRsp / packStatusRsp.
 *
 * The reference byte sequences below are the actual output of
 * host.protocol.packets.pack_status_rsp — captured from the Python, so a
 * passing round-trip here means the TS packer produces byte-identical
 * frames and the parser reads them back exactly as Python does.
 */

import { describe, it, expect } from "vitest";
import {
    MachineState,
    Fault,
    JobReason,
    AXIS_BITS,
    axisMask,
    MachineStatus,
    parseGetReply,
    getValue,
    statusFromGet,
    parseStatusRsp,
    packStatusRsp,
} from "../../../src/wire/format/status.js";
import { MAGIC_STATUS_RSP, STATUS_RSP_SIZE } from "../../../src/wire/format/constants.js";

// ── reference vectors ──────────────────────────────────────────────────────────
// state=JOB, axes_enabled=0x0f, axes_homed=0x0f, faults=0, reason=RUNNING,
//   buf_count=5, pos=[100,-200,0,0], expected_seq=42, queued_us=1234567
const REF_JOB = new Uint8Array([
    167, 9, 15, 15, 0, 0, 1, 5, 0, 100, 0, 0, 0, 56, 255, 255,
    255, 0, 0, 0, 0, 0, 0, 0, 0, 42, 135, 214, 18, 0, 245,
]);
// state=0, axes_enabled=0, axes_homed=0, faults=0, reason=0, buf_count=0,
//   pos=[0,0,0,0], expected_seq=0, queued_us=0
const REF_IDLE = new Uint8Array([
    167, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 115,
]);

describe("wire/format/status: enums", () => {
    it("MachineState mirrors the firmware values", () => {
        expect(MachineState.IDLE).toBe(0);
        expect(MachineState.ESTOP).toBe(2);
        expect(MachineState.ALARM).toBe(3);
        expect(MachineState.HOMING).toBe(5);
        expect(MachineState.JOB).toBe(9);
    });

    it("Fault mirrors the firmware bits", () => {
        expect(Fault.BUS).toBe(0x0001);
        expect(Fault.CONFIG).toBe(0x0002);
        expect(Fault.UNMAPPED).toBe(0x0004);
        expect(Fault.LATCHED).toBe(0x0008);
        expect(Fault.HOMING).toBe(0x0010);
        expect(Fault.PROBE).toBe(0x0020);
        expect(Fault.FACTS).toBe(0x0040);
        expect(Fault.SEQUENCE).toBe(0x0080);
        expect(Fault.ESTOP).toBe(0x8000);
    });

    it("JobReason mirrors the firmware values", () => {
        expect(JobReason.BUFFERING).toBe(0);
        expect(JobReason.RUNNING).toBe(1);
        expect(JobReason.HOLDING).toBe(2);
        expect(JobReason.PAUSED).toBe(3);
        expect(JobReason.ABORTING).toBe(4);
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
    it("produces a 31-byte frame", () => {
        const pkt = packStatusRsp({
            state: 0,
            axesEnabled: 0,
            axesHomed: 0,
            faults: 0,
            reason: 0,
        });
        expect(pkt.length).toBe(STATUS_RSP_SIZE);
    });

    it("packs the JOB vector byte for byte", () => {
        const pkt = packStatusRsp({
            state: MachineState.JOB,
            axesEnabled: 0x0f,
            axesHomed: 0x0f,
            faults: 0,
            reason: JobReason.RUNNING,
            bufCount: 5,
            pos: [100, -200, 0, 0],
            expectedSeq: 42,
            queuedUs: 1234567,
        });
        expect([...pkt]).toEqual([...REF_JOB]);
    });

    it("is byte-identical to Python pack_status_rsp for the all-zero IDLE vector", () => {
        const pkt = packStatusRsp({
            state: MachineState.IDLE,
            axesEnabled: 0,
            axesHomed: 0,
            faults: 0,
            reason: 0,
        });
        expect([...pkt]).toEqual([...REF_IDLE]);
    });

    it("packs negative pos values as two's-complement int32 LE", () => {
        const pkt = packStatusRsp({
            state: 0,
            axesEnabled: 0,
            axesHomed: 0,
            faults: 0,
            reason: 0,
            pos: [-1, 0, 0, 0],
        });
        expect(pkt[9]).toBe(0xff);
        expect(pkt[10]).toBe(0xff);
        expect(pkt[11]).toBe(0xff);
        expect(pkt[12]).toBe(0xff);
    });
});

describe("wire/format/status: parseStatusRsp", () => {
    it("round-trips the JOB vector exactly", () => {
        const st = parseStatusRsp(REF_JOB);
        expect(st.state).toBe(MachineState.JOB);
        expect(st.axesEnabled).toBe(0x0f);
        expect(st.axesHomed).toBe(0x0f);
        expect(st.faults).toBe(0);
        expect(st.reason).toBe(JobReason.RUNNING);
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
        const bad = packStatusRsp({ state: 0, axesEnabled: 0, axesHomed: 0, faults: 0, reason: 0 });
        bad[0] = 0xcf;
        expect(() => parseStatusRsp(bad)).toThrow(/Bad magic: 0xcf/);
    });

    it("rejects a wrong-size buffer", () => {
        const short = new Uint8Array(9);
        short[0] = MAGIC_STATUS_RSP;
        expect(() => parseStatusRsp(short)).toThrow(/Expected 31 bytes, got 9/);
    });

    it("rejects a CRC mismatch", () => {
        const pkt = packStatusRsp({ state: 0, axesEnabled: 0, axesHomed: 0, faults: 0, reason: 0 });
        pkt[10] = pkt[10]! ^ 0xff; // flip a payload byte
        expect(() => parseStatusRsp(pkt)).toThrow(/CRC mismatch/);
    });

    it("falls back to IDLE on an out-of-range state byte (forward-compat)", () => {
        const pkt = packStatusRsp({ state: 0, axesEnabled: 0, axesHomed: 0, faults: 0, reason: 0 });
        // 99 is not a known MachineState — keep CRC valid by recomputing via pack
        const forged = packStatusRsp({
            state: 99,
            axesEnabled: 0,
            axesHomed: 0,
            faults: 0,
            reason: 99, // passed through raw
        });
        void pkt;
        const st = parseStatusRsp(forged);
        expect(st.state).toBe(MachineState.IDLE);
        expect(st.faults).toBe(0);
        expect(st.reason).toBe(99);
    });
});

describe("wire/format/status: statusFromGet (text plane)", () => {
    const parse = (line: string) => statusFromGet(parseGetReply(line));

    it("parses a standard reply", () => {
        const st = parse("state=9 enabled=0x0f homed=0x0f faults=0x0000 job=3");
        expect(st.state).toBe(MachineState.JOB);
        expect(st.axesEnabled).toBe(0x0f);
        expect(st.axesHomed).toBe(0x0f);
        expect(st.faults).toBe(0);
        expect(st.reason).toBe(JobReason.PAUSED);
    });

    it("leaves binary-only fields undefined (distinguishing text from real zero)", () => {
        const st = parse("state=0 enabled=0x0f homed=0x0f faults=0x0000 job=-");
        expect(st.bufCount).toBeUndefined();
        expect(st.pos).toBeUndefined();
        expect(st.expectedSeq).toBeUndefined();
        expect(st.queuedUs).toBeUndefined();
    });

    it("defaults enabled to 0 when absent", () => {
        const st = parse("state=9 homed=0x03");
        expect(st.state).toBe(MachineState.JOB);
        expect(st.axesEnabled).toBe(0);
        expect(st.axesHomed).toBe(0x03);
    });

    it("tolerates unknown trailing tokens (forward-compat)", () => {
        const st = parse("state=7 homed=0x0f future=7");
        expect(st.state).toBe(MachineState.JOGGING);
    });

    it("falls back to IDLE/NONE on unknown enum values", () => {
        const st = parse("state=42 homed=0x0f faults=0x0000 job=99");
        expect(st.state).toBe(MachineState.IDLE);
        expect(st.faults).toBe(0);
    });

    it("throws on a non-status line", () => {
        expect(() => parse("pong")).toThrow(/not a get reply/);
    });

    it("throws on a line missing homed", () => {
        expect(() => parse("state=0 enabled=0x0f")).toThrow(/lacks state or homed/);
    });

    it("parses homecycle and nodehomed", () => {
        const st = parse("state=5 homed=0x00 homing=0 probed=0 homecycle=2 nodehomed=0x018");
        expect(st.homeCycle).toBe(2);
        expect(st.nodeHomed).toBe(0x18);
    });

    it("leaves homecycle and nodehomed undefined when absent", () => {
        const st = parse("state=0 homed=0x0f");
        expect(st.homeCycle).toBeUndefined();
        expect(st.nodeHomed).toBeUndefined();
    });
});

describe("wire/format/status: get reply", () => {
    it("keeps keys in reply order with their raw values", () => {
        const r = parseGetReply("pos=1,-2,3,4 homed=0x0f homefail=- bogus=!");
        expect([...r.keys()]).toEqual(["pos", "homed", "homefail", "bogus"]);
        expect(r.get("homefail")).toBe("-");
    });

    it("folds markers to undefined in getValue", () => {
        const r = parseGetReply("a=- b=? c=! d=7");
        expect(getValue(r, "a")).toBeUndefined();
        expect(getValue(r, "b")).toBeUndefined();
        expect(getValue(r, "c")).toBeUndefined();
        expect(getValue(r, "d")).toBe("7");
        expect(getValue(r, "e")).toBeUndefined();
    });

    it("throws on an err reply or a line with no key=value", () => {
        expect(() => parseGetReply("err too_many_keys")).toThrow(/not a get reply/);
        expect(() => parseGetReply("err unknown")).toThrow(/not a get reply/);
        expect(() => parseGetReply("pong")).toThrow(/not a get reply/);
    });

    it("reads markers as absent fields", () => {
        const withMarkers = statusFromGet(parseGetReply(
            "state=3 enabled=0x00 homed=0x01 faults=0x0010 job=- latched=0x00 homefail=2 " +
            "homenode=3 cfgerr=- homing=- probing=- probe=- retries=- psteps=- probed=0 " +
            "pz=- homecycle=? nodehomed=0x008 future=!"));
        const without = statusFromGet(parseGetReply(
            "state=3 enabled=0x00 homed=0x01 faults=0x0010 job=- latched=0x00 homefail=2 " +
            "homenode=3 probed=0 nodehomed=0x008"));
        expect(withMarkers).toEqual(without);
    });

    it("reads seqfail", () => {
        const st = statusFromGet(parseGetReply("state=3 homed=0x03 faults=0x0080 seqfail=soft_limit seq=5"));
        expect(st.hasFault(Fault.SEQUENCE)).toBe(true);
        expect(st.seqFail).toBe("soft_limit");
        expect(st.seq).toBe(5);
        expect(statusFromGet(parseGetReply("state=0 homed=0x03 seqfail=-")).seqFail).toBeUndefined();
    });

    it("requires state and homed", () => {
        expect(() => statusFromGet(parseGetReply("state=0 homed=!"))).toThrow(/lacks state or homed/);
    });
});

describe("wire/format/status: MachineStatus helpers", () => {
    // axesHomed=0x06 (Y+Z homed, X+A not), axesEnabled=0x0f (all energised)
    const st = new MachineStatus(
        MachineState.IDLE,
        0x06,
        0x0f,
        0,
        0,
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