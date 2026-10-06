# Coordinate system

The machine's frames, axis directions, offsets and the work frame, owned by
the Pico. Supersedes §1, §4 and §6 of docs/coordinate_frames_and_limits.md,
which predates the Pico controller. The homing config that sets the machine
frame is in docs/plans/controller-homing.md.

## Decisions

### Directions

* **+ is physical and fixed.** Looking at the bed from above, from the
  operator's side: X+ is right, Y+ is up (away from the operator). Z+ is up,
  away from the work (ISO 841).
* Rotary A (a rotation about Z; ISO would name it C): + is counter-clockwise
  seen from above (the right-hand rule about Z+), 0° is the knife's edge
  facing X+. A is then the path heading, `atan2(dy, dx)`, with no offset or
  sign flip.
* **`invertDir`** per axis is the wiring fix that makes the motor obey it.
  Commissioning checks it by jogging each axis +.
* Only the + direction can mirror a job. Where 0 sits only shifts the
  numbers, so it is free to choose.

### Machine frame (`MPos`)

* The anchor's position: the laser tip if one is fitted, else the head the
  config places at zero offset.
* 0 per axis is placed by homing: `parkPos` (controller-homing.md), absent
  meaning the all-positive front-left default.
* Signed: any corner may be the origin. On the current machine (X switch
  right, Y switch front):

  | Origin | X range | Y range | X `parkPos` | Y `parkPos` |
  |---|---|---|---|---|
  | front-left | 0 … +max | 0 … +max | +max | 0 |
  | front-right | −max … 0 | 0 … +max | 0 | 0 |
  | back-left | 0 … +max | −max … 0 | +max | −max |
  | back-right | −max … 0 | −max … 0 | 0 | −max |

  Rule: `parkPos` is 0 when the switch is at the origin end, +max when the
  switch is at the + end and the origin at the − end, −max the other way.
* Coordinates are tip positions, measured by the machine: the unmeasured
  distance from the switch striker to the anchor cancels, because every bed
  point (work origin, probe station, offsets) is taught by jogging a tip to it.

### Controlled point

* The selected head and tool: the anchor plus the head offset (XY) plus the
  tool offset (XY, and tool length on Z). Offsets are measured tip to tip.
* Selecting a head works like a G-code tool change: the offset changes, the
  gantry does not move, `WPos` jumps by the offset.
* Z and A are per head, with no shared-axis arithmetic.

### Work frame (`WPos`)

* One work offset to start (the G54 role): a point on the material, stored in
  machine coordinates, kept in flash, valid across re-homing (homing
  re-establishes the machine frame exactly).
* Default 0, so `WPos` equals `MPos` until set; set by "zero here" (the
  selected tip's position) or by coordinates.
* `WPos` = selected tip − work offset. Signed.
* Z0 is the material surface, set by probing (open).

### Stored positions

* Anchor positions in machine coordinates, the same for every head (the G28
  and G30 roles): park, and later a load position.

### Limits and reach

* Soft limits per axis: the range from homing (`[park, park ± maxTravel]`).
  The strip between the switch and the park position is outside it.
* Each tip reaches the anchor's range shifted by its offsets. Checked at job
  preflight (each tool's bbox, shifted by the work offset) and on every jog.
  `select` never moves, so it cannot fail on reach.

### Jobs from SVG

* One y reflection (SVG's y points down), then translations only: placement
  and the work offset. A sign change anywhere after the reflection mirrors
  the job.

### Ownership

* The Pico owns the frames. `status` reports `MPos` and `WPos`, each labelled;
  the host displays them. The web's `frames.ts` stays until its job and
  homing paths are removed; nothing new is built on it.

### Dropped

* Positive-only coordinates with + pointing away from the origin
  (`originDir`, `atOrigin`): an origin choice could mirror the frame.
* A reference frame between machine and work: the work offset (default 0)
  does its job.
* A fixed front-left origin: `parkPos` chooses it per axis.
* A left-most-anchor rule to avoid negative tip coordinates: signs are
  harmless.
* Positive-down Z and a host-side Z sign flip: ISO Z, since ranges are signed.

## Wire changes

* `status`: `MPos` and `WPos`, labelled; the selected head and tool.
* Commands: set the work offset (zero here, by coordinates, clear); go to a
  stored position.

## Branches

Proposed; each gets a full Plan section when its turn comes.

1. `feature/pico-frames`: the controlled point, the work offset in flash,
   `MPos`/`WPos` in `status`, stored positions. Depends on
   controller-homing branch 1 (`parkPos`). Also the soft-range check
   (`softLimits`, `[park, park ± maxTravel]`) deferred from
   controller-homing 1b: the bench `leg <node> park <count>` target is
   converted through the origin and refused outside it.

## Open questions

* Rotary A: whether angles wrap. Whether `web/src/toolpath/flatten.ts:70`
  takes headings before or after the y reflection (before means its angles
  are clockwise-positive today).
* Z: the tool length offset and the work Z0 by probing; the probe config
  (`switchXMm/YMm`, `tripMm`) restated in these frames.
* More work offsets (G55–G59), and which stored positions besides park.
* Soft limits for tips beyond the anchor's range (heads that reach past the
  bed edge).
