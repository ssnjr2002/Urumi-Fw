/**
 * link/backends/node.ts — Node.js Transport over the `serialport` package.
 *
 * `serialport` is an OPTIONAL peer dependency — the Link core + every other
 * backend never imports it, so a browser build stays dependency-free. This
 * file lives at a separate package subpath and is the ONLY place that touches
 * the native module.
 *
 * Usage:
 *   const link = await openNodeLink("/dev/ttyACM0");
 */

import type { Transport } from "../transport.js";

/** The subset of the SerialPort API this backend actually uses. */
interface NodeSerialPort {
    path: string;
    baudRate: number;
    isOpen: boolean;
    on(event: "data", cb: (data: Buffer) => void): void;
    on(event: "error" | "close", cb: (err?: Error) => void): void;
    write(data: Buffer): boolean;
    close(): Promise<void>;
}

export class NodeTransport implements Transport {
    private readonly _port: NodeSerialPort;
    private _closed = false;

    private constructor(port: NodeSerialPort) {
        this._port = port;
        this._port.on("error", () => {
            /* link's read loop will see end-of-stream */
        });
    }

    /**
     * Open a serial port. The `serialport` native package is dynamically
     * loaded — a browser build never resolves it.
     */
    static async open(path: string, baudRate: number = 115200): Promise<NodeTransport> {
        // @ts-expect-error — serialport is an optional peer dep; this file lives
        // at a separate package subpath consumers opt into. The TS error is
        // expected when the dep isn't installed (CI, browsers, Node without it).
        const mod = (await import("serialport")) as {
            SerialPort: new (opts: { path: string; baudRate: number }) => NodeSerialPort;
        };
        const port = new mod.SerialPort({ path, baudRate });
        return new NodeTransport(port);
    }

    // -- Transport surface ----------------------------------------------------

    write(bytes: Uint8Array): Promise<void> {
        this._port.write(Buffer.from(bytes));
        return Promise.resolve();
    }

    /** Pump the serial port's data events into the Demux via the Link's read loop. */
    async *read(): AsyncIterable<Uint8Array> {
        let resolveChunk: (() => void) | null = null;
        const chunks: Uint8Array[] = [];
        let ended = false;

        const onData = (data: Buffer): void => {
            chunks.push(new Uint8Array(data));
            if (resolveChunk) {
                resolveChunk();
                resolveChunk = null;
            }
        };
        const onEnd = (): void => {
            ended = true;
            if (resolveChunk) {
                resolveChunk();
                resolveChunk = null;
            }
        };

        this._port.on("data", onData);
        this._port.on("close", onEnd);
        this._port.on("error", onEnd);

        try {
            while (!ended) {
                if (chunks.length > 0) {
                    const total = chunks.reduce((n, c) => n + c.length, 0);
                    const buf = new Uint8Array(total);
                    let off = 0;
                    for (const c of chunks) {
                        buf.set(c, off);
                        off += c.length;
                    }
                    chunks.length = 0;
                    yield buf;
                } else {
                    await new Promise<void>((r) => {
                        resolveChunk = r;
                    });
                }
            }
        } finally {
            if (chunks.length > 0) {
                const total = chunks.reduce((n, c) => n + c.length, 0);
                const buf = new Uint8Array(total);
                let off = 0;
                for (const c of chunks) {
                    buf.set(c, off);
                    off += c.length;
                }
                chunks.length = 0;
                yield buf;
            }
        }
    }

    async close(): Promise<void> {
        if (this._closed) return;
        this._closed = true;
        await this._port.close();
    }
}

/**
 * One-liner: open a Node serial port and return a Link ready to use.
 * Dynamically imports `serialport`, so a browser build never loads native bindings.
 */
export async function openNodeLink(path: string, baudRate?: number): Promise<import("../link.js").Link> {
    const { Link } = await import("../link.js");
    return new Link(await NodeTransport.open(path, baudRate));
}