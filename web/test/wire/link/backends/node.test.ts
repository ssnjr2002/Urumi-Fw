/**
 * Tests for transport/node — opening. `serialport` is not installed in this
 * package, which is the missing-package case; a fake constructor stands in
 * for the rest.
 */

import { describe, it, expect } from "vitest";
import { NodeSerialTransport, type SerialPortCtor } from "../../../../src/transport/node.js";

/** A fake SerialPort whose open() calls back with `err` after a tick. */
function fakeCtor(err: Error | null, seen: { opened: boolean; opts?: unknown }): SerialPortCtor {
    return class {
        path = "";
        baudRate = 0;
        isOpen = false;
        constructor(opts: { path: string; baudRate: number; autoOpen: false }) {
            seen.opts = opts;
        }
        on(): void {}
        write(): boolean { return true; }
        open(cb: (e: Error | null) => void): void {
            setTimeout(() => {
                seen.opened = err === null;
                cb(err);
            }, 1);
        }
        async close(): Promise<void> {}
    } as unknown as SerialPortCtor;
}

describe("transport/node: open", () => {
    it("names serialport when the package is missing", async () => {
        await expect(NodeSerialTransport.open("/dev/null")).rejects.toThrow(/"serialport" package/);
    });

    it("resolves only once the port is open", async () => {
        const seen = { opened: false } as { opened: boolean; opts?: unknown };
        const t = await NodeSerialTransport.openWith(fakeCtor(null, seen), "/dev/ttyACM0", 115200);
        expect(t).toBeInstanceOf(NodeSerialTransport);
        expect(seen.opened).toBe(true);
        expect(seen.opts).toEqual({ path: "/dev/ttyACM0", baudRate: 115200, autoOpen: false });
    });

    it("rejects with serialport's error when the port can't be opened", async () => {
        const seen = { opened: false };
        await expect(
            NodeSerialTransport.openWith(fakeCtor(new Error("Access denied"), seen), "COM5", 115200),
        ).rejects.toThrow("Access denied");
    });
});
