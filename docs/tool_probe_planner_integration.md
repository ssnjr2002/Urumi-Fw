# Tool-Height Probe: Planner Integration

**Status:** Agreed design, not yet implemented
**Scope:** Pico firmware (`src/rp2350/`) and web host (`web/src/`)

---

## 1. The model

A probe gives one number per head: the machine-frame Z at which the mounted
tool's tip opens the bed switch. Lowering it by the switch's `tripMm` gives the
**mat surface** for that tool, and every Z the job needs is measured up from the
mat:

| height | above the mat | meaning |
|---|---|---|
| cut   | `plunge ? 0 : materialMm` | where the tool works |
| clear | `materialMm + clearanceMm` | nothing touches the material |
| lift  | `clear − cut` | what a block lowers and raises by |

`tripMm` is signed, per switch, in `ProbeConfig`: the switch's trip point above
the mat surface. Positive means it trips above the mat, so the tool stops short
of it. Negative means a recessed switch that trips below the surface. Zero means
contact is the mat.

- `plunge` is per tool: `true` goes through the material to the mat (knife),
  `false` works on the material's top surface (pen).
- `materialMm` is per job, entered before the job starts. A job without it does
  not start.
- `clearanceMm` is machine config, the margin above the material (1–2 mm).

So a knife lifts `materialMm + clearanceMm` and a pen lifts `clearanceMm`.

### What the probe value is — and is not

The value is the machine Z read by `getpos` after the **latch** leg ends on
contact. It is **not** `getstate`'s `psteps=`: that is the last leg's emitted
step count, a relative number, and it is gone once `probe_end` closes the
session.

Machine Z steps increase **downward** on this machine (park 4800, bed contact
~47400 on head 0), so "above contact" means *fewer* steps. The host converts
through `stepsPerUnit` and `invert` in one place, never inline.

---

## 2. Pico: `nodeProbed` is the authority

The Pico holds the contact height and whether it is valid. The host asks; it
does not keep its own copy.

```cpp
// position.cpp
static uint16_t nodeProbed = 0;               // bit n = probeZ[n] is valid
static int32_t  probeZ[BUS_ADDR_MAX + 1];     // contact, machine-frame steps
```

Keyed by **bus node**, like `nodeHomed`, so a parked head keeps its probe across
`axis_map` swaps. Head 0 Z is node 3, head 1 Z is node 6.

```cpp
bool probeValidZ(void) {
    uint8_t zNode = slotNodeAt(SLOT_Z);
    return zNode != SLOT_NONE && (nodeProbed & (1u << zNode));
}
```

### Invariant: `nodeProbed ⊆ nodeHomed`

A contact height is a machine-frame number, so it is only meaningful against the
datum it was measured in. Every path that changes or loses a datum clears the
probe for that node:

| event | hook | today |
|---|---|---|
| datum lost (node reset, fault, `NODE_FLAG_DATUM` gone) | `originInvalidate(n)` | add clear |
| e-stop, soft reset, all datums wiped | `originInvalidateAll()` | add clear |
| **re-home** — a new datum replaces a valid one | `originRecord(n, …)` | **add clear** — it overwrites today without touching anything else |
| a probe session opens on this Z | `probeBegin()` | add clear |

No extra state for failure. A failed leg tears the session down (tool_probe.md
§5.11.2), the host never reaches `setprobe`, and the bit stays clear.

---

## 3. Commands

### `setprobe <z_steps>`

Records the contact height for the Z node currently in slot 2.

Refused, in order:

| condition | reply |
|---|---|
| not `STATE_IDLE` / `STATE_PAUSED` | `err bad_state` |
| probe session open | `err probing` |
| slot 2 unbound | `err unbound` |
| Z not homed | `err not_homed` |

```cpp
probeZ[zNode] = zSteps;
nodeProbed   |= (1u << zNode);
```

### `unprobe [node]`

Clears one node's probe. With no argument it clears the node in slot 2. The node
argument exists because a tool swap can happen on the **parked** head, which has
no slot. Idempotent.

### Reporting

`getstate` appends, last, after every field an existing host parses:

```
probed=<0|1>[ pz=<z_steps>]
```

`probed` is `probeValidZ()`. `pz` is present only when `probed=1`. The host
reads the cut height from here, not from a cache of its own.

---

## 4. Host

### 4.1 Config and job inputs

| field | where | change |
|---|---|---|
| `liftHeight` | `ToolProfile` | **removed** — derived per job (§1) |
| `plunge` | `ToolProfile` | **new**, boolean. Knife `true`, pen `false` |
| `clearanceMm` | machine | **new**, default 2 |
| `materialMm` | job | **new**, required before compile |
| `tripMm` | `ProbeConfig` | **new**, signed (§1) |
| probe recipe | `ProbeConfig` | existing feeds, distances and `seekOvertravelMm`; the bench timing (§4.4) is set by giving the seek the latch feed, `rampSteps` 0 and an overtravel under one step |

One helper computes cut, clear and lift from the profile, `clearanceMm` and
`materialMm`. `compileBlock` and the runner both call it, so they cannot
disagree about where the tool is.

Every tool now moves Z. `resolve.ts` today enables Z only when
`liftHeight > 0`, which is why the pen has no plunge at all; that condition goes
away.

### 4.2 Blocks stay relative

`discretize.ts` keeps emitting `−lift` to cut and `+lift` after the stroke. The
only change is where `lift` comes from: `clear − cut` for this tool and this
job, instead of `profile.liftHeight`. Blocks are still a pure function of their
inputs. `materialMm` and `plunge` just joined those inputs.

This is what the planner needs, and all it needs: `materialMm`, before blocks
are compiled. A knife block cannot be compiled without it, and changing it means
recompiling. The probe value never reaches the planner; only the runner uses
`pz` and `tripMm`.

### 4.3 The runner puts Z at clear

A block assumes it starts at clear height. Nothing establishes that today; Z is
wherever it was left. So before a phase's first block, the runner:

1. Reads `getstate`. If `probed=0`, measures the tool first: the probe (§4.4)
   for a Z with a `ProbeConfig`, a manual touch-off (§4.6) for one without.
2. Computes clear Z from `pz`, `tripMm`, `materialMm` and `clearanceMm`.
3. Moves Z there (absolute: target minus `getpos` Z).

A block ends where it started, so Z is back at clear after every block and no
further moves are needed within the phase.

### 4.4 Running a probe

1. `probe_map`, then the four `probe_leg`s, timed and budgeted from
   `ProbeConfig`. Configure all four legs at the latch timing (1068 µs, no ramp,
   poll every step), differing only in step budget: that gave 27 µm
   repeatability on the bench, and a fast ramped approach scattered by 200–900
   steps.
2. After the latch leg: wait for `probing=` to leave 0. `probe_leg` answers `ok`
   when the leg starts, not when it finishes. Then read `getpos` Z.
3. `probe_end`, then `setprobe <z>`.

### 4.5 When the host invalidates

The Pico clears the probe for anything involving the datum (§2). The Pico cannot
see tool changes, so the host sends `unprobe <node>` on:

- `confirmSwap()` for each head whose tool changed,
- a manual mount from the UI (`controller.mount()`).

### 4.6 Manual touch-off

A Z with no `ProbeConfig` has no bed switch. Whenever that head is about to cut
unprobed (the first phase, after a swap, after a mid-phase head switch), the
runner records XY and calls its `touchOff` hook. The operator jogs the tip onto
bare mat and stores that Z (`touchOffHere`, i.e. `setprobe` at the current
`getpos` Z). The hook resolves only once `getstate` shows `probed=1`. The
runner then moves Z to clear and returns XY to where it was recorded.

The mat is the reference, as with the switch, so no material thickness is
needed to touch off.

---

## 5. Open

- Where `materialMm` is entered in the UI. The demos have a plain number field
  for now. It must come before compile (§4.2).
- Whether `materialMm` is kept between jobs.
