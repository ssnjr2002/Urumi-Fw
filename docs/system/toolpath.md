# Toolpath

**Checked against `e088f31`.** To see what may have changed since:

```bash
git diff e088f31 -- web/src/toolpath/svg.ts web/src/toolpath/ web/src/packets/bezier.ts
```

Commands, replies, status keys and code names are written as the
[notation](../README.md#notation) says.

The toolpath is how the host turns one layer of an SVG drawing into the
`BEZIER` records of one layer of a job. It runs on the host only, as pure
computation: it reads no port and holds no state between calls.

This page follows a drawing through its four stages. It leaves the rest of
the job to other pages: the job, tool and layer headers around the records
are built by the job session ([jobs](jobs.md#7-the-hosts-job-session)), the
records travel over the [wire](wire.md), their bytes are in
[reference/packets.md](../reference/packets.md), and the controller turns
them into motion in the [planner](planner.md).

## 1. Overview: four stages

```text
SVG text
  │ load       toolpath/svg.ts       one walk of the document
  ▼
layers of Subpaths: cubics in mm, Y up, each open or closed
  │ clean      toolpath/clean.ts     one subpath at a time
  ▼
CleanSubpath: curves with defined end tangents, a join kind between each two
  │ annotate   toolpath/annotate.ts  one subpath at a time
  ▼
AnnotatedBezier[]: pieces the controller accepts, measured, flagged
  │ pack       packets/bezier.ts     one piece at a time
  ▼
62-byte BEZIER records
```

* **Load** reads the drawing and keeps only geometry: every line, arc and
  shape becomes cubic Bézier curves, in mm with Y up, grouped by layer. A
  **subpath** is one connected run of curves.
* **Clean** makes each subpath safe to measure: it drops curves of no
  length, gives every curve a direction at both ends, closes a nearly closed
  subpath, and marks each join between two curves **smooth** or **corner**.
* **Annotate** splits each curve into **pieces** the controller can queue
  without analysing them, measures each piece as the controller would, and
  flags where each piece sits in its contour. A subpath becomes a
  **contour**: the tool down from `START` to `END`.
* **Pack** writes each piece as one `BEZIER` record.

`prepareBezierJob()` (`toolpath/job.ts`) runs all four on one layer.

## 2. Decisions

* **Every element becomes cubics before anything else sees it.** Lines,
  quadratics, arcs and shapes all have exact or bounded cubic forms, and an
  affine map of a cubic is the cubic of its mapped control points, so the
  transform costs no accuracy. Arcs convert in the element's own
  coordinates, before the transform, so a skewed or stretched arc stays
  exact. Rejected: sampling curves into points on the host, which the old
  path did ([History](#11-history)).
* **Cleaning invents no geometry.** A curve moves only by a handle fix or a
  snap under `gapTol`; a join is kept as data, smooth or corner, and a
  corner becomes a `BREAK`. Rejected: bridging gaps with new curves, as the
  old repair stage did.
* **The host analyses every piece, and the controller only checks it.**
  `analyzeBezier()` is a copy of the controller's own analysis
  (`lib/planner/bezier.cpp`), same samples and same refusals, and the record
  carries its results. Rejected: the controller analysing each record, at
  about 0.6 ms a curve, too slow at stream rate.
* **The host sends no piece the controller would refuse.** A piece that
  can't be fixed by splitting stops the layer on the host with an error.
  Rejected: sending it and handling the `NACK`, which fails the job partway.
* **Float32 is part of the contract.** The host computes in doubles, and the
  record carries float32. No split leaves a piece shorter than `minLength`,
  and the controller's check allows a few float steps of slack, so a piece
  that passes in doubles also passes rounded. Rejected: checking in doubles
  only, which let 101 of a test drawing's pieces through that the controller
  refused.
* **Splits are exact.** Every piece is cut from its curve by de Casteljau,
  so pieces meet at the same point, and the same double gives the same
  float. Only a handle at a stop moves. Rejected: refitting curves.
* **The host orders the contours; the controller travels between them.**
  Contours go in document order, and a `START` record's `p0` is where the
  tool goes down. The lift, travel and plunge are the controller's, from
  its tool profiles. Rejected: the host sending travel moves.
* **The drawing is reflected once, then only moved.** Y flips from SVG's
  down to the machine's up in the viewport map; after that only
  translations apply (the offset, placement). A sign change anywhere after
  it would mirror the job.
* **A malformed file fails whole.** Bad path data or a bad transform throws.
  Rejected: drawing up to the error, as the SVG spec does, since a partial
  cut is worse than none.
* **Annotate's tolerances stay out of the config.** Only cleaning's
  `angleTol` and `gapTol` come from `quality`; annotate runs on its
  defaults ([Tolerances](#8-tolerances)). Rejected: exposing them before a
  machine needs to tune them.

## 3. Loading

`loadSvgPaths()` (`toolpath/svg.ts`) parses the text with the platform's
`DOMParser` and walks the document once, in document order. Under Node, the
caller supplies a `DOMParser` (the tests use `@xmldom/xmldom`).

### The walk

The walk carries two things down the tree: the transform so far, and the
layer name ([Layers](#4-layers-and-tool-layers)).

* **Containers it enters:** `<svg>` (a nested one adds its `x` and `y`),
  `<g>`, `<a>`, and `<switch>`, of which only the first child without a
  condition attribute (`requiredFeatures`, `requiredExtensions`,
  `systemLanguage`) is entered.
* **Shapes it draws:** `<path>`, `<circle>`, `<ellipse>`, `<rect>`,
  `<line>`, `<polygon>`, `<polyline>`.
* **Everything else draws nothing:** `<defs>`, `<clipPath>`, `<mask>`,
  `<symbol>`, `<marker>`, `<pattern>`, `<text>`, `<use>`, `<image>`.
* **Paint:** a shape whose fill and stroke are both `none` or `transparent`
  draws nothing. A shape with no fill named counts as filled, as in SVG.
  `style=` wins over the presentation attribute.

### Shapes into cubics

* **Path data:** every command, absolute and relative: `M`, `L`, `H`, `V`,
  `C`, `S`, `Q`, `T`, `A`, `Z`. Numbers after `M` repeat as `L`. `S` and `T`
  reflect the previous control point, or use the current point when the
  previous command wasn't of their kind. Arc flags may run into the next
  number (`a1 1 0 01 5 5`).
* **Lines** become cubics with handles at a third and two thirds.
  **Quadratics** are raised to cubics exactly.
* **Arcs** follow SVG 1.1 appendix F.6: radii too small for the chord are
  scaled up, a zero radius is a line, and an arc to its own start is nothing.
  Each arc becomes cubics of at most 90°, which stray from the ellipse by
  at most about 3e-4 of its radius.
* **Circle and ellipse:** four quarter cubics. **Rect:** four lines, or with
  `rx`/`ry` four lines and four arcs. **Polygon** closes; **polyline**
  doesn't. A circle, ellipse or rect with no size draws nothing, nor does a
  polygon or polyline under two points; a `<line>` of no length is left to
  cleaning.
* **Closed:** a subpath ended by `Z`, or a circle, ellipse, rect or polygon.
  `Z` adds a line back to the start; a line shorter than `zTol` (1e-6 mm by
  default) is dropped after the transform.

### Into mm, Y up

Each element's matrix is the viewport map × its parents' transforms × its
own `transform`; the matrix is applied to the control points.

* **The viewport map** scales `viewBox` units to mm and flips Y, so the
  `viewBox`'s bottom-left is (0, 0) and +Y is up.
* **Units:** `width` and `height` in `mm`, `cm`, `in`, `pt`, `pc` or `px`
  (1/96 in, also for a bare number). Without both, a `viewBox` unit is
  1 mm.
* **`transform`:** `matrix`, `translate`, `scale`, `rotate` (with or
  without a centre), `skewX`, `skewY`, composed left to right.

### Not supported

Read as if absent: `preserveAspectRatio` (the `viewBox` is stretched to the
size), `display` and `visibility` (hidden elements are drawn), and
anything `<use>` would place.

### Failures

These throw, and the whole file is refused:

* malformed path data: an unknown command, a missing number, numbers
  before any command;
* a `matrix()` without six numbers;
* a `width` or `height` it can't read, including a `%` size;
* a `viewBox` without four numbers, including one written with commas;
* a document whose root isn't `<svg>`.

## 4. Layers and tool layers

The loader returns a map from layer name to that layer's subpaths, in
document order. How a group names a layer depends on whether the caller
passes the config's tool names (`tools`).

* **With `tools`** (the job's case): a layer is the outermost `<g>` whose
  name is a tool, or `<tool>.<n>` for chamber n (counted from 1). It holds
  everything inside it. Groups with the same name join. A tool group inside
  another is merged into the outer one, with a warning in `warnings`. Other
  groups don't name anything, so an app's own groups (`g123`, `Group_3`)
  don't split a layer.
* **Without `tools`:** every `<g>` with an `inkscape:label` or an `id`
  names a layer, nested names joined by `/`.
* **Geometry outside any layer** is under `''`, which no tool cuts.

With `tools`, a group's name is its `inkscape:label`, else `serif:id`
(Affinity keeps the original there), else its `id`, and `toolLayer()` reads
it:

* Illustrator's escapes are undone: `_x2E_` is `.`, so `pen_x2E_2` is
  `pen.2`.
* A suffix exporters add to a repeated name is dropped: `_1_`
  (Illustrator), `_2` (Figma).
* A whole name that is a tool wins over splitting off `.<n>`.
* `.0` names no chamber. Whether chamber n exists is checked by the caller,
  against the tool's chambers.

`toolLayerKey()` gives the map key back: `pen`, or `pen.2`.

## 5. Cleaning

`cleanSubpath()` (`toolpath/clean.ts`) takes one subpath and returns a
`CleanSubpath`, or null when nothing is left. In order:

1. **Curves of no length go:** all four control points within `handleTol`
   of the start. The next curve starts where the last kept one ended.
2. **Every curve gets a direction at both ends.** A handle lying on its own
   endpoint (within `handleTol`) moves a third of the way toward the next
   control point that isn't on it. The controller refuses a curve without
   one.
3. **The closure is snapped.** A subpath ending within `gapTol` of its
   start ends exactly on it, carrying its last handle along, and is closed
   even if it had no `Z`. A closed subpath with a larger gap gets a closing
   line; that only happens when the loader's `zTol` is above `gapTol`.
4. **Each join gets a kind:** a **corner** where the direction turns more
   than `angleTol`, **smooth** otherwise. `joins[i]` is between `curves[i]`
   and the next; a closed subpath has one more, from its last curve to its
   first.

## 6. Annotating

`annotate()` (`toolpath/annotate.ts`) takes one `CleanSubpath` and returns
its pieces, each an `AnnotatedBezier`.

### What the controller refuses

`analyzeBezier()` measures a piece the way the controller's
`analyzeBezier()` does, with the same 128 samples and the same refusals:

* **`DegenerateHandle`:** a handle shorter than 1e-4 mm.
* **`Cusp`:** the curve nearly stops somewhere (its speed falls below
  1/1000 of its length).
* **`NonMonotonic`:** the fitted t(s) runs backwards somewhere.

A piece it accepts gets its **length**, its largest **curvature** (`κ_max`)
and rate of change of curvature (`dκ_max`), signed curvatures at both ends,
and **t(s)**: a cubic in the distance along the piece that gives the curve's
parameter, so the controller can step by distance.

### Splits

Each curve is split in four passes:

1. **Cusps:** where the speed has a minimum below the controller's stop
   limit. The piece after a cusp is a `BREAK`. Each piece's ends are then
   given a speed the controller accepts: a short handle is lengthened along
   its own direction, one with no direction moves a third of the way, as in
   cleaning.
2. **Inflections:** where the curve changes from turning left to turning
   right.
3. **Curvature ratio:** while the largest curvature is more than
   `kappaRatio` times the smallest, split where the curvature is their
   geometric mean. Below `kappaFloor` a piece counts as straight, so a line
   or an inflection doesn't split forever.
4. **Fit:** halve while t(s) is off by more than `fitTol`, or runs
   backwards.

Passes 2 to 4 never leave a piece shorter than `minLength`, and passes 3 and
4 halve at most `maxSplitDepth` times. A piece still past `kappaRatio` or
`fitTol` at either limit is sent anyway: the controller accepts it. A piece
still refused throws, and the layer fails.

### Flags

* **`START`** on the subpath's first piece, **`END`** on its last.
* **`BREAK`** on the first piece after a corner join, and after a cusp.
* The closing join of a closed subpath is ignored: `START` and `END`
  already stop there.

A run of pieces between two `BREAK`s, or a `BREAK` and the contour's end, is
a **stretch**: the path turns smoothly along it.

### Per stretch

Two numbers look ahead along the stretch, so the controller can plan
without seeing it whole:

* **`nextBreakDist`**, on a `START` or `BREAK` piece only: mm from its `p0`
  to the next `BREAK` or the contour's end, rounded up, at most 65535
  (meaning at least that far). 0 on every other piece. The controller uses
  it to time a duty lift for the last corner that fits
  ([jobs](jobs.md#duty-limits)).
* **`sweepMin` and `sweepMax`**, on every piece: the range the heading
  covers from the piece's `p0` to the end of its stretch, in whole degrees
  from the heading at `p0`, unwrapped, rounded outward, within an i16. A
  smooth join's small turn counts. A tool that steers uses it to choose
  which turn of A to start the stretch on ([planner](planner.md)).

The heading and its unwrap come from `heading.ts`, which samples as the
controller's `bezierHeading()` and `bezierTurn()` do, so the two agree to
float rounding; the controller's range check keeps a margin for that.

## 7. The layer's records

`prepareBezierJob(svgText, layer, options)` (`toolpath/job.ts`) runs the
four stages on one layer:

1. Load the whole drawing, with `tools` if given, and take `layer`.
2. Add `offset` to every control point (none by default).
3. Clean each subpath with `quality`'s `angleTol` and `gapTol`, and
   annotate it. A subpath cleaning leaves nothing of is skipped.
4. Pack every piece, in document order, contour after contour.

It returns:

* **`packets`:** one 62-byte record per piece, with seq 0. The job session
  stamps the seq as it sends ([wire](wire.md#7-the-job-stream)).
* **`bbox`:** the box around every control point. A cubic stays inside its
  control points, so the box holds the whole cut, sometimes with a margin.
* **`contours`:** the pieces before packing, one array per contour.

A layer that isn't in the drawing, or has nothing left to cut, comes back as
`{ error }`. A malformed drawing or a refused piece throws.

The demo calls it once per layer of the job, and hands the packets and
`bbox` to the job session's `JobSource`, which adds the headers and unions
each tool's boxes into its `TOOL_HDR`
([jobs](jobs.md#building-the-stream)).

## 8. Tolerances

| Value | Default | Stage | Set by |
| --- | --- | --- | --- |
| `zTol` | 1e-6 mm | load | the caller; `prepareBezierJob()` leaves the default |
| `angleTol` | 5° | clean | the config, `quality.angleTol` |
| `gapTol` | 0.01 mm | clean | the config, `quality.gapTol` |
| `handleTol` | 1e-4 mm | clean | default only; the controller's handle limit |
| `kappaRatio` | 2 | annotate | default only |
| `kappaFloor` | 1e-3 /mm | annotate | default only |
| `fitTol` | 1e-3, in t | annotate | default only |
| `maxSplitDepth` | 8 | annotate | default only |
| `minLength` | 0.05 mm | annotate | default only |

Three numbers in `annotate.ts` are the controller's own and must match
`lib/planner/bezier.cpp`: the 128 samples, the 1e-4 mm handle limit, and
the stop limit (1/1000 of the length). A host that differed would send
pieces the controller refuses.

## 9. I/O

### Into the toolpath

* **The SVG text**, from the operator's file.
* **The config's tool names**, so tool-named groups name the layers.
* **`quality.angleTol` and `quality.gapTol`**, from the config.
* **An offset**, in mm.

### Out of the toolpath

* **To the job session:** each layer's records and box.
* **To the caller:** the loader's layer names and warnings, which the demo
  shows so the operator can pick each layer's head, feed and depth.

Nothing here talks to the controller. Only the demo calls it.

## 10. Who writes what

| Value | Written by | Read by |
| --- | --- | --- |
| `Viewport` | `loadSvgPaths()` | the caller |
| layer name (`layers` key) | `loadSvgPaths()`, `toolLayerKey()` | `prepareBezierJob()`, the demo |
| `ToolLayer` (tool, chamber) | `toolLayer()` | the demo |
| `warnings` | `loadSvgPaths()` | the demo |
| `Subpath` (`curves`, `closed`) | `loadSvgPaths()` | `cleanSubpath()` |
| `CleanSubpath` (`curves`, `joins`, `closed`) | `cleanSubpath()` | `annotate()` |
| `JoinKind` (`smooth`, `corner`) | `cleanSubpath()` | `annotate()` |
| `BezierAnalysis` (`length`, `kappaMax`, `dkappaMax`, `ts`, `kappaStart`, `kappaEnd`, `fitError`) | `analyzeBezier()` | `annotate()`, `heading.ts`, `packBezier()` |
| `flags` (`START`, `BREAK`, `END`) | `annotate()` | `packBezier()`, the controller |
| `nextBreakDist` | `annotate()` | `packBezier()`, the controller |
| `sweepMin`, `sweepMax` | `annotate()`, through `bezierTurn()` | `packBezier()`, the controller |
| `contours` | `prepareBezierJob()` | the tests |
| `packets` | `packBezier()`, through `prepareBezierJob()` | `JobSource` |
| `bbox` | `prepareBezierJob()` | `JobSource`, for `TOOL_HDR` |
| seq byte | the job session (`stampSeq()`) | the controller |

## 11. History

### The MicroSegment path

Before Bézier records, the host did the planning. `svg/ingest.ts` read the
drawing in pixels, `repair.ts` (`enforceC1`) joined the curves, and
`flatten` sampled them into 26-byte MicroSegments that the controller
stepped without seeing a curve. The controller then took on planning
(`pico-planner.md` branches 1 to 5), and needed curves.

That path had faults it wasn't worth fixing:

* `ingest` ignored `transform`, broke on `A` and `T` (its tokenizer dropped
  the letters), made zero-length lines at `Z`, and its flat walk entered
  `<defs>` and `<clipPath>`.
* `enforceC1` bridged gaps with curves it invented, and its join kinds went
  to a log, not into data.

Fixing them in place meant keeping the pixel-space API and regenerating
goldens for a path that was going away. So branch 6 forked: new stages for
the Bézier path (6-load, 6-clean, 6a, 6b, 6c), with the old ones left
frozen and removed once Bézier jobs ran on the machine.

### The record

* **56 bytes** (6b): magic, flags, seq, the four points, length, `κ_max`,
  `dκ_max`, c2, c3, CRC. `p0` on every record gives the controller a
  continuity check and an absolute anchor. c1 and the end curvatures were
  left for the controller to derive. `dκ_max` was carried before anything
  read it, for the tangential A axis.
* **58 bytes** (`job-z.md` ZC1): `nextBreakDist`, for duty lifts at a
  corner.
* **62 bytes** (`job-a.md` AE): `sweepMin` and `sweepMax`, reversing
  planner-za's "no sweeps on the wire", so A can start a stretch on a turn
  that fits it.

### Float32 (6-fix)

The first cross-check found 101 of `fish.svg`'s 210 pieces passed the host's
analysis in doubles and failed the controller's `checkBezier()` once
rounded to float32: lines whose float curvature wasn't quite 0, curves with
their largest curvature at an end, and pieces a micron or two long. The fix
went on both sides: slack in `checkBezier()` scaled to the coordinates, and
`minLength` in annotate. `fish.svg` went to 171 pieces. The cross-check
stayed as a test ([Code](#12-code)).

### Tool layers (`demo-console.md` S1)

Every labelled group used to add to the layer's name, so an app's own
groups split a layer and the job couldn't find its tool. Layers became
tool-named groups, with exporters' escapes and suffixes undone.

## 12. Code

| File | Holds |
| --- | --- |
| `web/src/toolpath/svg.ts` | the walk, path data, shapes, transforms, viewport, tool layers |
| `web/src/toolpath/geometry.ts` | points, cubics, derivatives, `splitAt()`, affine maps, arcs |
| `web/src/toolpath/clean.ts` | cleaning |
| `web/src/toolpath/annotate.ts` | `analyzeBezier()`, splits, flags, the stretch numbers |
| `web/src/toolpath/heading.ts` | heading and unwrapped turn |
| `web/src/toolpath/job.ts` | `prepareBezierJob()` |
| `web/src/packets/bezier.ts` | `packBezier()` |

### Tests

* `web/test/svg/load.test.ts`, `web/test/toolpath/geometry.test.ts`,
  `clean.test.ts`, `annotate.test.ts`, `job.test.ts`: one case per rule,
  mostly inline SVG and curves; tool layers against
  `web/test/fixtures/svg_tool_layers.svg`.
* **The cross-check:** `web/test/port/cppRefBezier.test.ts` runs `fish.svg`
  through all four stages and writes every record to
  `test/data/bezier_records.txt` (only with `GEN_CPP_REF=1`).
  `test/test_planner/test_bezier.cpp` decodes each record and expects the
  controller's `checkBezier()` to pass it.
