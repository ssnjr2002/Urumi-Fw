# Toolpath

A drawing to a job ([toolpath](../../system/toolpath.md)), in three forms:

| Form | Is | Made by |
|---|---|---|
| **SVG** | the drawing's text | the caller |
| **tool layer** | a group in the SVG named after a config tool: `pen`, or `revolver_pen.2` for chamber 2 | [`loadSvg()`](#loadsvg) |
| **job layer** | a [`JobLayer`](packets.md#joblayer): one tool layer's header and its `BEZIER` records, ready for a `JobSource` | [`buildJob()`](#buildjob) |

```ts
import { loadSvg, buildJob } from "urumi-host/toolpath";
```

**Checked against `<commit>`.** To see what may have changed since:

```bash
git diff <commit> -- web/src/toolpath/
```

# Functions

| Function | |
|---|---|
| [`loadSvg()`](#loadsvg) | the SVG's tool layers, to list or pick from |
| [`buildJob()`](#buildjob) | the SVG and a choice of tool layers to job layers, checked |

### `loadSvg()`

**Syntax**

```ts
loadSvg(svg: string, config: ControllerConfig): LoadedSvg

interface LoadedSvg {
  readonly layers: readonly ToolLayer[];
  readonly warnings: readonly string[];
}
interface ToolLayer {
  readonly key: string;     // "pen", "revolver_pen.2"
  readonly tool: string;    // a toolProfiles name
  readonly chamber: number; // 0 when the name gives none
  readonly paths: readonly { curves: readonly { p0; p1; p2; p3 }[]; closed: boolean }[];
}
```

**Parameters**

* `svg`: the SVG.
* `config`: its tool names decide which groups are tool layers.

**Return value**

The tool layers in document order, and warnings for a person to read.
Each layer's `paths` are its curves, each point a `Point` in mm with Y up,
for drawing it.

A group is a tool layer when its name is a tool's, or a tool's followed by
`.` and a chamber number; the outermost such group counts, and one inside
it joins it, with a warning. Groups of the same name join.

**Exceptions**

* `Error` if `svg` isn't a well-formed SVG.

### `buildJob()`

Turns the chosen tool layers into job layers, and checks what a caller
would otherwise check by hand.

**Syntax**

```ts
buildJob(svg: string, config: ControllerConfig, spec: JobSpec): JobResult

interface JobSpec {
  readonly thickness: number;            // the material, mm
  readonly offset?: Point;               // added to every point, mm
  readonly layers: readonly LayerSpec[];
}
interface LayerSpec {
  readonly key: string;                  // a ToolLayer's key
  readonly head: number;
  readonly feed?: number;                // mm/s; 0 or absent: the tool's cutFeed
  readonly depth?: number;               // plunge below the top, mm; absent: thickness
}
type JobResult =
  | { ok: true; layers: JobLayer[] }
  | { ok: false; errors: readonly string[] }
```

**Parameters**

* `svg`: the SVG. It is read once, however many layers.
* `config`: the tools and the toolpath's tolerances (`quality`).
* `spec`: the material's thickness, and one entry per job layer, in the
  order they run. A tool layer may appear more than once.

**Return value**

The job layers, in `spec.layers`' order, or every error found. It checks:

* each key is a tool layer of the SVG, and the layer has something to cut;
* each tool has an `id`, and the chamber is one the tool has (0 for a tool
  without chambers);
* a plunging tool has a depth above 0;
* no tool is on two heads;
* 1 to 4 tools in all.

A malformed SVG is an error, and so is a curve the controller would refuse
([toolpath](../../system/toolpath.md)), naming its layer.

**Example**

```ts
const job = buildJob(svg, config, {
  thickness: 3,
  layers: [{ key: "knife", head: 0 }, { key: "pen", head: 1, feed: 40 }],
});
if (!job.ok) return show(job.errors);
const session = link.session(new JobSource(job.layers));
```
