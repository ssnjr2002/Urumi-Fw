/**
 * runtimeHeader.ts — RUNTIME_FIELDS as the controller's X-macro header.
 */

import { RUNTIME_AXES, RUNTIME_EFFECTS, RUNTIME_FIELDS, type RuntimeField } from "./runtime.js";

function limit(v: number | undefined): string {
    if (v === undefined) return "RT_NO_LIMIT";
    return Number.isInteger(v) ? `${v}.0f` : `${v}f`;
}

function line(f: RuntimeField): string {
    const effects = f.effects.map((e) => `RT_FX_${e}`).join(" | ") || "0";
    return [
        `RT_SCOPE_${f.scope.toUpperCase()}`,
        `"${f.path}"`,
        f.member,
        ...(f.has !== undefined ? [f.has] : []),
        `RT_TYPE_${f.type.toUpperCase()}`,
        limit(f.min),
        limit(f.max),
        f.minExclusive ? "true" : "false",
        `RT_HOMING_${(f.homing ?? "none").toUpperCase()}`,
        f.axes === undefined ? "RT_AXES_ALL" : f.axes.map((a) => `RT_AXES_${a.toUpperCase()}`).join(" | "),
        effects,
    ].join(", ");
}

export function renderRuntimeHeader(): string {
    const rows = RUNTIME_FIELDS.map(
        (f) => `    ${f.has !== undefined ? "OPTIONAL" : "REQUIRED"}(${line(f)}) \\`,
    );
    const effects = RUNTIME_EFFECTS.map((e, i) => `    RT_FX_${e} = 1u << ${i},`);
    const axes = RUNTIME_AXES.map((a, i) => `    RT_AXES_${a.toUpperCase()} = 1u << ${i},`);
    return [
        "// Generated from web/src/config/runtime.ts by `pnpm gen:runtime`. Do not edit.",
        "#pragma once",
        "#include <math.h>",
        "",
        "enum RtScope { RT_SCOPE_MACHINE, RT_SCOPE_AXIS, RT_SCOPE_HEAD, RT_SCOPE_TOOL };",
        "enum RtType { RT_TYPE_F32, RT_TYPE_BOOL, RT_TYPE_U16, RT_TYPE_POINT };",
        "enum RtHoming { RT_HOMING_NONE, RT_HOMING_LINEAR, RT_HOMING_ROTARY, RT_HOMING_BOTH };",
        "enum RtEffect : unsigned {",
        ...effects,
        "};",
        "// An axis row's axes; other scopes take RT_AXES_ALL.",
        "enum RtAxes : unsigned {",
        ...axes,
        `    RT_AXES_ALL = ${(1 << RUNTIME_AXES.length) - 1}u,`,
        "};",
        "",
        "#define RT_NO_LIMIT NAN",
        "",
        "// REQUIRED(scope, path, member, type, min, max, minExclusive, homing, axes, effects)",
        "// OPTIONAL(scope, path, member, has, type, min, max, minExclusive, homing, axes, effects)",
        "#define RUNTIME_FIELDS(REQUIRED, OPTIONAL) \\",
        ...rows,
        "",
        "",
    ].join("\n");
}
