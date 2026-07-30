/**
 * refFormat.ts — shared serialisation for the C++ port's reference vectors.
 *
 * The port's criterion is bit-equality (docs/planner_audit.md, "Numeric
 * porting rule"), so numbers cross the language boundary as raw IEEE-754 bit
 * patterns. A decimal round-trip here — even at 17 significant digits — would
 * be one more thing to be suspicious of when a diff appears.
 *
 * Not a .test.ts file: vitest must not collect it.
 */

/** A double as its exact 16-hex-digit IEEE-754 bit pattern. */
export function hex(v: number): string {
    const dv = new DataView(new ArrayBuffer(8));
    dv.setFloat64(0, v, false);
    let s = "";
    for (let i = 0; i < 8; i++) s += dv.getUint8(i).toString(16).padStart(2, "0");
    return s;
}
