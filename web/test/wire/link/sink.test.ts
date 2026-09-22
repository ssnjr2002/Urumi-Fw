/**
 * Tests for wire/link/sink — Sink<T> (awaitable queue) and LatestSink<T>
 * (latest-wins slot). These are the D9/D12 primitives the demux fans frames
 * out to and the session/poller subscribe to.
 */

import { describe, it, expect } from "vitest";
import { Sink, LatestSink } from "../../../src/wire/link/sink.js";

describe("wire/link/sink: Sink", () => {
    it("put before get: get resolves immediately with the item", async () => {
        const s = new Sink<number>("t");
        s.put(42);
        expect(await s.get(50)).toBe(42);
    });

    it("get before put: get waits then resolves on put", async () => {
        const s = new Sink<number>("t");
        const p = s.get(50);
        s.put(7);
        expect(await p).toBe(7);
    });

    it("get(timeout) returns null when nothing arrives", async () => {
        const s = new Sink<number>("t");
        expect(await s.get(20)).toBeNull();
    });

    it("clear returns the count and empties the queue", () => {
        const s = new Sink<number>("t");
        s.put(1);
        s.put(2);
        s.put(3);
        expect(s.clear()).toBe(3);
        expect(s.length).toBe(0);
    });

    it("delivers in FIFO order", async () => {
        const s = new Sink<number>("t");
        s.put(1);
        s.put(2);
        s.put(3);
        expect(await s.get(50)).toBe(1);
        expect(await s.get(50)).toBe(2);
        expect(await s.get(50)).toBe(3);
    });

    it("multiple concurrent gets each wake on their own put", async () => {
        const s = new Sink<number>("t");
        const p1 = s.get(50);
        const p2 = s.get(50);
        s.put(10);
        s.put(20);
        expect(await p1).toBe(10);
        expect(await p2).toBe(20);
        expect(s.length).toBe(0);
    });

    it("put to a waiting consumer does not enqueue", async () => {
        const s = new Sink<number>("t");
        const p = s.get(50);
        s.put(99);
        expect(await p).toBe(99);
        expect(s.length).toBe(0);
    });

    it("clear does not resolve a pending get (its awaiter is the current command)", async () => {
        const s = new Sink<number>("t");
        const p = s.get(20);
        s.clear();
        expect(await p).toBeNull();
    });
});

describe("wire/link/sink: LatestSink", () => {
    it("value starts null", () => {
        expect(new LatestSink<number>("t").value).toBeNull();
    });

    it("put sets value and increments the stamp", () => {
        const s = new LatestSink<number>("t");
        s.put(1);
        const stamp1 = s.sample[1];
        s.put(2);
        const stamp2 = s.sample[1];
        expect(s.value).toBe(2);
        expect(stamp2).toBeGreaterThan(stamp1);
    });

    it("sample returns the consistent (value, stamp, arrival) triple", () => {
        const s = new LatestSink<string>("t");
        s.put("hi");
        const [v, stamp, at] = s.sample;
        expect(v).toBe("hi");
        expect(stamp).toBeGreaterThan(0);
        expect(at).toBeGreaterThanOrEqual(0);
    });

    it("waitUpdate resolves immediately when a sample newer than `since` is already in", async () => {
        const s = new LatestSink<number>("t");
        s.put(5);
        const stamp0 = s.sample[1];
        const [v, stamp] = await s.waitUpdate(50, stamp0 - 1);
        expect(v).toBe(5);
        expect(stamp).toBe(stamp0);
    });

    it("waitUpdate(since=current) waits for the NEXT sample", async () => {
        const s = new LatestSink<number>("t");
        s.put(1);
        const stamp0 = s.sample[1];
        const p = s.waitUpdate(200, stamp0); // since == current → wait
        s.put(2);
        const [v, stamp] = await p;
        expect(v).toBe(2);
        expect(stamp).toBeGreaterThan(stamp0);
    });

    it("waitUpdate(timeout) returns [null, stamp] on timeout", async () => {
        const s = new LatestSink<number>("t");
        const result = await s.waitUpdate(20);
        expect(result[0]).toBeNull();
    });

    it("put wakes a pending waitUpdate with the new sample", async () => {
        const s = new LatestSink<number>("t");
        const p = s.waitUpdate(200);
        s.put(42);
        const [v] = await p;
        expect(v).toBe(42);
    });
});