/**
 * link/abort.ts — the Writer's two seams. Private to link.
 *
 * `Writable` is all the Writer needs of a transport. `AbortToken`: the Writer
 * checks it before committing a batch (bounded to never hold the write lock
 * for a whole session), and a Session's `truncate()` sets it so an urgent
 * abort waits at most one frame rather than a whole batch.
 */

/** Bytes-out surface — the Writer needs only this. */
export interface Writable {
    write(bytes: Uint8Array): Promise<void>;
}

/** Flag the Writer polls before committing a batch. */
export interface AbortToken {
    isSet(): boolean;
}

/**
 * Default AbortToken: a boolean flag. A Session constructs one and `truncate()`
 * sets it; the Writer's `writeBatch` sees it and returns 0 without writing,
 * so the in-flight batch never goes out and the caller rewinds to `base`.
 */
export class AbortFlag implements AbortToken {
    private _set = false;
    set(): void {
        this._set = true;
    }
    clear(): void {
        this._set = false;
    }
    isSet(): boolean {
        return this._set;
    }
}
