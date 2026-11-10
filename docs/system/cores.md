# Cores

**Checked against `effbba5`.** To see what may have changed since:

```bash
git diff effbba5 -- src/rp2350/ipc/ src/rp2350/core1/core1.cpp src/rp2350/core1/rpc_server.cpp src/rp2350/core0/core0.cpp src/rp2350/core0/ops/observe.cpp src/rp2350/core0/config/config_store.cpp src/rp2350/main.cpp
```

Commands, replies and code names are written as the
[notation](../README.md#notation) says.

The RP2350 has two cores. Core 0 runs the [controller](controller.md): it
talks to the host and decides what runs. Core 1 emits the steps and drives
the bus. This page is the boundary between them: how they start and stop
together, and the channels that cross it.

## 1. Hardware

| Part | What the cores do with it |
| --- | --- |
| Two Cortex-M33 cores | arduino-pico starts both: Core 0 runs the controller (`setup()`, `loop()`), Core 1 the steps and the bus (`setup1()`) |
| Shared SRAM | every value both cores see lives in it, defined in `main.cpp` |
| The hardware spinlocks | one guards the planner ring; the RPC queues take one each |
| The inter-core FIFO | unused (Decisions) |
| Flash, read in place | both cores run code from it; erasing or writing it stalls them, so Core 1 parks in RAM while Core 0 writes the config |

## 2. I/O

**Core 0 to Core 1:**

* **RPC requests:** something for Core 1 to run: a node command, a debug
  step burst, or a probe leg. Core 0 starts every transaction.
* **Requests:** flags Core 0 raises and Core 1 acts on: the e-stop request
  (`estopRequested`), the abort and pause requests (`abortRequested`,
  `pauseRequested`), a pending tool change (`toolChangePending`), and the
  two parks.
* **Shared state:** what Core 0 sets for Core 1 to follow: the context
  (`machineContext`), the ring's owner (`ringOwner`), the job's tool and its
  duty limits (`jobPower`), the job's buffer thresholds (`bufferQueueS`,
  `bufferIdleMs`).
* **The planner ring:** motion, which Core 1 runs.

**Core 1 to Core 0:**

* **RPC replies:** one per request that wants one.
* **Facts:** what Core 1 has finished, on a ring of their own.
* **Shared state:** what Core 1 sets for Core 0 to read: the e-stop swept
  (`estopSwept`), the machine position (`machinePos`), which nodes are
  energised (`nodeEnabled`), the job's reason as the ring moves it
  (`jobReason`), and that it has parked.

## 3. Lifecycle

### Cold boot

* **Core 0** opens USB, sets up the RPC queues (`rpcInit()`), claims the
  planner's lock (`plannerLock`), and loads the config and the bed mesh.
  Then it runs the soft reset below.
* **Core 1** starts the RS485 port, then parks: the soft-reset request
  (`soft_reset_requested`) starts raised, so it touches nothing until
  Core 0's first wipe is done.

### Soft reset

`reset`, an adopted config and power-on all run it.

1. **Core 0** raises the soft-reset request (`soft_reset_requested`); its
   loop ends.
2. **Core 1** finishes its pass, says it has parked
   (`core1_parked_for_reset`), and waits.
3. **Core 0**, with Core 1 parked, needs no locks. It flushes USB input, the
   RPC queues (`rpcReset()`) and the fact ring (`core1FactsReset()`), and
   wipes the shared state: the context and the reasons, position and homed
   axes, the requests, the planner ring. Which nodes are energised
   (`nodeEnabled`) is Core 1's, so it stays. Then Core 0 lowers `soft_reset_requested`.
4. **Core 1** waits for its port to finish sending, flushes what it received,
   and sends an empty stream byte so every node's parser starts clean. Then
   it runs its loop.
5. **Core 0** sweeps the bus, applies the default map, and runs its loop
   ([controller](controller.md) §3, Boot).

### Flash park

Writing flash stalls both cores. The flash library idles Core 1 by itself,
but that can land anywhere in Core 1's loop, so Core 0 parks it first.

1. **Core 0** raises the flash request (`flash_op_requested`) and waits.
2. **Core 1** sees it at the top of its next pass, says it has parked
   (`core1_parked_for_flash`), and spins in a loop that runs from RAM
   (`core1FlashPark()`).
3. **Core 0** writes flash, lowers `flash_op_requested`, and waits for
   `core1_parked_for_flash` to clear.
4. **Core 1** clears it and carries on.

The park wipes nothing, so position and homing survive. Config writes are
admitted only with nothing running, so no motion is cut.

### E-stop

1. **Core 0** latches the stop and raises the e-stop request
   (`estopRequested`). Every step emitter on Core 1 stops on it.
2. **Core 1**, in its next pass, drops any pending pause or abort and the
   planner ring, broadcasts a disable, then makes every node safe one by one
   ([bus](bus.md)). It sets the e-stop swept (`estopSwept`) and posts the
   e-stop swept fact (`FACT_ESTOP_SWEPT`).
3. **Core 0** publishes ALARM once `estopSwept` is set, and clears the
   fenced slots' positions when it reads `FACT_ESTOP_SWEPT`.

`unstop` and the soft reset clear `estopRequested` and `estopSwept`.

## 4. Core 1's loop

Each pass of Core 1's loop (`processBus()`), in order:

1. **The e-stop**, once per latch (above). The pass ends there.
2. **Planner motion** (`processPlanner()`), while the ring holds blocks or a
   job is open ([planner](planner.md)).
3. **An abort with nothing moving:** drops the ring and clears the abort
   request (`abortRequested`); if there was motion to drop, posts the ring
   drained fact (`FACT_RING_DRAINED`).
4. **The keep-alive** (`busKeepalive()`): an empty stream byte when nothing
   was sent for 333 ms ([bus](bus.md)).
5. **One RPC request** (`rpcServerPoll()`), if one waits.

The order is the policy. A node command takes up to 20 ms on the bus, many
step intervals, so it waits for the queued motion to finish rather than
stretch a step. A command sent mid-job waits for the ring.

## 5. The channels

### RPC

Two queues of four entries: requests from Core 0, replies from Core 1.

* **One transaction in flight.** Starting a request (`rpcStart()`) refuses a
  second while one is outstanding. The bus is serial, so this costs nothing,
  and replies cannot arrive out of order.
* **Start, then finish.** `rpcStart()` returns the request's id; collecting
  the reply (`rpcFinish()`) never blocks; giving up (`rpcAbandon()`) drops
  the claim, and a late reply is then discarded. A handler with nothing else
  to do starts and waits in one call (`rpcCall()`).
* **`rpcCall()` gives up after 6 s.** It catches a wedged Core 1, not a slow
  one: a request sent mid-job waits for the ring. A caller that must keep
  serving the host, the probe leg, uses `rpcStart()` and `rpcFinish()`.
* **Excluded nodes** (`bus_exclude`): `rpcStart()` refuses a request to one
  without posting it, except make-safe.

A request (`RpcRequest`) is what to run, a node command, a node id and up to
20 bytes of arguments:

| Request | In code | Core 1 does | Replies |
| --- | --- | --- | --- |
| node command | `RPC_OP_NODE` | sends it, waits for the node's answer | the node's payload |
| debug step | `RPC_OP_STEP_DEBUG` | emits a burst of stream bytes into one slot | nothing |
| probe leg | `RPC_OP_PROBE_LEG` | runs the leg to its end, stepping Z and polling the vacuum's switch | the cause, retries, switch level, steps |

A reply's result is one of:

| Result | In code | Means |
| --- | --- | --- |
| ok | `RPC_OK` | the node answered as expected; for a probe leg, the leg ran, whatever its cause |
| timeout | `RPC_TIMEOUT` | no answer within 20 ms |
| nak | `RPC_NAK` | the node refused, with a reason |
| bad reply | `RPC_BAD_REPLY` | the node answered with something unreadable |

A broadcast is answered by no node: `RPC_OK` means only that the frame went
out.

**Which nodes are energised** (`nodeEnabled`) is learned here. Core 1 folds
every reply that carries a node's status into it (`noteEnabled()`). Only
confirmed replies count: a timeout or a nak leaves it alone. A broadcast
disable clears every node; a broadcast enable changes nothing, since
believing a node armed that never heard is the dangerous direction. Core 0
turns `nodeEnabled` into energised axes (`axes_enabled`) each pass.

### Shared state

Plain values in shared SRAM, each written by one core (§6). A value of
several fields is written under the planner's lock (`plannerLock`), or while
the other core cannot be reading it (parked, or the ring idle). A flag that
publishes other values is written after them, behind a memory barrier.

### The planner ring

One ring of motion blocks (`plannerRing`) and the executor that runs it
(`plannerExec`). Core 0 pushes and commits blocks; Core 1 runs them. Every
change to the ring holds `plannerLock`. Core 0 resets the ring only while
Core 1 is not running it (`plannerActive` clear), and sets with it the
ring's owner (`ringOwner`) and what the ring runs with: steps per mm
(`plannerSpm`), the bed mesh (`plannerMesh`), the blade offset
(`plannerBlade`). What a block is and how it becomes steps is the
[planner](planner.md) page's.

### Facts

A ring of eight bytes: Core 1 pushes (`core1FactPush()`), Core 0 pops
(`core1FactPop()`). A fact reports something Core 1 has finished; it never
asks Core 0 to act, and whatever Core 1 wrote is published before its fact
is pushed.

| Fact | In code | Core 1 has |
| --- | --- | --- |
| e-stop swept | `FACT_ESTOP_SWEPT` | swept the bus safe after an e-stop |
| ring drained | `FACT_RING_DRAINED` | stopped planner motion, ring empty |
| held | `FACT_HELD` | brought a paused job to rest, its resume point kept |
| tool failed | `FACT_TOOL_FAILED` | failed to switch the tool after its retries, and waits at the event |
| duty held | `FACT_DUTY_HELD` | stopped for the tool's duty, and waits for Core 0 to queue the lift |

Core 0 reads every fact at the top of its loop (`observeTick()`,
[controller](controller.md) §4). A full ring drops the fact and marks one
lost (`core1FactsLost()`); Core 0 then raises the facts-lost fault (`F_FACTS`), and clears the
fenced positions if an e-stop's sweep is done, as `FACT_ESTOP_SWEPT` would
have.

## 6. Who writes what

| Value | In code | Written by | Read by |
| --- | --- | --- | --- |
| Soft-reset request, flash request | `soft_reset_requested`, `flash_op_requested` | Core 0 | Core 1 |
| Parked for reset, parked for flash | `core1_parked_for_reset`, `core1_parked_for_flash` | Core 1 | Core 0 |
| E-stop request | `estopRequested` | Core 0, with the latch | Core 1 |
| E-stop swept | `estopSwept` | Core 1, after the sweep | Core 0 |
| Pause and abort requests | `pauseRequested`, `abortRequested` | Core 0 raises; Core 1 lowers as it acts | both |
| Tool change pending | `toolChangePending` | Core 0 raises, and lowers once the tool change starts; Core 1 lowers it when a pause wins | both |
| The context, the ring's owner | `machineContext`, `ringOwner` | Core 0 | both |
| The job's reason | `jobReason` | both ([controller](controller.md) §6) | both |
| Machine position | `machinePos` | Core 1 as it emits; Core 0 with the ring idle (an origin, a bind) | both |
| Which nodes are energised | `nodeEnabled` | Core 1 only, from confirmed replies and its sweeps | Core 0, into `axes_enabled` each pass |
| The planner ring and what it runs with | `plannerRing`, `plannerExec`, `plannerSpm`, `plannerMesh`, `plannerBlade` | Core 0 pushes and resets; Core 1 runs; both under `plannerLock` | both |
| Core 1 running the ring | `plannerActive` | Core 1, under `plannerLock` | Core 0 |
| Where a job stands: resume point, block running, lifts, tool on, duty counters, duty stop | `jobResume`, `jobExec`, `jobLift`, `toolOn`, `dutyOnMs`, `toolOnAtMs`, `dutyStop` | Core 1 | Core 0 |
| The job's tool, duty reset, buffer thresholds | `jobPower`, `dutyEpoch`, `bufferQueueS`, `bufferIdleMs` | Core 0 | Core 1 |
| Waiting for a duty lift | `dutyWait` | Core 1 raises with `dutyStop`; Core 0 lowers once the lift is queued | both |
| The fact ring | `core1FactPush()`, `core1FactPop()` | Core 1 pushes; Core 0 pops | |

## 7. Decisions

* **Queues, not the inter-core FIFO.** The RP2350's FIFO is four words deep,
  arduino-pico uses it to idle the other core, and its blocking read has no
  timeout, so a wedged Core 1 would hang Core 0.
* **Core 0 always starts.** Core 1 never opens a transaction: what it must
  report is a value Core 0 reads or a fact.
* **Facts report, they never ask.** Core 0 decides what a fact means; Core 1
  is done by the time it is read.
* **RPC after motion.** A bus transaction never stretches a step; a command
  waits for the ring instead.
* **Core 1 owns which nodes are energised** (`nodeEnabled`). It is the one
  per-node value with a writer Core 0 does not drive: the e-stop sweep.
  Owned by Core 0, its updates raced the sweep. The other per-node values
  (origins, homed, latched) stay Core 0's, since nothing changes them
  unasked ([node-frame-ownership](../plans/node-frame-ownership.md)).
* **Two handshakes, not one.** A config write must keep position and homing,
  so the flash park wipes nothing; the soft reset wipes everything.

## 8. Code

All under `src/rp2350/`:

| File | Holds |
| --- | --- |
| `main.cpp` | where the shared values are defined |
| `ipc/shared_state.h` | the shared state, the requests, the planner ring's values, the handshakes |
| `ipc/core1_rpc.h`, `core1_rpc.cpp` | the RPC contract and Core 0's side: queues, ids, decoding replies, one call per node command |
| `core1/rpc_server.cpp` | Core 1's side of the RPC: the node transaction, `noteEnabled()` |
| `ipc/core1_facts.h`, `core1_facts.cpp` | the fact ring |
| `core1/core1.cpp` | Core 1's loop, its parks, the e-stop sweep, the keep-alive |
| `core0/core0.cpp` | Core 0's setup and the soft reset |
| `core0/config/config_store.cpp` | the flash park, Core 0's side |
| `core0/ops/observe.cpp` | reading the facts |
