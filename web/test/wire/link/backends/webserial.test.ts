/**
 * Tests for transport/webserial — the two ways in. The Web Serial API itself
 * is browser-only; a fake port stands in, with Node's web streams.
 */

import { describe, it, expect } from "vitest";
import { WebSerialTransport } from "../../../../src/transport/webserial.js";

type FakePort = Parameters<typeof WebSerialTransport.fromPort>[0];

function closedPort(): FakePort {
    return { readable: null, writable: null, open: async () => {}, close: async () => {} };
}

function openPort(written: Uint8Array[]): FakePort {
    return {
        readable: new ReadableStream<Uint8Array>(),
        writable: new WritableStream<Uint8Array>({ write: (c) => void written.push(c) }),
        open: async () => {},
        close: async () => {},
    };
}

describe("transport/webserial: fromPort", () => {
    it("throws on a port that is not open", () => {
        expect(() => WebSerialTransport.fromPort(closedPort())).toThrow(/not open/);
    });

    it("wraps an open port and writes through it", async () => {
        const written: Uint8Array[] = [];
        const t = WebSerialTransport.fromPort(openPort(written));
        await t.write(new Uint8Array([1, 2, 3]));
        expect(written).toEqual([new Uint8Array([1, 2, 3])]);
    });
});
