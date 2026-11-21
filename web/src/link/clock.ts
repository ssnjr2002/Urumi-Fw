/**
 * link/clock.ts — timers that keep time in a background tab
 * (docs/plans/host-watch.md).
 *
 * A hidden tab's setTimeout runs about once a second, and after a few minutes
 * hidden Chrome may batch it to once a minute. The job session's 100 ms poll
 * is what tells the controller the host is still there (it pauses a dry job
 * after 3 s of silence), so its timers run on a dedicated Worker, whose timers
 * are not throttled that way. Where there is no Worker (Node, tests) they are
 * plain setTimeout.
 */

export type TimerId = number | ReturnType<typeof setTimeout>;

interface Clock {
    set(fn: () => void, ms: number): TimerId;
    clear(id: TimerId): void;
}

const plain: Clock = {
    set: (fn, ms) => setTimeout(fn, ms),
    clear: (id) => clearTimeout(id as ReturnType<typeof setTimeout>),
};

const WORKER_SRC = `
const t = new Map();
onmessage = (e) => {
    const [op, id, ms] = e.data;
    if (op === "set") t.set(id, setTimeout(() => { t.delete(id); postMessage(id); }, ms));
    else { clearTimeout(t.get(id)); t.delete(id); }
};`;

function workerClock(): Clock | null {
    if (typeof Worker === "undefined" || typeof Blob === "undefined" ||
        typeof URL === "undefined" || typeof URL.createObjectURL !== "function") return null;
    let w: Worker;
    try {
        w = new Worker(URL.createObjectURL(new Blob([WORKER_SRC], { type: "text/javascript" })));
    } catch {
        return null; // a CSP that forbids blob: workers
    }
    const pending = new Map<number, () => void>();
    let next = 1;
    w.onmessage = (e: MessageEvent<number>) => {
        const fn = pending.get(e.data);
        pending.delete(e.data);
        fn?.();
    };
    return {
        set(fn, ms) {
            const id = next++;
            pending.set(id, fn);
            w.postMessage(["set", id, ms]);
            return id;
        },
        clear(id) {
            if (typeof id !== "number" || !pending.delete(id)) return;
            w.postMessage(["clear", id]);
        },
    };
}

let clock: Clock | null = null;
const get = (): Clock => (clock ??= workerClock() ?? plain);

/** setTimeout that keeps running in a background tab. */
export function setTimer(fn: () => void, ms: number): TimerId {
    return get().set(fn, ms);
}

/** clearTimeout for setTimer(). */
export function clearTimer(id: TimerId): void {
    get().clear(id);
}

/** Resolve after `ms`, in a background tab too. */
export function delay(ms: number): Promise<void> {
    return new Promise((r) => setTimer(r, ms));
}
