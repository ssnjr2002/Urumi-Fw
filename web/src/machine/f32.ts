/**
 * f32.ts — the float32 the Pico keeps for a number.
 *
 * The Pico holds config numbers as float32 and saves each as its shortest
 * round-trip decimal, so a saved number equals the host's only if it is that
 * decimal already.
 */

/** The shortest decimal with the same float32 as `v`. */
export function f32(v: number): number {
    const f = Math.fround(v);
    for (let p = 1; p < 9; p++) {
        const s = Number(f.toPrecision(p));
        if (Math.fround(s) === f) return s;
    }
    return Number(f.toPrecision(9));
}

/** Whether the Pico would keep `v` exactly. */
export function fitsF32(v: number): boolean {
    return f32(v) === v;
}
