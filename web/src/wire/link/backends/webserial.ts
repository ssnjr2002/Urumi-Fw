/**
 * link/backends/webserial.ts — browser-only Transport over navigator.serial.
 *
 * Wraps the Web Serial API (the browser equivalent of a PySerial port) behind
 * the same Transport interface the Link speaks. Importing this module pulls in
 * no Node deps; it lives at a separate package subpath so a Node consumer never
 * sees it.
 *
 * Usage:
 *   const t = await WebSerialTransport.requestAndOpen();
 *   const link = new Link(t);
 */

import type { Transport } from "../transport.js";

/** Minimal declaration — the full DOM type normally lives in `lib.dom.d.ts`
 *  (TS 5.5+), but the deployed lib may not include it. The fields the backend
 *  actually uses are declared here. */
interface SerialPort {
    readable: ReadableStream<Uint8Array>;
    writable: WritableStream<Uint8Array>;
    open(opts: SerialOptions): Promise<void>;
    close(): Promise<void>;
}

interface SerialOptions {
    baudRate: number;
}

interface SerialPortRequestOptions {
    filters?: Array<{ usbVendorId?: number; usbProductId?: number }>;
}

export class WebSerialTransport implements Transport {
    private readonly _port: SerialPort;
    private _writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
    private _reader: ReadableStreamDefaultReader<Uint8Array> | null = null;

    constructor(port: SerialPort) {
        this._port = port;
    }

    /** Show the browser's port picker and open at `baudRate`. */
    static async requestAndOpen(baudRate: number = 115200): Promise<WebSerialTransport> {
        const port = await (globalThis.navigator as Navigator & {
            serial: {
                requestPort: (opts?: SerialPortRequestOptions) => Promise<SerialPort>;
            };
        }).serial.requestPort();
        await port.open({ baudRate });
        return new WebSerialTransport(port);
    }

    /** Wrap an already-opened port (e.g. from a stored user preference). */
    static fromOpen(port: SerialPort): WebSerialTransport {
        return new WebSerialTransport(port);
    }

    get port(): SerialPort {
        return this._port;
    }

    // -- Transport surface ----------------------------------------------------

    private _ensureWriter(): WritableStreamDefaultWriter<Uint8Array> {
        if (!this._writer) {
            this._writer = this._port.writable.getWriter();
        }
        return this._writer;
    }

    async write(bytes: Uint8Array): Promise<void> {
        await this._ensureWriter().write(bytes);
    }

    /** Pump the port's readable into the Demux via the Link's read loop. */
    async *read(): AsyncIterable<Uint8Array> {
        if (!this._port.readable) {
            return; // no data stream
        }
        this._reader = this._port.readable.getReader();
        try {
            for (;;) {
                const { value, done } = await this._reader.read();
                if (done) break;
                if (value) yield value;
            }
        } finally {
            this._reader.releaseLock();
            this._reader = null;
        }
    }

    async close(): Promise<void> {
        // Cancel the reader so the read() generator's for(;;) loop
        // breaks and its finally block releases the reader lock (sets
        // _reader = null). Poll until that happens — without it,
        // port.close() hangs because the stream is locked by the reader
        // the Link's read loop holds.
        if (this._reader) {
            try { await this._reader.cancel(); } catch { /* ignore */ }
            const deadline = Date.now() + 2000;
            while (this._reader !== null && Date.now() < deadline) {
                await new Promise((r) => setTimeout(r, 5));
            }
        }
        if (this._writer) {
            this._writer.releaseLock();
            this._writer = null;
        }
        await this._port.close();
    }
}