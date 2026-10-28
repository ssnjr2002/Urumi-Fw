/**
 * Tests for wire/format/status — STATUS_RSP (0xA3, 37B), the operational
 * enums, MachineStatus, the get reply / parseStatusRsp / packStatusRsp.
 *
 * The reference byte sequences below come from outside the TS packer: one
 * captured from the Pico, one packed in Python.
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
    Selection,
    NO_LAYER,
} from "../../../src/wire/format/status.js";
import { MAGIC_STATUS_RSP, STATUS_RSP_SIZE } from "../../../src/wire/format/constants.js";

// ── reference vectors ──────────────────────────────────────────────────────────
// Packed independently (Python struct, Maxim CRC8): state=JOB, reason=RUNNING,
//   faults=0, nodeEnabled=0x01e, nodeHomed=0x006, selection=1, tools=[2,3],
//   wpos=[100.5,-200,12.25,NaN], bufCount=5, layer=2, record=1234567
const REF_JOB = new Uint8Array([
    163, 9, 1, 0, 0, 30, 0, 6, 0, 1, 2, 3, 0, 0, 201, 66, 0, 0, 72,
    195, 0, 0, 68, 65, 0, 0, 192, 127, 5, 0, 2, 0, 135, 214, 18, 0, 34,
]);
// Emitted by the Pico at rest with no job: IDLE, head 0 selected, no tools,
//   wpos=[-70,-30,0,0], layer=0xFFFF, record=0
const REF_IDLE = new Uint8Array([
    163, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 140, 194, 0, 0, 240,
    193, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 0, 0, 64,
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
        expect(Fault.TOOL).toBe(0x0100);
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
    it("produces a 36-byte frame", () => {
        const pkt = packStatusRsp({ state: 0, faults: 0, reason: 0 });
        expect(pkt.length).toBe(STATUS_RSP_SIZE);
        expect(STATUS_RSP_SIZE).toBe(37);
    });

    it("packs the JOB vector byte for byte", () => {
        const pkt = packStatusRsp({
            state: MachineState.JOB,
            faults: 0,
            reason: JobReason.RUNNING,
            nodeEnabled: 0x01e,
            nodeHomed: 0x006,
            selection: 1,
            tools: [2, 3],
            wpos: [100.5, -200, 12.25, NaN],
            bufCount: 5,
            layer: 2,
            record: 1234567,
        });
        expect([...pkt]).toEqual([...REF_JOB]);
    });

    it("packs the Pico's IDLE frame byte for byte", () => {
        const pkt = packStatusRsp({
            state: MachineState.IDLE,
            faults: 0,
            reason: 0,
            selection: 0,
            wpos: [-70, -30, 0, 0],
        });
        expect([...pkt]).toEqual([...REF_IDLE]);
    });

    it("defaults to no selection, no position and no job", () => {
        const st = parseStatusRsp(packStatusRsp({ state: 0, faults: 0, reason: 0 }));
        expect(st.selection).toBe(Selection.NONE);
        expect(st.tools).toEqual([0, 0]);
        expect(st.wpos!.every(Number.isNaN)).toBe(true);
        expect(st.layer).toBe(NO_LAYER);
        expect(st.record).toBe(0);
    });
});

describe("wire/format/status: parseStatusRsp", () => {
    it("round-trips the JOB vector exactly", () => {
        const st = parseStatusRsp(REF_JOB);
        expect(st.state).toBe(MachineState.JOB);
        expect(st.faults).toBe(0);
        expect(st.reason).toBe(JobReason.RUNNING);
        expect(st.nodeEnabled).toBe(0x01e);
        expect(st.nodeHomed).toBe(0x006);
        expect(st.selection).toBe(1);
        expect(st.tools).toEqual([2, 3]);
        expect(st.wpos!.slice(0, 3)).toEqual([100.5, -200, 12.25]);
        expect(st.wpos![3]).toBeNaN();
        expect(st.bufCount).toBe(5);
        expect(st.layer).toBe(2);
        expect(st.record).toBe(1234567);
    });

    it("leaves the text-plane slot masks undefined", () => {
        const st = parseStatusRsp(REF_JOB);
        expect(st.axesHomed).toBeUndefined();
        expect(st.axesEnabled).toBeUndefined();
        expect(st.homed("x")).toBe(false);
    });

    it("round-trips the IDLE vector exactly", () => {
        const st = parseStatusRsp(REF_IDLE);
        expect(st.state).toBe(MachineState.IDLE);
        expect(st.bufCount).toBe(0);
        expect(st.selection).toBe(0);
        expect(st.wpos).toEqual([-70, -30, 0, 0]);
        expect(st.layer).toBe(NO_LAYER);
        expect(st.record).toBe(0);
    });

    it("rejects a v1 (0xA6) frame with the firmware-reflash message", () => {
        const v1 = new Uint8Array(STATUS_RSP_SIZE);
        v1[0] = 0xa6;
        expect(() => parseStatusRsp(v1)).toThrow(/retired 9-byte STATUS_RSP/);
        expect(() => parseStatusRsp(v1)).toThrow(/Reflash/);
    });

    it("rejects a v2 (0xA7) frame with the firmware-reflash message", () => {
        const v2 = new Uint8Array(STATUS_RSP_SIZE);
        v2[0] = 0xa7;
        expect(() => parseStatusRsp(v2)).toThrow(/retired 31-byte STATUS_RSP/);
    });

    it("rejects a v3 (0xA4) frame with the firmware-reflash message", () => {
        const v3 = new Uint8Array(STATUS_RSP_SIZE);
        v3[0] = 0xa4;
        expect(() => parseStatusRsp(v3)).toThrow(/retired 36-byte STATUS_RSP/);
    });

    it("rejects an unknown magic", () => {
        const bad = packStatusRsp({ state: 0, faults: 0, reason: 0 });
        bad[0] = 0xcf;
        expect(() => parseStatusRsp(bad)).toThrow(/Bad magic: 0xcf/);
    });

    it("rejects a wrong-size buffer", () => {
        const short = new Uint8Array(9);
        short[0] = MAGIC_STATUS_RSP;
        expect(() => parseStatusRsp(short)).toThrow(/Expected 37 bytes, got 9/);
    });

    it("rejects a CRC mismatch", () => {
        const pkt = packStatusRsp({ state: 0, faults: 0, reason: 0 });
        pkt[10] = pkt[10]! ^ 0xff; // flip a payload byte
        expect(() => parseStatusRsp(pkt)).toThrow(/CRC mismatch/);
    });

    it("falls back to IDLE on an out-of-range state byte (forward-compat)", () => {
        const pkt = packStatusRsp({ state: 0, faults: 0, reason: 0 });
        // 99 is not a known MachineState — keep CRC valid by recomputing via pack
        const forged = packStatusRsp({
            state: 99,
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
        expect(st.layer).toBeUndefined();
        expect(st.record).toBeUndefined();
    });

    it("reads the node, selection and work position keys", () => {
        const st = parse("state=0 homed=0x00 nodeenabled=0x01e head=1 tools=2,5 wpos=1.500,-2.000,-,-");
        expect(st.nodeEnabled).toBe(0x01e);
        expect(st.selection).toBe(1);
        expect(st.tools).toEqual([2, 5]);
        expect(st.wpos!.slice(0, 2)).toEqual([1.5, -2]);
        expect(st.wpos![2]).toBeNaN();
        expect(parse("state=0 homed=0x00 head=anchor tools=2,5").selection).toBe(Selection.ANCHOR);
        expect(parse("state=0 homed=0x00 tools=-,5").tools).toEqual([0, 5]);
        expect(parse("state=0 homed=0x00 tools=3").tools).toEqual([3, 0]);   // one head
        expect(parse("state=0 homed=0x00 tools=-").tools).toBeUndefined();   // no config
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