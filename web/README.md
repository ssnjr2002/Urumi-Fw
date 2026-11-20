# urumi-host

Host library for the Urumi cutter: turns an SVG layer into BEZIER packets,
encodes the machine config image, and talks to the Pico over USB
(`wire/link`). Works in modern browsers and in Node 22.12+. TypeScript, ESM.

The Pico plans and runs motion, homing, probing, parking and tool change; this
library prepares the job and streams it. Packet layouts are in
[docs/reference/packets.md](../docs/reference/packets.md), commands in
[docs/reference/commands.md](../docs/reference/commands.md).

## Development

```sh
pnpm install
pnpm typecheck
pnpm test
```

The build, its scripts and the demo: [docs/system/host-build.md](../docs/system/host-build.md).

## Contents

| Path | What |
|---|---|
| `src/svg/load.ts` | SVG text → subpaths in mm (Y up), per layer |
| `src/toolpath/` | clean, annotate, and `prepareBezierJob`: one layer → BEZIER packets |
| `src/wire/format/` | packet encoders and decoders (BEZIER, LAYER_HDR, CJOG, STATUS, CFG) |
| `src/wire/link/` | transports (sim, WebSerial, Node serialport), session, control commands |
| `src/machine/` | the config document: schema, tool presets, validation, binary image |
| `scripts/config-image.ts` | `config/*.jsonc` → `data/config.bin`, run by `pio run -e pico -t uploadfs` |
| `demo/` | the machine console, a plain page over this library (`pnpm demo`) |

## A job

```ts
import { prepareBezierJob, JobSource } from "urumi-host";

const job = prepareBezierJob(svgText, "knife", { offset: { x: 100, y: 50 } });
if ("error" in job) throw new Error(job.error);
// Run new JobSource([{ hdr, tool, head, bbox: job.bbox, packets: job.packets }]) through a Session.
// It holds as `place` after preflight; layers go once the operator sends `place set` and `place ok`.
```

The real-port transports have their own subpaths, so a Node consumer never
pulls browser globals:

```ts
import { WebSerialTransport } from "urumi-host/wire/link/backends/webserial";
import { openNodeLink }       from "urumi-host/wire/link/backends/node";
```

## SVG input

A layer is a `<g>` named by its `inkscape:label`, else its `id`. What is read,
and how: [docs/system/toolpath.md](../docs/system/toolpath.md).
