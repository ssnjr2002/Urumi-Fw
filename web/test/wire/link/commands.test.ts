/**
 * Tests for link/commands — the text commands as methods, over a Link backed
 * by the in-process Sim, and the lines sent where the sim has no model.
 */

import { describe, it, expect } from "vitest";
import { Link } from "../../../src/link/link.js";
import { Commands, CommandError } from "../../../src/link/commands.js";
import { SimTransport } from "../../../src/transport/sim.js";
import { MachineState, Fault, JobReason } from "../../../src/packets/status.js";

const tick = (ms = 5) => new Promise<void>((r) => setTimeout(r, ms));

async function withLink<T>(
    fn: (link: Link, cmd: Commands, sim: SimTransport) => Promise<T>,
    opts: ConstructorParameters<typeof SimTransport>[0] = { axesMap: [1, 2, 3, 4] },
): Promise<T> {
    const sim = new SimTransport(opts);
    const link = new Link(sim, { statusPollMs: 10 });
    try {
        return await fn(link, link.commands(), sim);
    } finally {
        await link.close();
    }
}

/** Commands over a fake text plane that records each line and answers `reply`. */
function lines(reply: (line: string) => string = () => "ok"): { cmd: Commands; sent: string[] } {
    const sent: string[] = [];
    const line = (s: string): Promise<string> => {
        sent.push(s);
        return Promise.resolve(reply(s));
    };
    return { cmd: new Commands(line, {} as Link), sent };
}

async function refusal(p: Promise<unknown>): Promise<CommandError> {
    const e = await p.then(() => null, (x: unknown) => x);
    expect(e).toBeInstanceOf(CommandError);
    return e as CommandError;
}

describe("link/commands: the object", () => {
    it("link.commands() is the same object every time", async () => {
        await withLink(async (link, cmd) => {
            expect(link.commands()).toBe(cmd);
        });
    });
});

describe("link/commands: reads", () => {
    it("ping resolves on pong", async () => {
        await withLink(async (_l, cmd) => {
            await expect(cmd.ping()).resolves.toBeUndefined();
        });
    });

    it("pingNode resolves on node ok and throws the node's result", async () => {
        await withLink(async (_l, cmd) => {
            await cmd.pingNode(3);
            const e = await refusal(cmd.pingNode(7));
            expect([e.reason, e.reply]).toEqual(["timeout", "node 7 timeout"]);
        });
    });

    it("pingAll maps every address to its result", async () => {
        await withLink(async (_l, cmd) => {
            const all = await cmd.pingAll();
            expect(all.size).toBe(8);
            expect(all.get(1)).toBe("ok");
            expect(all.get(5)).toBe("timeout");
        });
    });

    it("get answers each key in the order asked, unknown keys as !", async () => {
        await withLink(async (_l, cmd) => {
            const r = await cmd.get(["pos", "state", "nosuchkey", "pz"]);
            expect([...r]).toEqual([["pos", "1:-,2:-,3:-,4:-"], ["state", "0"], ["nosuchkey", "!"], ["pz", "-"]]);
        });
    });

    it("get refuses an empty or oversized key list before sending", async () => {
        await withLink(async (_l, cmd) => {
            await expect(cmd.get([])).rejects.toThrow(RangeError);
            await expect(cmd.get(Array(33).fill("state"))).rejects.toThrow(RangeError);
        });
    });

    it("keys lists the keys the firmware knows", async () => {
        await withLink(async (_l, cmd) => {
            expect(await cmd.keys()).toContain("placed");
        });
    });

    it("nodePos is the node's counter; a missing node throws", async () => {
        await withLink(async (_l, cmd) => {
            expect(await cmd.nodePos(3)).toBe(0);
            expect((await refusal(cmd.nodePos(7))).reason).toBe("timeout");
        });
    });

    it("nodeStat leaves out what the node didn't send", async () => {
        const { cmd } = lines(() => "node 4 type 1 en 1 datum 0 homing 0 pos -12 slot 2");
        expect(await cmd.nodeStat(4)).toEqual({
            id: 4, type: 1, enabled: true, datum: false, limit: undefined, homing: false, pos: -12,
            span: undefined, index: undefined, indexCause: undefined, crossings: undefined, stepsPerRev: undefined,
        });
    });

    it("no reply in time throws timeout", async () => {
        const { cmd } = lines(() => "");
        const e = await refusal(cmd.ping());
        expect([e.reason, e.reply]).toEqual(["timeout", ""]);
    });
});

describe("link/commands: lifecycle and nodes", () => {
    it("enable, disable, axesEnable and setOrigin resolve on ok", async () => {
        await withLink(async (_l, cmd) => {
            await cmd.axesEnable(true);
            await cmd.enable(2);
            await cmd.disable(2);
            await cmd.setOrigin([{ id: 1, steps: 0 }]);
        });
    });

    it("unalarm refuses a latched e-stop with reason estop", async () => {
        await withLink(async (link, cmd) => {
            link.estop();
            await tick(20);
            const st = await link.freshStatus();
            expect(st.state).toBe(MachineState.ALARM);
            expect(st.hasFault(Fault.ESTOP)).toBe(true);
            expect((await refusal(cmd.unalarm())).reason).toBe("estop");
            expect(link.stats().textDesyncs).toBe(0);
        });
    });

    it("reset resolves once the banner is past", async () => {
        await withLink(async (link, cmd) => {
            await cmd.reset();
            await cmd.ping();
            expect(link.stats().textDesyncs).toBe(0);
        });
    });

    it("pause refused in IDLE; a paused job cancels to IDLE", async () => {
        await withLink(async (link, cmd, sim) => {
            expect((await refusal(cmd.pause())).reason).toBe("bad_state");
            sim._forceRunning();
            await cmd.pause();
            const st = await link.freshStatus();
            expect([st.state, st.reason]).toEqual([MachineState.JOB, JobReason.PAUSED]);
            await cmd.cancel();
            expect((await link.freshStatus()).state).toBe(MachineState.IDLE);
        });
    });
});

describe("link/commands: peripherals", () => {
    const opts = { axesMap: [1, 2, 3, 4], busNodes: [1, 2, 3, 4, 7, 8] };

    it("reach the node and record their state", async () => {
        await withLink(async (_l, cmd, sim) => {
            await cmd.knifeOsc(7, true);
            expect(sim.knifeOsc.get(7)).toBe(true);
            await cmd.knifeBlower(7, 60);
            expect(sim.knifeBlower.get(7)).toBe(60);
            await cmd.vacPump(8, true);
            expect(sim.vacPump.get(8)).toBe(true);
            await cmd.vacServo(8, 1, true);
            expect(sim.vacServo.get("8:1")).toBe(true);
            expect(await cmd.vacSwitch(8)).toBe(false);
        }, opts);
    });

    it("a node off the bus throws its result", async () => {
        await withLink(async (_l, cmd) => {
            expect((await refusal(cmd.vacPump(5, true))).reason).toBe("timeout");
            expect((await refusal(cmd.vacSwitch(5))).reason).toBe("timeout");
        });
    });

    it("knifeBlower refuses a duty outside 0 to 100 before sending", async () => {
        const { cmd, sent } = lines();
        await expect(cmd.knifeBlower(7, 250)).rejects.toThrow(RangeError);
        expect(sent).toEqual([]);
    });
});

describe("link/commands: axesMap and slotMap on an unmapped boot", () => {
    const opts = { busNodes: [1, 2, 3, 4, 5, 6] };

    it("read and set; a refusal leaves the map", async () => {
        await withLink(async (_l, cmd) => {
            expect(await cmd.axesMap()).toEqual([null, null, null, null]);
            await cmd.axesMap([1, 2, 3, 4]);
            expect(await cmd.axesMap()).toEqual([1, 2, 3, 4]);
            const e = await refusal(cmd.axesMap([1, 2, 7, 4]));
            expect([e.reason, e.reply]).toEqual(["timeout", "err node 7 timeout"]);
            expect((await refusal(cmd.axesMap([1, 1, 3, 4]))).reason).toBe("dup");
            await cmd.slotMap([1, 2, 5, null]);
            expect(await cmd.slotMap()).toEqual([1, 2, 5, null]);
        }, opts);
    });

    it("reads a pending or fenced slot as its node", async () => {
        const { cmd } = lines(() => "slot_map 1 !2 - 4");
        expect(await cmd.slotMap()).toEqual([1, 2, null, 4]);
    });
});

describe("link/commands: lines sent", () => {
    it("home and its forms", async () => {
        const { cmd, sent } = lines();
        await cmd.home();
        await cmd.home({ nodes: [3, 4] });
        await cmd.home({ nodes: [3], only: true });
        await cmd.home({ form: "unhomed" });
        await cmd.home({ cycle: 2 });
        await cmd.home({ head: 1 });
        await cmd.home({ form: "xy" });
        expect(sent).toEqual([
            "home", "home 3 4", "home only 3", "home unhomed", "home cycle 2", "home head 1", "home xy",
        ]);
    });

    it("leg puts the form before the node, as the firmware reads it", async () => {
        const { cmd, sent } = lines();
        const ramp = { startUs: 2500, endUs: 500, steps: 400 };
        await cmd.leg({ form: "seek", id: 1, dir: 1, ramp, maxSteps: 88000 });
        await cmd.leg({ form: "park", id: 2, target: 100, ramp });
        await cmd.leg({ form: "probe", id: 3, dir: 0, ramp, pollDiv: 4, maxSteps: 900, deadlineUs: 5000000, expectOpen: true });
        await cmd.legAbort(1);
        expect(sent).toEqual([
            "leg seek 1 1 2500 500 400 88000",
            "leg park 2 100 2500 500 400",
            "leg probe 3 0 2500 500 400 4 900 5000000 1",
            "leg_abort 1",
        ]);
    });

    it("goto, jog and the positioning commands", async () => {
        const { cmd, sent } = lines((s) => (s.startsWith("jog") ? "ok 2" : "ok"));
        await cmd.goto({ frame: "m", x: 10, y: 20 });
        await cmd.goto({ frame: "w", x: "min", y: "max", scale: 0.5 });
        await cmd.goto({ frame: "w", z: 3 });
        await cmd.goto({ frame: "m", a: 90, scale: 2 });
        await cmd.goto({ name: "park", scale: 0.5 });
        await cmd.goto({ name: "up" });
        expect(await cmd.jog({ x: 1, y: -1 })).toBe(2);
        await cmd.jog({ z: 0.5 }, { scale: 0.25 });
        await cmd.jog({ x: 3 });
        await cmd.select("anchor");
        await cmd.wzero();
        await cmd.wzero(["x", "zSwitch"]);
        await cmd.wset({ x: 10, z: -2 });
        await cmd.wclear();
        await cmd.place("set");
        await cmd.place("go", { x: "max", y: "min" });
        expect(sent).toEqual([
            "goto m xy 10 20", "goto w xy min max 0.5", "goto w z 3", "goto m a 90 2", "goto park 0.5", "goto up",
            "jog xy 1 -1", "jog z 0.5 0.25", "jog x 3",
            "select anchor", "wzero", "wzero x z_switch", "wset x 10 z -2", "wclear",
            "place set", "place go xmax ymin",
        ]);
    });

    it("jog refuses no axis set or two before sending", async () => {
        const { cmd, sent } = lines();
        await expect(cmd.jog({})).rejects.toThrow(RangeError);
        await expect(cmd.jog({ x: 1, z: 1 } as never)).rejects.toThrow(RangeError);
        expect(sent).toEqual([]);
    });

    it("a refusal carries the line, the token and the reply", async () => {
        const { cmd } = lines(() => "err soft_limit x 12.5");
        const e = await refusal(cmd.goto({ frame: "w", x: 500, y: 0 }));
        expect([e.command, e.reason, e.reply]).toEqual(["goto w xy 500 0", "soft_limit", "err soft_limit x 12.5"]);
    });
});

describe("link/commands: mount and unmount", () => {
    it("a head holds one tool: mount refuses until unmount", async () => {
        await withLink(async (link, cmd) => {
            expect((await refusal(cmd.mount(0, 0))).reason).toBe("usage");
            await cmd.mount(0, 1);
            expect((await refusal(cmd.mount(0, 2))).reason).toBe("mounted");
            await cmd.unmount(0);
            await cmd.mount(0, 2);
            expect((await link.freshStatus()).tools).toEqual([2, 0]);
        }, { axesMap: [1, 2, 3, 4], tools: {} });
    });
});

describe("link/commands: onReady", () => {
    it("waits out a running leg, then sends", async () => {
        await withLink(async (link, cmd) => {
            await cmd.axesEnable(true);
            await cmd.leg({ form: "seek", id: 1, dir: 1, ramp: { startUs: 2500, endUs: 500, steps: 400 }, maxSteps: 88000 });
            expect((await refusal(cmd.leg({ form: "seek", id: 2, dir: 1, ramp: { startUs: 2500, endUs: 500, steps: 400 }, maxSteps: 88000 }))).reason).toBe("busy");
            await cmd.onReady.legAbort(2);
            expect((await link.freshStatus()).busy).toBe(false);
        });
    });

    it("throws estop while the e-stop is latched", async () => {
        await withLink(async (link, cmd) => {
            link.estop();
            await tick(20);
            await link.freshStatus();
            expect((await refusal(cmd.onReady.axesEnable(true))).reason).toBe("estop");
        });
    });

    it("has no reads", async () => {
        await withLink(async (_l, cmd) => {
            expect("get" in cmd.onReady).toBe(false);
            expect("send" in cmd.onReady).toBe(false);
        });
    });
});
