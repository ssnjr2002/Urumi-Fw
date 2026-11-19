# Status keys

The keys `get` reads: what each answers, and in which states. The `get`
command itself is in [reference/commands.md](reference/commands.md); binary
records, including `STATUS_RSP`, which mirrors the main keys, are in
[reference/packets.md](reference/packets.md).

This page holds only the keys until the commands reference is revised to take
them.

## `get` keys

`get` answers `key=value` for each key asked, space-separated, in the order
asked, all read in one snapshot. In place of a value: `?` the read failed, `-`
the key does not apply now, `!` the key is not recognised (older firmware). A
request line is at most 255 characters.

`config.<path>` reads one editable field of the active config, as `set` names it
(exact case): `x,y` for a point, `-` for an absent optional field, `!` for no
such field.

A group name stands for its keys, expanded in place; group and key names share
one namespace. A key asked for twice is printed once, where first asked, so
`get status homed` prints `homed` once.

```
status = state ctx homed enabled head tools unlocked mpos wpos buf mute excluded touched mesh
```

```
state=<s>     machineState    0=IDLE 2=ESTOP 3=ALARM 5=HOMING 6=PROBING 7=JOGGING 8=TRAVERSE 9=JOB (1 and 4 retired)
ctx=<c>       machineContext  0=IDLE 1=JOB 2=PROBE 3=HOMING
enabled=<hex> nodeEnabled     bitmask, bit n = bus node n enabled, mapped or not
homed=<hex>   originMask      bitmask, bit n = bus node n holds an origin, mapped or not
faults=<hex>  machineFaults   fault mask, as STATUS_RSP (e.g. 0x0004)
job=<j>       jobReason       0=BUFFERING 1=RUNNING 2=HOLDING 3=PAUSED 4=ABORTING 5=TOOL_CHANGE 6=TOOL_WAIT 7=DWELL 8=PREFLIGHT 9=PLACE  (only while ctx=1)
resume_layer=<l>   the layer a paused job resumes at  (only while job=3)
resume_record=<r>  the record within it  (only while job=3)
liftreason=<r>,…   the latest lift's reasons: end corner duty deadline unwind pause abort (dry reserved); `-` before the first  (only while state=JOB)
liftcount=<n>      lifts begun since the job opened  (only while state=JOB)
toolon=<0|1>       the powered tool's switch  (only while state=JOB, with a powered tool)
onms=<ms>          its duty on-time since the duty's last reset, the open span included  (as toolon)
latched=<hex> nodeLatched     bitmask, bit n = bus node n held by a latched limit, mapped or not
homefail=<f>  homingFailWhy   LEGFAIL_* of the leg that failed  (only with the HOMING fault)
homenode=<n>  homingFailNode  bus id of that leg's node, 0 for a dummy leg  (with homefail)
seq=<s>       sequenceId      the running sequence, else the one that failed (with the SEQUENCE, HOMING or PROBE fault): 1=LEG 2=TRAVERSE 3=HOME 4=RECOVERY 5=GOTO 6=TOOL_CHANGE
seqfail=<r>   sequenceFailWhy what refused a sequence after its ok, one token (e.g. soft_limit)  (only with the SEQUENCE fault)
cfgerr=<r>    boot config     absent | fs | file | <decode error>  (only with the CONFIG fault)
cfgdirty=<n>  config edits    1 while the active config holds edits `save` has not stored
jogging=<j>   ringOwner       1=STEP 2=CONT 3=GOTO  (only while state=JOGGING; STEP = jog/line/bez, CONT = a continuous jog, GOTO = goto)
homing=<h>    homingReason    0=LEG 1=WAIT  (only while state=HOMING; LEG = any leg runs, WAIT = none)
probing=<p>   probingReason   probe session phase  (only while PROBING or with the PROBE fault)
probe=<c>     last leg cause  PROBE_*  (with probing)
retries=<n>   last leg retries  (with probing)
psteps=<n>    last leg steps  (with probing)
probed=<0|1>  a probe height is held for the Z in slot 2
pz=<z>        that height in steps  (only with probed=1)
homecycle=<k> homingHeld      the cycle a `home` run is in  (only while one runs)
mute=<hex> excluded=<hex> touched=<hex>  bus id masks, bit n = node n (core0/ops/bus.h)
buf=<n>              planner blocks queued
queued=<us>          the motion time of the queued blocks not yet started, µs
expectedseq=<n>      the next wire seq the data plane will execute; informational, not flow control (ACKs advance the window)
pos=<n>:<steps>,…    debug: each config axis node's machine position, id order; `<n>:-` while unhomed or not known. What the Pico believes; `nodepos` reads the node's counter
mpos=<x>,<y>,<z>,<a> machine position, units (mm, deg), + physical; `-` while unbound or unhomed
wpos=<x>,<y>,<z>,<a> work position: selected tip (the blade tip with an `offsetMm`) − work offset; A the edge heading (MPos + the mounted tool's `aOffsetDeg`) folded to (−180, 180]; `-` while unbound or unhomed, X/Y `-` with no head, Z `-` until the head's Z is zeroed. STATUS_RSP carries NaN for each `-`
head=<n|anchor>      the controlled point: head index, `anchor` (the laser)  (`-` with none)
laser=<on|off>       the laser as its node last acked a `CMD_LASER`, not the pin  (`?` before the first command or after a failed one, `-` with no laser)
tools=<id>,…         the tool id each head has mounted (`mount`), head order, `-` for none  (`-` without a config)
unlocked=<hex>       bit h = head h's holder can release now: IDLE or JOB.TOOL_WAIT, nothing running, its A homed and within `heads[].a.unlockRange`  (`-` with no head having one; an A node that is not `present` has none)
mesh=<m>             the bed mesh: <nx>x<ny> followed, `off` (loaded, not followed), `flat` (no /mesh.bin), `bad` (unreadable)
meshslope=<s>        its steepest slope between neighbouring points, mm/mm  (`-` without a loaded mesh)
late=<n>             planner blocks Core 1 found late, since boot; a job lands PAUSED, a jog holds and runs on
amiss=<n>            tangential curves whose tracked A ended over 0.5° from the planned A, since boot; A snapped to it
texp= tmeas= twall=           DEBUG_TIMING builds only: last burst's expected, measured and wall time (us)
```
A lift is Z rising out of a cut or a plunge for one of `liftreason`'s reasons,
with the A, tool events or XY it carries and the plunge back; one lift carries
every reason due at its spot (a BREAK's duty stop and corner swivel are one
lift, `duty,corner`). A START's travel is not a lift. `liftreason` stays on the
latest lift until the next one; the four keys reset as a job opens, not as it
resumes.
