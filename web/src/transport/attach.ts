/**
 * transport/attach.ts — the sim's private hook into the link. Not exported
 * from urumi-host/transport.
 *
 * A transport that answers in-process (the sim) has no byte stream to read;
 * it exposes `attach(sink)` and feeds its replies straight into the link's
 * demultiplexer. Real ports don't implement it: the link drains their
 * `read()` instead.
 */

/** What an attached transport feeds its replies into. */
export interface ByteSink {
    feed(data: Uint8Array): void;
}

/** A transport that feeds an attached sink. */
export interface Attachable {
    attach(sink: ByteSink): void;
}
