# Code Review: Architecture Improvements

Date: 04/07/2026 10:41

## Finding: Duplicated Observer Pattern Logic

**Components:**
- [host/ui/app_state.py](file:///c:/Users/user/Documents/fablab/ATtiny3224xRP2350_RS485_Custom_for_ai/host/ui/app_state.py) (`AppState`)
- [host/ui/offline/session.py](file:///c:/Users/user/Documents/fablab/ATtiny3224xRP2350_RS485_Custom_for_ai/host/ui/offline/session.py) (`OfflineSession`)

**Observation:**
Currently, both `AppState` (managing global state) and `OfflineSession` (managing local tab state) independently implement the exact same Observer pattern logic. They both define:
1. An internal list of subscribers (`self._subscribers` and `self._callbacks` respectively).
2. A `subscribe(self, callback)` method to register callbacks.
3. A `_notify(self)` method to trigger the registered callbacks upon state changes.

**Recommendation:**
Extract this duplicated logic into a shared `Observable` (or `EventEmitter`) base class to adhere strictly to the DRY (Don't Repeat Yourself) principle.

### Proposed Implementation

```python
# common/observable.py (or similar utility file)
class Observable:
    def __init__(self):
        self._subscribers = []
        
    def subscribe(self, callback):
        if callback not in self._subscribers:
            self._subscribers.append(callback)
            
    def _notify(self):
        for cb in self._subscribers:
            cb()
```

Then, refactor the existing classes to inherit from this base class:

```python
from common.observable import Observable

class AppState(Observable):
    def __init__(self):
        super().__init__()
        self._config = None
        # ...

class OfflineSession(Observable):
    def __init__(self, app_state: AppState):
        super().__init__()
        self._app_state = app_state
        # ...
```

> [!TIP]
> This refactor will make it extremely easy to add reactive state management to any new components we create in the future without having to rewrite the subscription logic!

---

## Finding: Overly Broad Concurrency Guard Blocks Jog Chaining

**Components:**
- [host/ui/online/session.py](file:///c:/Users/user/Documents/fablab/ATtiny3224xRP2350_RS485_Custom_for_ai/host/ui/online/session.py) (`OnlineSession.jog`)

**Observation:**
The `jog()` method uses the global `self.busy` flag to prevent queuing jog commands. While this successfully prevents jogs from interrupting an active job, it also inadvertently prevents queuing a jog while *another jog* is currently streaming. This makes the `self.jog_q` queue useless for its intended purpose and prevents the user from rapidly clicking the jog button to chain movements together.

**Recommendation:**
Change the guard in `jog()` to specifically check if a *job* is running (e.g., by checking `self._gui_op is not None` instead of `self.busy`), and update `run_job()` to respect the jog queue. 

### Proposed Implementation

In `jog()`:
```python
    def jog(self, ltr: str, sign: int, dist: float, rate: float, accel: float):
        # ...
        # Change `if self.busy:` to:
        if self._gui_op is not None:
            self.last_command_status = "Rejected: job in progress"
            self._notify()
            return
```

In `run_job()`:
```python
    def run_job(self):
        """Starts a background thread to execute the loaded plan."""
        # Add `not self.jog_q.empty()` to prevent starting jobs when jogs are queued
        if self.busy or not self.jog_q.empty() or not self.is_connected or not self.app_state.plan:
            return
```

> [!WARNING]
> Because of a user-configured permission rule, this fix must be applied manually to `host/ui/online/session.py`.

---

## Feature Blueprint: True Jog Blending via Producer-Consumer Generator

**Components:**
- `host/protocol/packets.py` (`make_jog`)
- `host/ui/online/session.py` (`_jog_worker`, `jog`)

**Observation:**
Currently, jog commands come to a full stop between clicks because the `jog()` method pre-calculates a complete trapezoidal motion profile (acceleration -> cruise -> deceleration) for every single click and tags the final packet with `MSEG_FLAG_PATH_END`. 

**Proposed Architecture:**
To achieve "Jog Blending" (maintaining cruise speed across multiple rapid clicks) without forcing the background worker to do complex physics calculations on the fly, a Producer-Consumer Python Generator pattern can be used.

1. **Pre-calculate the Components:**
   When the user clicks jog, the UI thread calculates the physics and pushes a dictionary of distinct "Lego blocks" into the queue:
   - `accel`: Packets to ramp from $V_0$ to $V_{cruise}$
   - `cruise_blend`: Pure cruise-speed packets for the requested distance
   - `decel`: Packets to ramp from $V_{cruise}$ to $V_0$

2. **The Generator (Producer):**
   The `_jog_worker` streams packets to the hardware using a generator function that stitches the blocks together:
   - Yields the `accel` block (if not already cruising).
   - Yields the `cruise_blend` block.
   - **The Lookahead:** Once the cruise block is finished, it checks the queue. 
     - If the user clicked again (same axis), it skips the `decel` block, stays at cruise speed, pops the next jog, and immediately yields the next `cruise_blend` block.
     - If the queue is empty, or the axis changes, it yields the original `decel` block to bring the machine to a safe, smooth stop.

**Benefits:**
- **Jog Blending:** Seamlessly chains multiple clicks into one continuous movement.
- **Smooth Jog Cancel (Queue Flush):** If the user changes directions, simply clearing the queue forces the producer to gracefully yield the current `decel` block, stopping smoothly before changing axes.
- **E-Stop Integration:** Emergency stops bypass the queue and send `cmd.stop` to instantly kill motion, sacrificing smoothness for safety.
- **Thread Safety:** Keeps heavy float math on the UI thread and keeps the background streaming worker simple and fast.

---

## Finding: Protocol Desync via Multiline Firmware Response

**Components:**
- `host/ui/online/session.py` (`ping_all`)
- `host/protocol/link.py` (`SimBackend._handle`, `Link.command`)
- `src/rp2350/core0.cpp` (`pingnode all` command)

**Observation:**
The host control plane protocol is strictly designed as a "One Line Request ➔ One Line Response" system (`Link.command` reads exactly one line). 

However, calling `pingnode all` exposes a critical discrepancy between the simulator and the physical hardware:
1. **The Simulator** (`SimBackend`) incorrectly returns a single-line response (`node all ok`), successfully masking the bug during offline testing.
2. **The Physical Firmware** (`core0.cpp`) treats `pingnode all` as a human "bring-up convenience" and explicitly returns a multiline response (four separate lines, one for each node).

When `session.ping_all()` sends `pingnode all` to the real hardware, `Link.command()` reads the first line (`node 1 ok`), leaving the remaining three lines in the serial buffer. The next time the background polling loop runs, it reads the leftover lines instead of the expected status data, crashing the parser and permanently breaking UI synchronization.

**Recommendation:**
The UI must respect the single-line constraint. Rewrite `ping_all()` in `session.py` to loop through the configured nodes and send the single-line form (`pingnode <id>`) to each node individually, rather than using the multiline `pingnode all` convenience command.

---

## Finding: Hardcoded Jog Acceleration Causes Jitter

**Components:**
- `host/ui/online/axis_nodes_view.py` (UI Accel Box)
- `host/ui/online/session.py` (`jog` method)

**Observation:**
The new UI introduces an "Accel" text box for jogging (`axis_nodes_view.py` line 146) which defaults to `500.0` mm/s². Because the `make_jog` function generates velocity chunks in ~10ms intervals, an acceleration of 500 mm/s² creates an extremely steep, coarse 4-step velocity staircase. This dumps massive torque into the motors instantly, causing violent shaking, stuttering, and missed steps on the physical machine.

In contrast, the older `jog_ui.py` script automatically calculated a much gentler, proportional acceleration ramp (`max(feed * 8.0, 50.0)`), which resulted in physically smooth motion.

**Recommendation:**
Remove the manual "Accel" text box from the UI entirely to simplify the interface. Update `session.py` to automatically calculate the acceleration proportionally based on the requested feed rate, mimicking the proven behavior from `jog_ui.py`:
```python
accel = max(rate * 8.0, 50.0)
```
