# Jobs

**Checked against `821fac6`.** To see what may have changed since:

```bash
git diff 821fac6 -- src/rp2350/core0/data_plane.cpp src/rp2350/core0/ops/layer.h src/rp2350/core0/ops/layer.cpp src/rp2350/core0/planner/queue.cpp src/rp2350/core0/controller/seq/place.cpp src/rp2350/core0/controller/cmd/place.cpp src/rp2350/core0/controller/seq/toolchange.cpp src/rp2350/core0/cmd/lifecycle.cpp src/rp2350/core0/ops/machine.cpp src/rp2350/core0/ops/observe.cpp src/rp2350/core0/ops/tools.cpp src/rp2350/core1/emit/follower.cpp src/rp2350/ipc/shared_state.h web/src/link/session.ts web/src/packets/layer.ts
```

Commands, replies and code names are written as the
[notation](../README.md#notation) says.

A job is one drawing cut out of a sheet of material. The drawing starts on
the host, as an SVG the operator has loaded. The host turns it into curves,
and sends those curves to the controller a few at a time while the machine
cuts. The controller never sees the whole drawing at once: it only ever holds
the part that is about to be cut.

That raises a problem. If the controller only sees the drawing piece by
piece, how can it know, before the first cut, that the drawing fits on the
machine at all? And if the operator pauses halfway, or the tool needs
changing, how does the host know where to pick up again? Most of what this
page describes is the answer to those two questions.

The answer to the first is that a job opens with a few headers that describe
it as a whole: how many tools it uses, and the area each tool will cover. The
controller checks those before anything moves. The answer to the second is
that every curve the controller runs is labelled with where it came from, so
the controller can always say "the job stopped at this curve", and the host
can resend from there.

In between, the job goes through a sequence of stages:

1. **Preflight.** The controller reads the headers and checks that the job
   can run on this machine, and that there is somewhere on the bed it fits.
2. **Placement.** The operator lays the material down, and the controller
   shows with a laser dot where the job will go. The operator moves the dot
   until it sits on the material's corner, and confirms.
3. **Cutting, layer by layer.** Each layer uses one tool. The controller
   checks that the right tool is mounted, and if it isn't, stops and waits
   for the operator to change it. Then it turns each curve into motion: lift
   the tool, travel to the start, lower it into the material, cut along the
   curve, lift again.
4. **The end.** The host tells the controller the job is over.

The operator can pause at any point during the cutting, and resume later
from where the cut stopped.

This page follows a job through those stages, then gathers the reference
material: the terms, the code, the full stream and the state table. It
doesn't cover how the host makes curves from an SVG (the
[toolpath](toolpath.md) page), how the packets travel over USB (the
[wire](wire.md) page; their bytes are in
[reference/packets.md](../reference/packets.md)), or how a curve becomes motor steps
(the [planner](planner.md) page).

## 1. Preflight

Preflight is the controller's chance to say no before anything moves. Once
the machine has started cutting, stopping halfway leaves a ruined sheet, so
everything that can be checked up front is checked here.

### What the job header checks

The job header (`JOB_HDR`) is the first thing the host sends. It doesn't
describe any geometry yet, so the controller uses it to check the machine
itself:

* **Is the machine free and configured?** It must be idle, with a valid
  config loaded.
* **Does the machine know where it is?** X, Y and every head's Z must have
  been homed. Without that, a position on the bed means nothing.
* **Are the soft limits on for X and Y?** Soft limits are the edges of the
  travel the controller allows. The fit check below needs them: without
  edges, every position "fits".
* **Is the laser there?** Placement points with the laser dot, so if the
  machine has a laser, its node must be answering on the bus.

If all of that holds, the job opens, in the `PREFLIGHT` state, and the
controller waits for the tool headers.

### What each tool header checks

A job uses one to four tools, and the host sends one tool header
(`TOOL_HDR`) for each. A tool header names the tool, the head it will be
mounted on, and the tool's **box**: the smallest rectangle that contains
everything that tool will cut, across the whole job.

The controller first checks that the tool can run at all:

* the tool is in the config, and hasn't already been named in this job;
* its head exists, and has a rotary A axis if the tool needs one (a knife
  that steers along the cut, or a revolver that picks a pen);
* if the tool is powered, its power node is answering on the bus;
* if a tool change on that head will need to turn A, that A has been homed.

Then it checks the harder question: is there anywhere on the bed this job
fits?

### Where the job can go

Picture the job's box on the bed. The operator can slide the job anywhere
during placement, but some positions would push part of the box past the
machine's edges. The controller works out, for each axis, the range of
positions where the whole box stays inside. It calls these the **valid
origins**.

A worked example on X. Say the machine's soft range on X is 0 to 1000 mm,
and the box runs from x = 0 to x = 300 mm in the drawing. The job's left edge
can then sit anywhere from 0 to 700 mm: any further right and its right edge
passes 1000. So the valid origins on X are 0 to 700.

Two things shift that range:

* **The tool's offset.** A tool on a second head doesn't sit where the first
  one does. If its head is 50 mm to the right, the whole range moves by
  50 mm.
* **A blade's offset.** A drag knife cuts slightly behind the point it is
  pushed from, so the range shrinks by that amount on each side.

Each tool gets its own range, and the job can only go where every tool's
range overlaps. So each tool header narrows a running range: the first
tool's range, then the overlap with the second's, and so on. The laser dot's
range is narrowed the same way, since the dot must be able to reach every
corner of the job too.

If the overlap ever becomes empty, there is nowhere the job fits, and the
controller refuses with `OUT_OF_RANGE`. The refusal says which axis ran out
of room, and on which side, so the host can tell the operator "too wide" or
"too far forward".

### A refusal ends the job

Any refusal during preflight closes the job, and the machine goes back to
idle. The host has to fix the job (or the machine) and start again from the
job header.

Holding the job open instead would leave the machine in a job that can't
run, waiting for something that will never change: the drawing doesn't get
smaller by waiting.

### Range checks after preflight

Preflight proves the job fits *somewhere*. Once the operator has placed it,
the controller checks again against where it actually went:

* **At each layer**, the tool header comes again, and its box is checked
  against the placed position. If it doesn't fit, the layer is refused, but
  the job stays open.
* **At each curve**, the controller checks every point, and the moves around
  it (the lift, the travel, the plunge), against the soft limits and the bed
  mesh. A curve that fails is refused with `NACK_SOFT_LIMIT` and never
  reaches the motors. The job runs what it already has, then stops and
  waits. This isn't an alarm: nothing actually went out of range, the
  controller just refused to start.

| Frame | Possible refusals |
| --- | --- |
| `JOB_HDR` | `BAD_STATE`, `NOT_HOMED`, `TOOL_UNAVAILABLE`, `OUT_OF_RANGE` (soft limits off), `JOB` (bad version or tool count) |
| `TOOL_HDR` | `TOOL_UNKNOWN`, `JOB` (named twice), `TOOL_UNAVAILABLE`, `NOT_HOMED`, `OUT_OF_RANGE` |
| `LAYER_HDR`, `BEZIER` before preflight ends | `JOB` |

The code: `acceptJobHdr()` and `preflightTool()` in `data_plane.cpp`;
`layerOrigins()` and `layerOriginsNarrow()` in `ops/layer.cpp`.

## 2. Placement

Preflight found the range of positions where the job fits. Placement is
where the operator picks one of them, by putting the material on the bed and
showing the controller where it is. This happens for every job.

The controller doesn't know the size or shape of the sheet. It never
measures it. Instead it points at a spot with the laser, and the operator
judges by eye whether the job lands on the material.

### What the dot marks

The dot marks the job's own corner: the bottom-left corner of the box
around everything the job will cut, across all its tools. It doesn't mark
the drawing's zero.

That choice matters because a drawing's zero can be anywhere. A drawing
might be laid out with its zero far to the left of the shapes, leaving an
empty margin. If the dot marked the zero, the operator would have to place
an empty point, possibly one off the edge of the bed. Marking the box's
corner means the dot is always on something that will be cut.

### What points

If the machine has a laser, the laser points. Otherwise the controller picks
a head that sits exactly where the gantry's reference point is (a head with
no offset), and that head's position is the pointer.

Whatever points, it has to be able to reach every corner of the job. That's
why preflight narrowed the pointer's range along with the tools'. The cost
is a strip of the bed the pointer can't reach, so a job can't be placed
there even if a tool could.

### How placement runs

When the last tool header is accepted, the job moves to the `PLACE` state
and the controller:

1. selects the pointer, and turns the laser on;
2. parks every Z, so nothing drags across the material while the dot moves;
3. moves the dot to a starting point, the **handoff**.

The handoff tries to save the operator work. If the previous job was placed
somewhere that is still a valid origin for this job, the dot goes back
there, on the theory that the material probably hasn't moved. If not, it
goes to the nearest valid position (or, with a work area configured, the
lowest valid one).

The operator then jogs the dot until it sits on the material's corner. The
jogs are limited to the window of valid positions, so the dot can't be moved
anywhere the job wouldn't fit.

### Setting and checking

The operator then sends a few commands:

| Command | When | What it does |
| --- | --- | --- |
| `place set` | before setting | makes the dot's position the job's corner |
| `place reset` | after setting | forgets it, so the dot can be moved again |
| `place go <xmin\|xmax> <ymin\|ymax>` | after setting | moves the dot to one of the box's corners |
| `place frame` | after setting | traces the outline of the box once |
| `place ok` | after setting | ends placement and starts the job |

`place set` is the only thing that changes where the job goes. It records
the dot's position as the work zero for X and Y, less the corner's offset in
the drawing. (A tiny clamp keeps it inside the valid range, since the motor
steps can round the dot just past the edge.)

`place go` and `place frame` are there for checking. The operator can send
the dot to each corner, or trace the whole outline, and see whether it stays
on the material.

`place ok` moves the job to `BUFFERING`, ready for its first layer, and
gives back whichever head was selected before the job started. If the job
leaves placement any other way (cancelled, aborted, an alarm), the laser
goes off and the selection is given back once the machine is at rest.

A `place` command while the dot is still moving is refused with `err busy`;
one sent at the wrong time is `err bad_state`.

The code: `controller/seq/place.cpp`, with `layerHandoff()` in
`ops/layer.cpp`.

## 3. Layers and tool change

A job is cut one layer at a time. A layer is everything one tool does: the
host groups the drawing by tool, and each group becomes a layer. A layer
opens with two headers: a **layer header** (`LAYER_HDR`) that sets how to
cut, and the tool header again, naming which tool and head will do it.

### The layer header

The layer header carries the settings for cutting:

* **the feed:** how fast to cut, in mm/s. Zero means "use the tool's
  default", and if the tool has none, the machine's;
* **the plunge depth:** how far into the material the tool goes;
* **the material's thickness**, which tells the controller where the top of
  the material is;
* **the chamber**, for a revolver tool that holds several pens: which one to
  use. Zero for any other tool.

It also carries two numbers that identify where in the job it starts: its
layer number, and the number of its first curve. These are what make
resuming possible (§5).

On its own, the layer header can only be checked for things that don't
depend on the tool: are the numbers finite and in a sensible range? So the
controller doesn't act on it yet. It holds it and waits for the tool header
that follows.

### The tool header, again

The tool header that follows names a tool already seen in preflight, and its
box is checked against where the job was actually placed (§1).

Then the controller combines the two headers with the config and works out
the layer's motion: the actual cut feed and plunge feed, how high the
material's top is, how deep to cut, and, for tools that turn, how the A axis
should behave. Some combinations are refused at this point. A plunge depth
that would cut further below the material than the tool's `maxOvercut`
allows is one; a tool that needs an A axis on a head without one is another.

### Is the tool ready?

Before the layer can start, the controller checks two things about the
head:

* **Is the right tool mounted?** The controller keeps track of which tool is
  on each head (the operator tells it with `mount`).
* **Has the head's Z been zeroed since that tool went on?** All the heights
  in a job are measured from the head's zero, so a tool that hasn't been
  zeroed would cut at the wrong depth.

If both hold, and the layer is on the head already selected, the layer
opens and its curves can follow.

If the layer is on a different head, the controller has to switch heads.
That means remapping which motors respond to which axis, which is done by
talking to the nodes over the bus. It can only do that while the bus is free,
which means the machine must be at rest, as in a paused job. Otherwise the
switch is treated like a missing tool: the job stops for a tool change.

### Tool change

When the tool isn't ready, the controller refuses the tool header with
`NACK_TOOL_CHANGE`. The refusal is deliberate: it means "not yet". The
header isn't used up. The host keeps it, and will send it again once the
tool is ready. That way the controller doesn't have to remember the layer
while it waits.

What happens next depends on whether the machine is still cutting the
previous layer:

* **If it's at rest**, the job moves straight to `TOOL_CHANGE`.
* **If it's still cutting**, the controller lets it finish everything
  already queued, and moves to `TOOL_CHANGE` when the queue runs out.

In `TOOL_CHANGE`, the controller runs a short sequence to get the head
ready for the operator:

1. selects the head;
2. marks the head as empty, since the operator is about to take the tool
   off;
3. parks every Z, out of the way;
4. on a head whose tool holder locks with the A axis, turns A to 0 so the
   holder can open;
5. moves to the configured `mount` position, where the operator can reach
   the head.

(If the tool turns out to be ready by the time the sequence starts, it
skips all that and goes straight back.)

Then the job waits in `TOOL_WAIT`. The operator swaps the tool, tells the
controller with `mount`, and zeroes Z. They can jog and use `goto` while
they do it.

The only way out of `TOOL_WAIT` is the host sending the header again. If the
right tool is now mounted and zeroed, the job goes back to where it was
(buffering, or paused if the change started from a pause), and the header is
taken as normal. If not, it gets `NACK_TOOL_CHANGE` again and the job keeps
waiting.

The code: `takeLayer()`, `layerTool()` and `acceptLayer()` in
`data_plane.cpp`; `layerResolve()` in `ops/layer.cpp`;
`controller/seq/toolchange.cpp`; `plannerHoldForTool()` in
`planner/queue.cpp`.

## 4. Records into motion

Once a layer is open, the host sends its curves. Each one arrives as a
**record** (`BEZIER`): one cubic Bézier curve, plus a few flags and numbers
the host worked out in advance.

The curves of a drawing come in **contours**: a run of curves the tool cuts
in one go, without lifting. A circle is one contour; so is a rectangle. Each
record carries flags that say where it sits in its contour:

* **`START`** on the first curve of a contour;
* **`END`** on the last;
* **`BREAK`** on the first curve after a sharp corner, where the path
  changes direction abruptly.

These flags are how the controller knows when to lift the tool, lower it,
and turn it.

### What each record becomes

The controller doesn't run a record as just a curve. It wraps it in the
moves the cut needs around it:

* **A `START` record** gets a lift to a safe height, a travel to where the
  curve begins, the tool switched on (for a powered tool), and a plunge
  into the material. Then the curve.
* **A `BREAK` record**, for a tool that steers (a tangential knife), gets a
  turn of the blade to the new direction before the curve. If the corner is
  sharper than the tool's `cornerAngleDeg`, the blade can't turn in the
  material, so the tool lifts, turns, and plunges again.
* **An `END` record** gets the curve, then a lift, and the tool switched off.
* **Any other record** is just the curve.

All the moves for one record are checked first, and only then pushed onto
the queue together. A record either goes in whole or not at all.

The first record of a layer is always treated as a `START`, whatever its
flags say: the tool has to get there somehow.

### Heights

All heights are measured from the head's zero, which is the bed. The top of
the material is the bed plus the material's thickness. The cut goes the
layer's plunge depth below that. The safe height for travel is the top plus
the machine's `zClearance`.

### Checks on the curves

The controller checks that the flags make sense. A `START` while a contour
is still open, or a curve without a `START` when no contour is open, is
refused with `NACK_BAD_CURVE`. So is a curve that doesn't begin where the
previous one ended, apart from a tiny tolerance after the machine has
stopped and restarted.

### Tools that turn

A tangential knife has to point along the cut. The controller turns the A
axis to follow the curve's direction, picking whichever equivalent angle
keeps the whole stretch between corners within the tool's cable range: a
knife with a cable can only turn so far before the cable winds up. When a
smooth stretch would go past that limit, the tool lifts, unwinds by whole
turns, and plunges again in the same direction. A revolver turns to its
chamber during the first travel and stays there. The details are on the
planner page.

### Duty limits

Some tools can only run for so long before they need a rest. The controller
keeps a running estimate of how long the tool has been on. At a corner, if
the next stretch would take it past its limit, it plans a stop: lift, switch
off, switch on again, plunge, carry on. A second check runs as the job cuts
and catches anything the estimate missed. Both are on the planner page.

### Labels

Every move pushed for a record is labelled with that record's layer and
number. The lifts are also labelled with why they happen (end of a contour,
a corner, a duty stop and so on), which the host can read in the status.
The lift after an `END` is labelled with the *next* record's number, so a
pause during that lift resumes at the next contour, not the one just
finished.

The code: `plannerQueueRecord()` in `planner/queue.cpp`; `acceptBezier()`
in `data_plane.cpp`.

## 5. Buffering, pause and resume

### Buffering

The host sends records over USB, which is fast but not perfectly steady. If
the controller started moving the moment the first record arrived, it could
run out of curves mid-cut and have to stop. So a job first waits, in the
`BUFFERING` state, until enough is queued.

Core 1, which runs the motion, starts the job on the first of:

* the queue is full;
* the queue holds at least `machine.buffer.queueTime` seconds of motion;
* nothing new has arrived for `machine.buffer.idleTime` (the host has sent
  everything it has, for now).

A job buffers at its start, after a resume, after a tool change, and
whenever the queue runs dry while cutting. When the queue runs dry, the
machine stops where the records ran out, and stays there, tool down and
still on, until more arrive. It doesn't lift: lifting and plunging again
would leave a mark in the cut. The exception is a lost host (§6): then the
job pauses.

### Pause

The operator pauses with `pause`. It's accepted while a layer is open and
the job is buffering, cutting, or waiting out a tool's rest.

A pause happens in three steps:

1. **Core 1 brakes** along the path, in the `HOLDING` state. It slows down
   along the curve it is cutting, rather than stopping dead.
2. **At rest, Core 1 records where it stopped:** which record, and how far
   along it. This is the **resume point**. Then it empties the queue and
   tells Core 0.
3. **Core 0 lifts the tool** to the safe height and switches it off. When
   that's done, the job is `PAUSED`.

Emptying the queue is what makes the machine usable while paused. The
operator can jog or probe, using the same queue, without disturbing
anything the job still needs: whatever the job still needs, the host will
send again.

### Resume

To resume, the host sends the layer header again, naming the resume point:
the layer, and the record it stopped in. The controller only accepts a
layer header that matches its resume point exactly. Anything else is
refused. That way a stray record, left over from before the pause, can't
start the machine moving again.

There's no `resume` text command for the same reason: the header carries
the exact point, and the controller can check it.

When the header matches, the controller checks the tool is still ready (the
operator might have swapped it while paused), and the job goes back to
`BUFFERING`. The first record is cut starting 2 mm before where it stopped,
travelled to and plunged into as if it were a `START`. Starting a little
early means the new cut overlaps the old one, with no gap.

The code: `cmdPause()` in `cmd/lifecycle.cpp`; `landHeld()` and
`bufferDone()` in `core1/emit/follower.cpp`; `held()` in `ops/observe.cpp`;
`plannerQueueLift()`, `plannerLandPaused()` and `plannerOpenLayer()` in
`planner/queue.cpp`.

## 6. Ending and failures

### How a job ends normally

A job doesn't end when its last curve is cut. The controller doesn't track
how many curves a job has. When the host has nothing more to send, the queue
runs dry and the job waits in `BUFFERING`, the same as if the host were
slow.

It's the host that ends the job, by sending `cancel`. `cancel` only works
when the machine is at rest: during preflight, placement (with the dot
still), buffering, paused, or waiting for a tool (with nothing moving). It
empties the queue, switches the tool off, and returns the machine to idle.
Sent while moving, it's refused with `err bad_state`.

### Stopping while moving

To stop a moving job, the host sends **ABORT**, a single byte. ABORT:

1. brakes along the path (`ABORTING`);
2. at rest, lifts the tool once and switches it off;
3. closes the job, and the machine is idle.

From the moment ABORT arrives until the machine is idle, the controller
refuses every job packet with `NACK_ABORTING`. That tells the host to wait,
rather than treating it as an error. Any packets that were in flight when
the abort happened are turned away, rather than slipping in after the brake.

### When something goes wrong

* **An alarm**, such as a node fault, ends the job the same way as ABORT:
  brake, lift, idle.
* **An e-stop** ends the job at once. Core 1 drops the queue and the
  machine stops without lifting, since an e-stop means nothing should move.
* **If the host goes away,** the job pauses once the queue runs dry. While
  a job is open the controller counts the host lost when DTR drops (the
  port is closed, the tab closed, or the cable pulled) or nothing has
  arrived from it for 3 s (`HOST_TIMEOUT_MS`, against the host's 100 ms
  poll), which catches a host that hangs with the port open. A silence ends
  when the host is heard again: with DTR up throughout, the port stayed open
  and only the same session can speak. A DTR loss holds until the job pauses
  or ends, even if a host reopens the port meanwhile: that is a new session,
  without the stream, which waits for the pause to resume. What is queued
  is still cut. Then, in `BUFFERING` with the host lost, the job pauses as
  an operator pause at rest does: the tool lifts and switches off, and the
  job lands `PAUSED`. The resume point is the end of the last curve cut, so
  a resume overlaps it by 2 mm, or the next record if the last block wasn't
  a curve. `get pausereason` reads 1 for this pause, 0 for an operator's;
  `get host` reads 0 while the host is lost. The code: `core0/host_watch.cpp`,
  and `enter()` and `landHeld()` in `core1/emit/follower.cpp`.
* **If the power goes**, the controller loses the job entirely. It keeps
  nothing in flash, because writing flash stalls both cores and the
  controller never does it during motion. The host is the only record of the
  job. It reads which record is being cut from every status poll, so it
  knows roughly where the job stopped. The job can't be continued: the
  material may have moved, and placement only marks a box's corner, so a
  restart can't be put back on the cut already made.

### Starting again

After an alarm or a power loss, there is no resuming. The host starts a new
job, with a fresh job header and the layers from where the last one left
off. Preflight and placement run again.

The code: `cmdCancel()` in `cmd/lifecycle.cpp`; `jobAbort()`,
`machineTeardown()` and `abortSettle()` in `ops/machine.cpp`.

## 7. The host's job session

Everything above is the controller's side. On the host, a job is run by a
**job session**, which builds the stream and sends it, and follows the
controller through each stage.

### Building the stream

The session's source of packets, `JobSource`, holds the job's layers in
memory. From them it builds:

1. the job header;
2. one tool header per tool, its box covering every layer that tool cuts;
3. for each layer: its layer header, its tool's header, and its records.

It refuses a job with more than four tools, or one that puts the same tool
on two heads.

It also works out the **job id** that the job header carries: a CRC32 over
every frame of the job in the order sent, leaving out each frame's seq byte
and CRC8 and the header's own id field. The same layers always give the same
id, and changing any record changes it.

For a resume, it builds a different stream: the layer header for the resume
point on its own, the records from that point on, and then the layers after
it, as normal.

It can also run in **manual** mode, where each layer waits until the host
releases it. That lets an operator check each layer before it starts.

### Sending, and holding

The session sends the job header and tool headers first. Then it stops and
waits until placement is done. It can't send the layers any earlier,
because the controller refuses them during placement.

From then on it streams the layers. But at several points the controller
stops taking records, and the session has to stop sending and wait too.
These waits are the session's **holds**. During a hold it sends nothing but
status requests, so it can see when the controller is ready again.

| Hold | Why it starts | How it ends |
| --- | --- | --- |
| `place` | preflight accepted; the operator is placing | the status shows `BUFFERING`, after `place ok` |
| `paused` | the controller refused a record with `NACK_PAUSED`, or the host itself paused, or the status shows `PAUSED` | the host calls `resume()`: the session restarts the stream at the resume point the status reports |
| `refused` | a resume header didn't match the resume point (`NACK_JOB`) | as `paused` |
| `tool` | `NACK_TOOL_CHANGE` | once the controller is in `TOOL_WAIT`, the host calls `resume()` and the header is sent again |
| `silent` | no reply for 3 seconds | the session resends straight away; if the status shows `PAUSED` (the controller lost the host meanwhile) it holds `paused` instead |

Its timers run on a dedicated Worker in a browser, so a tab in the
background keeps polling every 100 ms instead of being slowed to once a
second or worse, which the controller would take for a lost host.

### Coming back to a paused job

When the host has gone away and come back, for example after the tab
crashed or the cable was pulled, the controller has paused the job (§6). A
new session takes it over with `link.attach(source)` instead of opening a
job: it checks that the controller is in a job on one of the source's
layers, and that `get jobid` reports the source's job id. A job still
cutting is waited on until it lands `PAUSED`, which it does once the queue
runs dry, since the controller holds the loss of a closed port. Then it
holds `paused`, and `resume()` carries on from the controller's resume point
as after any pause. If any check fails it stops with `FATAL_JOB_MISMATCH`,
having sent nothing but status requests and `get jobid`, so a different job
file can't be resumed onto the cut.

### Ending

The session never ends by itself. When everything has been sent and
acknowledged, it keeps polling the status until the host calls `cancel()`,
which sends `cancel`, or `end()`, which sends ABORT. Then it waits for the
job to close.

### When the session gives up

Some refusals mean the job itself is wrong, and resending won't help. The
session stops with an error for:

* any refusal during preflight;
* a curve the controller won't take (`NACK_BAD_CURVE`), or one out of range
  (`NACK_SOFT_LIMIT`);
* a tool or range refusal at a layer, or `NACK_BAD_STATE`;
* too many corrupted packets in a row.

It also stops if it **loses the job**: the controller has left the job
without the session asking it to, or reports a layer the job doesn't have.
This usually means the controller rebooted. Resending into a rebooted
controller would be worse than stopping: it would accept the packets as
duplicates and quietly do nothing with them. The session's result then
carries the last layer and record it saw (`lastLayer`, `lastRecord`), so the
operator knows how far the job got.

The code: `JobSource` and the job half of `Session` in
`web/src/link/session.ts`; the header packing in
`web/src/packets/layer.ts`.

## 8. Glossary

```text
job                one drawing; its headers and records
└─ layer           one tool, one set of feeds and heights
   └─ contour      the tool down: START … END; travel between contours
      └─ stretch   between corners (BREAKs): the path turns smoothly
         └─ piece  one cubic Bézier
```

The same piece of geometry has a different name depending on where it is:

| Where | Name | In the code |
| --- | --- | --- |
| on the host | piece | `AnnotatedBezier` |
| on the wire | record | `BEZIER` |
| in the controller's queue | block | `planner::Block` |

The queue also holds blocks no record sent: the lifts, travels, plunges and
turns wrapped around each record (§4), and the lifts a pause or abort adds.

Other terms used on this page:

* **Box:** the rectangle around everything one tool cuts in the job (§1).
* **Valid origins:** the positions where the job fits on the bed (§1).
* **Handoff:** where the placement dot starts (§2).
* **Resume point:** the record a pause stopped in, and how far along it
  (§5).
* **Hold:** a wait in the host's job session (§7).

## 9. Hardware and code

### What the job reads from the machine

* **The heads and their tools:** which tool is mounted on each head, each
  head's offset, and whether it has an A axis.
* **The pointer:** the laser, or a head with no offset.
* **`machine.positions.mount`:** where a tool change sends the head.
* **The soft limits and the work area:** the edges a job must fit inside.
* **The tool's power node,** for a powered tool, which must answer on the
  bus.

### Where the code is

| Part | Files | What it does |
| --- | --- | --- |
| The host | `web/src/link/session.ts`, `web/src/packets/layer.ts` | builds the stream, sends it, follows the controller's holds |
| Core 0 | `core0/data_plane.cpp` (the job half), `core0/ops/layer.*`, `core0/planner/queue.cpp` (the job half), `core0/controller/seq/place.*`, `core0/controller/seq/toolchange.*`, `core0/cmd/lifecycle.cpp`, `core0/ops/machine.cpp`, `core0/ops/observe.cpp`, `core0/ops/tools.*` | checks the headers, opens layers, turns records into moves, runs placement and tool changes, pauses and ends jobs |
| Core 1 | `core1/emit/follower.cpp` | the job states it decides while running: running, holding, dwelling, the resume point |

### Tests

* `test/test_config/test_layer.cpp`: resolving a layer, the feed fallback,
  A for turning tools, plunge limits, the valid origins and how they narrow,
  the handoff.
* `web/test/wire/link/job-session.test.ts`: the job session against the
  Sim's model of the controller: a job's life, pause and resume, the
  headers, manual layers, placement, tool change.
* `web/test/wire/format/layer.test.ts`: the header packing.

The controller's own job code, in the data plane and
`plannerQueueRecord()`, has no tests. The Sim reimplements it for the host's
tests, so those tests check the host against the Sim, not against the
firmware.

## 10. The stream

The full stream for a job:

```text
JOB_HDR                job id, record count, thickness, tool count (1–4)
TOOL_HDR × n           one per tool, any order: tool, head, box
LAYER_HDR(L, N)        layer L from record N: chamber, feed, plunge, thickness
TOOL_HDR               the layer's tool, head and box
BEZIER …               records N, N+1, … of layer L
LAYER_HDR(M, 0) …      the next layer, with its TOOL_HDR, and so on
```

A resume from a pause is `LAYER_HDR(L, N)` on its own, then the records
from N.

Why each header carries what it does:

* **The box is per tool, over all its layers,** so preflight can check the
  whole job before anything moves. It's four numbers, not a width and
  height, because where it sits relative to the drawing's zero matters.
* **The head is named only in the tool header.** The controller keeps track
  of the current tool and head, not a list of which tool goes on which head.
* **The layer header names its layer and first record.** Records aren't
  numbered on the wire. Over USB the packets arrive in order with no gaps,
  so the controller counts them from the layer header's first record. That
  count is what the resume point names.
* **Tools are named by their config `id`,** not by their position in the
  config, so reordering the config doesn't change what a job means.

The controller keeps the job header's job id for the life of the job and
reports it as `get jobid`, but doesn't check it: the host sets it to a hash
of the job, so a host that reconnects can tell whether a paused job is its
own. The record count is sent but not read.

## 11. States

A job runs in the JOB context, and `jobReason` says which stage it's in.
Core 0 sets the stages it's asked for; Core 1 sets the ones it decides
while running the queue.

| From | To | Set by | When |
| --- | --- | --- | --- |
| idle | `PREFLIGHT` | Core 0 | the job header is accepted |
| `PREFLIGHT` | `PLACE` | Core 0 | the last tool header is accepted |
| `PREFLIGHT` | idle | Core 0 | any preflight refusal |
| `PLACE` | `BUFFERING` | Core 0 | `place ok` |
| `BUFFERING` | `RUNNING` | Core 1 | enough is queued |
| `RUNNING` | `BUFFERING` | Core 1 | the queue ran dry, or a late block made it stop |
| `RUNNING` | `DWELL` → `RUNNING` | Core 1 | a plunge waits for the tool's rest |
| `BUFFERING`, `RUNNING`, `DWELL` | `HOLDING` | Core 1 | `pause` |
| `HOLDING` | `PAUSED` | Core 0 | at rest, after the lift |
| `PAUSED` | `BUFFERING` | Core 0 | a layer header at the resume point |
| `BUFFERING`, `PAUSED` | `TOOL_CHANGE` | Core 0 | a header whose tool isn't ready, with nothing queued |
| `RUNNING` | `TOOL_CHANGE` | Core 1 | the same, once the queue runs dry |
| `TOOL_CHANGE` | `TOOL_WAIT` | Core 0 | the head is at `mount` |
| `TOOL_CHANGE`, `TOOL_WAIT` | `BUFFERING` or `PAUSED` | Core 0 | the tool is ready |
| any, moving | `ABORTING` | Core 1 | ABORT or an alarm |
| `ABORTING` | idle | Core 0 | at rest, after the lift |
| at rest | idle | Core 0 | `cancel` |

Records are only taken while the job is buffering, running or dwelling.

A jog during a job (while placing, waiting for a tool, or paused) shows as
JOGGING, and the job's stage is kept underneath until the jog ends
([controller](controller.md), Context).

## 12. I/O

**Host to Core 0**, over USB:

| What | Channel | Reply |
| --- | --- | --- |
| `JOB_HDR`, `TOOL_HDR`, `LAYER_HDR`, `BEZIER` | data plane | ACK, or NACK with a reason and a detail byte |
| ABORT | data plane, one byte | none; the status shows idle |
| `pause`, `cancel`, `place …` | text | `ok` or `err <why>` |
| `mount`, `goto`, `jog`, `wzero` during a hold | text | as outside a job |
| the job's stage, the record being cut, the lift reason | `STATUS_RSP` | |

**Core 0 and Core 1**, through shared state ([cores](cores.md)):

| What | Direction |
| --- | --- |
| the queue, and that the job owns it (`RING_JOB`) | Core 0 → Core 1 |
| `jobReason` | both, as in §11 |
| `pauseRequested`, `abortRequested`, `toolChangePending` | Core 0 → Core 1, which clears them |
| the tool's power and duty settings (`jobPower`) | Core 0 → Core 1 |
| where the job is (`jobExec`, `jobLift`), where a pause stopped (`jobResume`) | Core 1 → Core 0 |
| "paused at rest" (`FACT_HELD`), "queue drained" (`FACT_RING_DRAINED`), "duty stop needed" (`FACT_DUTY_HELD`) | Core 1 → Core 0 |

## 13. Who writes what

| Value | Written by | Read by |
| --- | --- | --- |
| `machineContext` (JOB) | Core 0: opened at the job header, closed by `cancel`, the end of an abort, a preflight refusal, an e-stop | both cores, the host |
| `jobReason` | Core 0 and Core 1 (§11) | both cores, the host |
| `ringOwner` | Core 0 | Core 1 |
| `pauseRequested` | Core 0 (`pause`) | Core 1, which clears it |
| `abortRequested` | Core 0 (`jobAbort()`) | Core 1, which clears it |
| `toolChangePending` | Core 0 (`plannerHoldForTool()`) | Core 1 |
| `jobPower` | Core 0, when a layer opens | Core 1 |
| `jobExec`, `jobLift` | Core 1 | Core 0, the status |
| `jobResume` | Core 1 | Core 0, to check a resume |
| the current tool and head (`curTool`, `curHead`), the held layer header | Core 0, data plane | Core 0 |
| the valid origins (`preX`, `preY`) and the job's box | Core 0, during preflight | placement |
| the work zero for X and Y | Core 0, `place set` | everything that moves |
| the last placed corner (`lastOff`) | Core 0, `place set` | the next job's handoff |
| which tool is on each head | `mount`, a tool change | preflight, layers |

## 14. Decisions

### The host

* **The host is the job's only lasting record.** The alternative was the
  controller saving the resume point to flash. It was rejected because
  writing flash stalls both cores, and the controller does none during
  motion.
* **The host ends the job.** The controller runs dry and waits. Ending on
  the last record is still open: records are numbered per layer, so the job
  header's single record count doesn't say which record is last, and how a
  job's end should be marked hasn't been settled. Until it is, nothing reads
  the record count.
* **A lost job stops the session.** The alternative, resending into
  whatever answers, would have a rebooted controller accept the packets as
  duplicates and do nothing with them. A job paused because the host was
  lost isn't lost: a new session attaches to it and resumes.
* **A job lost to a power cut isn't recovered.** The alternative, a partial
  job from the last record seen, needs the material realigned to the cut
  already made, and placement can't do that: it marks a box's corner by eye,
  and doesn't see rotation. Revisit if placement gets registration marks.
* **The job id is the host's to check.** The host hashes the job into it; the
  controller only keeps and reports it. A controller check would stop
  nothing a host check doesn't: neither can verify the geometry.

### The controller

* **A job is one JOB context with a stage byte,** the same shape as homing
  and probing. The alternative was separate RUNNING and PAUSED states.
* **The head is named in the tool header.** The alternative, tried and
  dropped, had the controller pick the head from which tool is mounted
  where. But a box can only be checked against a head in preflight if the
  header says which head; otherwise the controller would have to keep every
  box, or a list of tools and heads, until each tool is mounted.
* **Preflight refuses rather than waits.** The host fixes the job and starts
  again. Waiting would leave a job open that can't run where it is.
* **A tool header that needs a tool change is refused, not kept.** The host
  sends it again when the tool is ready. The alternative, the controller
  keeping it until then, adds job state the controller doesn't otherwise
  need.
* **Records aren't numbered on the wire.** USB keeps them in order, so the
  layer header's first record and a count are enough.

### The job's flow

* **The dot marks the job's corner, not the drawing's zero.** So a job never
  has to reach its zero, which may be off the bed. The alternative, placing
  by the zero, needs the zero on the machine.
* **The pointer must reach the whole job.** It costs a strip of the bed the
  pointer can't reach. Pointing with a head instead is left until that strip
  matters.
* **A pause empties the queue.** That frees it for jogging, with no second
  queue. The alternatives were setting the queue aside, and the planner's
  own restart in place (`restartFrom`).
* **Resume is a layer header at the resume point,** so a stray record can't
  restart motion. The alternative was a `resume` text command.
* **A dry queue doesn't lift,** unless the host is lost. The job waits in
  place and carries on without a mark in the cut. A lost host (DTR down, or
  3 s silent) pauses it instead (§6), so the tool isn't left down in the
  material. The alternative, lifting on a dry queue after a timeout, would
  mark the cut for a host that's merely slow.
* **A lost host only acts on a dry queue,** for now. In every other stage the
  tool is already up or the operator holds it.

## 15. Code

| File | Sections |
| --- | --- |
| `web/src/link/session.ts` | 7, 10 |
| `web/src/packets/layer.ts` | 7, 10 |
| `src/rp2350/core0/data_plane.cpp` | 1, 3, 4, 5, 6 |
| `src/rp2350/core0/ops/layer.h`, `layer.cpp` | 1, 2, 3 |
| `src/rp2350/core0/planner/queue.cpp`, `queue.h` | 3, 4, 5, 11 |
| `src/rp2350/core0/controller/seq/place.cpp`, `place.h` | 2 |
| `src/rp2350/core0/controller/cmd/place.cpp` | 2 |
| `src/rp2350/core0/controller/seq/toolchange.cpp`, `toolchange.h` | 3 |
| `src/rp2350/core0/cmd/lifecycle.cpp` | 5, 6 |
| `src/rp2350/core0/ops/machine.cpp` | 6 |
| `src/rp2350/core0/ops/observe.cpp` | 5 |
| `src/rp2350/core0/ops/tools.cpp`, `tools.h` | 3 |
| `src/rp2350/core1/emit/follower.cpp` | 5, 11 |
| `src/rp2350/ipc/shared_state.h` | 11, 12, 13 |
