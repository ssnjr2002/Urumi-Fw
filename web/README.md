# urumi-host

Host library for the Urumi cutter: turns an SVG layer into BEZIER packets,
encodes the machine config image, and talks to the Pico over USB
(`wire/link`). Works in modern browsers and in Node 18+. TypeScript, ESM.

The Pico plans and runs motion, homing, probing, parking and tool change; this
library prepares the job and streams it. Packet layouts and commands are in
[docs/wire_protocol.md](../docs/wire_protocol.md).

## Development

```sh
pnpm install
pnpm typecheck
pnpm test
```

## Contents

| Path | What |
|---|---|
| `src/svg/load.ts` | SVG text → subpaths in mm (Y up), per layer |
| `src/toolpath/` | clean, annotate, and `prepareBezierJob`: one layer → BEZIER packets |
| `src/wire/format/` | packet encoders and decoders (BEZIER, LAYER_HDR, CJOG, STATUS, CFG) |
| `src/wire/link/` | transports (sim, WebSerial, Node serialport), session, control commands |
| `src/machine/` | the config document: schema, tool presets, validation, binary image |
| `scripts/config-image.ts` | `config/*.jsonc` → `data/config.bin`, run by `pio run -e pico -t uploadfs` |
| `demo/` | old demo pages, built on an API that no longer exists; kept for reference |

## A job

```ts
import { prepareBezierJob, packLayerHdr } from "urumi-host";

const job = prepareBezierJob(svgText, "knife", { offset: { x: 100, y: 50 } });
if ("error" in job) throw new Error(job.error);
// Stream packLayerHdr({ tool, head, ... }) and then job.packets through a Session.
```

The real-port transports have their own subpaths, so a Node consumer never
pulls browser globals:

```ts
import { WebSerialTransport } from "urumi-host/wire/link/backends/webserial";
import { openNodeLink }       from "urumi-host/wire/link/backends/node";
```

## SVG input

A layer is a `<g>` named by its `inkscape:label`, else its `id`. Nested named
groups join with `/` (`revolver_pen/slot1`). Geometry outside any named group
is under `''`.

| Element | How it is read |
|---|---|
| `<path>` | `M L H V C S Q T A Z`, absolute and relative |
| `<rect>` | 4 lines, or with arcs if `rx`/`ry` are set |
| `<circle>`, `<ellipse>` | 4 cubic arcs |
| `<line>`, `<polyline>`, `<polygon>` | lines (`<polygon>` closed) |

Anything else (`<defs>`, `<clipPath>`, `<mask>`, `<text>`, …) and elements with
neither fill nor stroke are skipped. Coordinates are converted to mm from the
SVG's `width`/`height`/`viewBox` (`mm`, `cm`, `in`, `pt`, `pc`, `px`; unitless
is px at 96 DPI), and Y is flipped to machine Y-up.

Parsing uses the global `DOMParser`. In Node, set `globalThis.DOMParser` first
(e.g. from `@xmldom/xmldom`, as `test/setup.ts` does).
