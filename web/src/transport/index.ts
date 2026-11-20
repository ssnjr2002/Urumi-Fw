/**
 * transport/index.ts (urumi-host/transport) — the environment-agnostic I/O contract a transport implements.
 *
 * The link layer (sink, demux, writer, session, link) speaks ONLY this
 * interface; a transport — the in-process Sim, a browser WebSerial port, or a
 * Node serialport — adapts its environment to it. That is what makes the
 * comms layer usable from a browser UI OR a Node CLI: everything above `Link`
 * is environment-blind, and the three transports are thin.
 *
 * `write` returns once the bytes are accepted by the underlying stream
 * (there is no separate `flush` — the browser/Node serial impl owns buffering,
 * and the Writer's atomicity guarantee is about not interleaving frames within
 * a single `write`, which the underlying stream already preserves for one
 * chunk). `read` is an async iterable of byte chunks; frame boundaries need
 * not align with chunk boundaries, which is the normal case at the 64-byte
 * USB packet quantum — the Demux consumes them blind.
 *
 * This file is the subpath's whole public surface: the interface alone.
 * attach.ts holds the sim's private hook into the link.
 */

/** A byte link to the controller, which a `Link` owns for a connection. */
export interface Transport {
    /** Send bytes; resolves once the port has accepted them. */
    write(bytes: Uint8Array): Promise<void>;
    /** Chunks from the controller, in arrival order; ends when the port closes. */
    read(): AsyncIterable<Uint8Array>;
    /** Close the port; resolves once closed. */
    close(): Promise<void>;
}
