import threading
import queue
import struct
import time
from typing import Callable, List, Optional

try:
    from serial.tools import list_ports
except ImportError:
    list_ports = None

from collections import namedtuple

from host.protocol.link import Link
from host.protocol.session import PacketSource
from host.protocol.packets import pack_jog, MSEG_FLAG_NONE
from host.ui.app_state import AppState
from host.ui.observable import Observable
from host.execution.job_runner import Operator

class GuiOperator(Operator):
    def __init__(self, session: "OnlineSession"):
        self.session = session

    def mount(self, tool_name: str):
        self.session.mount_ok = True
        self.session.mount_event.clear()
        self.session.pending_mount = tool_name
        self.session._notify()  # trigger UI to show modal
        self.session.mount_event.wait()
        self.session.pending_mount = None
        self.session._notify()
        if not self.session.mount_ok:
            raise RuntimeError("job cancelled by operator")

    def note(self, text: str):
        self.session.job_notes.append(text)
        self.session._notify()

SIM_PORT = "Simulator"


_JMS = namedtuple("MS", ["dx", "dy", "dz", "da", "interval", "flags"])


class _ClickJogSource(PacketSource):
    """Feeds an open session driven by jog-button CLICKS.

    One click = one fixed distance (the button's mm value). Clicking again while
    the machine is still moving ADDS that distance to what is left to travel, so
    the motion extends instead of stopping and restarting — that is the blend.

    This is an open session even though each click has a definite distance: the
    total is not known when the first byte goes out, because it depends on clicks
    that have not happened yet. The session ends when the remaining distance is
    spent and speed is back to rest.

    Pacing: keep at most LEAD_US of motion time queued, and no deeper than
    LOW_WATER segments. Every packet queued is a packet that must still execute,
    so running far ahead makes the machine unresponsive to the next click. Both
    figures come from the live status sample, which keeps updating DURING
    transmission — the host no longer dead-reckons how far ahead it is.

    Deceleration is split by cause: a move that runs to its requested distance
    ramps down here, because the distance must come out exact; a cancelled move
    hands the ramp to the Pico (§4.5), which is the only place the actual
    instantaneous velocity exists and the only one that can call back motion
    already sitting in the ring.
    """

    LOW_WATER = 4        # segments to keep queued on the Pico
    CHUNK_MS  = 20       # motion per emitted packet
    V_START   = 50.0     # steps/s — rest velocity, matches make_jog
    LEAD_US   = 80_000   # keep at most this much motion-time queued ahead (µs)
    MAX_BURST = 8        # packets per pull() — bounds one call, LEAD_US bounds
                         # the queue. Without a burst the source cannot outpace
                         # its own pull cadence (see pull()).

    def __init__(self, machine, axis, ltr, sign, rate, link=None):
        self.machine, self.axis = machine, axis
        self.ltr, self.sign, self.rate = ltr, sign, rate
        self.link = link
        self.emitted = 0
        self.steps_total = 0        # steps actually committed to the wire
        self.clicks = 1

        self._remaining = 0.0       # steps still to travel
        self._v = self.V_START      # current velocity, ramped across chunks
        self._finished = False
        self._cancelled = False     # reversal/stop — the Pico is ramping
        self._lock = threading.Lock()
        self._wake = threading.Event()

        # Sub-poll-interval pacing estimate. `ctx.queued_us` is the AUTHORITY on
        # how much motion is queued, but it is only as fresh as the last status
        # sample (~100 ms), and pull() is called as fast as the ack loop allows.
        # Reading a stale value between polls would dump the whole move onto the
        # wire in one go. So: wall clock for resolution, the report for truth.
        # (§4.6 claimed the report deletes this. It does not — it fixes WHICH
        # quantity is reported, not how often. Keep both.)
        self._queued_s = 0.0        # motion-seconds handed to the machine
        self._t0 = None             # when the first packet went out

        self.feed_sps = max(1.0, rate * axis.steps_per_unit)
        self.accel_sps2 = max(rate * 8.0, 50.0) * axis.steps_per_unit
        self._idx = {"x": 0, "y": 1, "z": 2, "a": 3}[ltr]

    # -- intent (called from the Tk thread) -----------------------------------

    def add(self, steps):
        """Another click in the same direction — extend the move. Returns False
        if this source has already finished, so the caller starts a new one."""
        with self._lock:
            if self._finished:
                return False
            self._remaining += steps
            self.clicks += 1
            self._wake.set()
            return True

    def cancel(self):
        """Reversal or stop — hand the deceleration to the Pico (§4.5).

        Was: drop the remaining distance and coast, letting the host-planned
        ramp-down branch wind the velocity out over the packets still queued.
        That only worked because the host guessed an accel it half knew, and it
        could not stop motion already sitting in the ring.

        Now one byte. The Pico ramps from its ACTUAL instantaneous velocity —
        the only place that value exists — flushes the ring and lands IDLE with
        position intact. Everything still in flight is discarded, so this source
        stops emitting immediately rather than winding down.
        """
        with self._lock:
            self._remaining = 0.0
            self._cancelled = True
            self._wake.set()
        if self.link is not None:
            self.link.abort()

    # -- packet construction --------------------------------------------------

    def _decel_distance(self, v):
        """Steps needed to get from v back down to rest."""
        return max(0.0, (v * v - self.V_START ** 2) / (2.0 * self.accel_sps2))

    def _packet(self, steps, v_avg):
        vec = [0, 0, 0, 0]
        # No axis.invert here: the existing jog path (OnlineSession.jog -> make_jog)
        # does not apply it either, and applying it in only one of them would make
        # the two disagree on direction for the same button.
        vec[self._idx] = int(self.sign * steps)
        interval = max(1, min(int(self.machine.f_cpu / max(v_avg, 1.0)),
                              self.machine.f_cpu))
        return pack_jog(_JMS(dx=vec[0], dy=vec[1], dz=vec[2], da=vec[3],
                             interval=interval, flags=MSEG_FLAG_NONE))

    # -- the source contract --------------------------------------------------

    def _lead_us(self):
        """Local estimate of motion time queued ahead of the machine, in µs.

        Fast but blind — it assumes every packet was accepted and that execution
        started when the first one went out. Good for resolution between polls,
        not for truth.
        """
        if self._t0 is None:
            return 0.0
        return max(0.0, (self._queued_s - (time.monotonic() - self._t0)) * 1e6)

    def _draining(self, ctx):
        """Is the machine still executing what we already sent?

        The reported figure decides it — this is the question the local estimate
        was worst at, and getting it wrong is what let `busy` go false while the
        machine was still moving, which broke blending. The local estimate is
        only consulted to cover the window before the first status sample lands,
        where a reported 0 means "no news yet", not "stopped".
        """
        queued = ctx.queued_us
        if queued is None:                       # version skew — no queued_us
            buf = ctx.buf_count
            return (buf is not None and buf > 0) or self._lead_us() > 0
        return queued > 0 or self._lead_us() > 0

    def pull(self, ctx):
        with self._lock:
            if self._finished:
                return None
            if self._cancelled:
                # The Pico is ramping and has thrown away the ring. Nothing we
                # emit now would be accepted (NACK_ABORTING), and nothing we
                # already sent survives, so the session is simply over.
                self._finished = True
                return None
            remaining = self._remaining

        if remaining <= 0.0:
            # Distance spent — but do NOT finish while the machine is still
            # executing what we already sent. Staying open is what lets a click
            # arriving mid-move blend into it instead of starting a fresh
            # session, and it keeps `busy` honest: it means "machine moving",
            # not "packets delivered".
            if not self._draining(ctx):
                with self._lock:
                    self._finished = True
                return None
            self._wake.wait(0.01)       # a click wakes this immediately
            self._wake.clear()
            return []

        # Pace on the LOCAL estimate. It decays continuously, so it is never
        # stale; the report is a sample up to a poll interval old and can be
        # stale in EITHER direction. Measured on hardware, taking max(local,
        # reported) let a stale-high report hold the source off until the next
        # poll: it emitted a burst, stalled ~100 ms, and the ring drained to
        # empty twice at the start of a jog — the machine stopping and
        # restarting mid-move. buf_count stays as the hard ceiling, since the
        # ring is finite regardless of what either measure claims.
        #
        # The report's job is _draining(), where "is anything queued at all"
        # is the question and a poll interval of lag is harmless.
        buf = ctx.buf_count
        if self._lead_us() >= self.LEAD_US or (buf is not None and buf >= self.LOW_WATER):
            self._wake.wait(0.005)
            self._wake.clear()
            return []                   # nothing right now, still open

        # Fill UP TO the lead target in one call, rather than one chunk per
        # pull(). Emitting a single CHUNK_MS packet per pull ties throughput to
        # pull cadence, and a pull that returns [] costs ~25 ms (our 5 ms wait
        # plus the session's ctx.wait) while one packet only buys CHUNK_MS = 20
        # ms of motion. The source falls behind, the ring bleeds down, and the
        # machine stops and restarts mid-jog — measured on hardware as buf_count
        # reaching 0 seven times during a 30 mm move.
        batch = []
        dt = self.CHUNK_MS / 1000.0
        while (len(batch) < self.MAX_BURST
               and remaining > 0.0
               and self._lead_us() < self.LEAD_US):

            # Decelerate once the distance left is only enough to stop in.
            target_v = (self.V_START if remaining <= self._decel_distance(self._v)
                        else self.feed_sps)

            v0 = self._v
            v1 = (min(target_v, v0 + self.accel_sps2 * dt) if target_v > v0
                  else max(target_v, v0 - self.accel_sps2 * dt))
            v_avg = max((v0 + v1) / 2.0, 1.0)

            steps = max(1, min(int(round(v_avg * dt)), int(round(remaining))))

            self._v = v1
            with self._lock:
                self._remaining = max(0.0, self._remaining - steps)
                remaining = self._remaining
            if self._t0 is None:
                self._t0 = time.monotonic()
            self._queued_s += steps / v_avg
            self.emitted += 1
            self.steps_total += steps
            batch.append(self._packet(steps, v_avg))

        return batch

class OnlineSession(Observable):
    """
    Business logic manager for the Online Execution phase.
    Handles the serial link, background polling, and job streaming.
    """
    def __init__(self, app_state: AppState):
        super().__init__()
        self.app_state = app_state

        # --- Connection State ---
        self.link = None
        self.connection_error: Optional[str] = None
        
        # --- Polling State ---
        self.machine_state = None
        self.machine_pos_steps = None
        self.polling_error: Optional[str] = None
        self.busy = False  # Set to True when job/jog is streaming
        
        # --- Command State ---
        self.last_command_status = "—"
        self.node_ping_status = {}
        
        # --- Jogging State ---
        # Debug capture: when on, jog packets are written to jog_output.bin
        # (length-prefixed, same framing as verify_packets.py reads) instead of
        # being sent to the sim/COM port — for A/B-comparing what the host
        # actually generates without needing the real link.
        self.jog_dump_to_file = False
        self.jog_dump_path = "jog_output.bin"
        self.jog_q = queue.Queue()
        self._worker = threading.Thread(target=self._jog_worker, daemon=True)
        self._worker.start()

        # --- Manual (hold-to-jog) State ---
        self._jog_source = None      # the open session's PacketSource, if running
        self._jog_session = None     # the Session, for truncate() on reversal
        self._jog_lock = threading.Lock()

        # --- Job State ---
        self.job_notes = []
        self.pending_mount = None
        self.mount_ok = True
        self.mount_event = threading.Event()
        self._gui_op = None

        # --- Background Status Poller ---
        # Runs on its own thread so a wedged/disconnected Pico (blocking serial
        # reads, up to Link.command's 1s timeout per call) can never freeze the
        # Tk event loop. Skips itself whenever busy — the jog worker and job
        # worker are each the sole owner of `link` while they run; the job
        # worker pushes its own live updates via send_plan's on_progress hook.
        self._poll_thread = threading.Thread(target=self._poll_worker, daemon=True)
        self._poll_thread.start()

    # ---------------------------------------------------------
    # 1. Connection Management
    # ---------------------------------------------------------
    @property
    def available_ports(self) -> List[str]:
        """Returns a list of available COM ports. Gated to Simulator only if sim config is loaded."""
        if getattr(self.app_state, 'is_sim', False):
            return [SIM_PORT]
            
        ports = [p.device for p in list_ports.comports()] if list_ports else []
        return [SIM_PORT] + ports

    @property
    def is_connected(self) -> bool:
        return self.link is not None

    def toggle_connect(self, port: str):
        """Connect if disconnected, or disconnect if connected."""
        if self.is_connected:
            self.disconnect()
        else:
            self.connect(port)

    def connect(self, port: str):
        """Attempt to open the serial or simulator link."""
        if self.is_connected:
            return
            
        try:
            self.link = Link.open_sim() if port == SIM_PORT else Link.open_serial(port)
            self.connection_error = None
        except Exception as e:
            self.link = None
            self.connection_error = str(e)
            
        self._notify()
        
    def disconnect(self):
        """Close the active link. Refuses while a job or jog is streaming —
        closing the port mid-transfer orphans the machine (whatever's already
        buffered in the Pico's ring buffer keeps executing with the host now
        blind to it). Cancel/wait for the job to finish first."""
        if self.busy:
            self.last_command_status = "Ignored: cannot disconnect while busy"
            self._notify()
            return
        if self.link:
            try:
                self.link.close()
            except Exception:
                pass
            self.link = None
            self.connection_error = None
            self.machine_state = None
            self.machine_pos_steps = None
            self.node_ping_status.clear()
            self._notify()

    # ---------------------------------------------------------
    # 2. Status Polling
    # ---------------------------------------------------------
    def _poll_worker(self):
        """
        Background thread: polls machine state at ~100ms, CONTINUOUSLY.

        It no longer skips while busy. Under the new link model the reader owns
        the port for the connection's lifetime and this poll only needs the
        writer for a single byte between frames, so it slots into an active
        stream instead of waiting for one to finish (docs/comms_architecture.md
        §3). This is the end-to-end proof of the whole architecture: a moving
        position readout during a job or a jog was impossible under seizure.

        Position rides in the same frame (§4.2), so there is no second round
        trip and no second cadence. That also makes the sample COHERENT: state
        and position are read from one instant on the Pico. The old split polled
        `getpos` on the text plane every 4th pass, so the two could describe
        moments up to ~400 ms apart — invisible while idle, and exactly wrong
        while jogging, which is when a position readout is worth having.
        """
        while True:
            time.sleep(0.1)
            if not self.is_connected:
                continue
            try:
                st = self.link.get_status(timeout=0.5)
                self.machine_state = st
                if st.pos is not None:
                    self.machine_pos_steps = st.pos
                self.polling_error = None
            except Exception as e:
                self.polling_error = str(e)

            self._notify()

    # ---------------------------------------------------------
    # 3. Control Plane Commands
    # ---------------------------------------------------------
    def _send_command(self, cmd_fn) -> bool:
        """Helper to send a control plane command (skip if busy)."""
        if not self.is_connected or self.busy:
            self.last_command_status = "Ignored: link busy or disconnected"
            self._notify()
            return False
            
        try:
            ok, reason = cmd_fn(self.link)
            if not ok:
                self.last_command_status = f"Rejected: {reason}"
            else:
                self.last_command_status = f"OK: {cmd_fn.__name__}"
            self._notify()
            return ok
        except Exception as e:
            self.last_command_status = f"Error: {e}"
            self._notify()
            return False

    def enable_all(self):
        from host.protocol import commands as cmd
        self._send_command(cmd.enable)
        
    def disable_all(self):
        from host.protocol import commands as cmd
        self._send_command(cmd.disable)
        
    def set_origin_all(self):
        from host.protocol import commands as cmd
        self._send_command(cmd.setorigin)

    def estop(self):
        # We use cmd.stop for an emergency stop that flushes the pipeline
        from host.protocol import commands as cmd
        self._send_command(cmd.stop)
        
    def unalarm(self):
        from host.protocol import commands as cmd
        self._send_command(cmd.unalarm)
        
    def ping_node(self, node_id: int):
        from host.protocol import commands as cmd
        if not self.is_connected or self.busy:
            return
            
        try:
            # cmd.ping_node returns a boolean indicating success
            ok = cmd.ping_node(self.link, node_id)
            if ok:
                self.node_ping_status[node_id] = "OK"
                self.last_command_status = f"OK: ping_node {node_id}"
            else:
                self.node_ping_status[node_id] = "TIMEOUT"
                self.last_command_status = f"Timeout: ping_node {node_id}"
        except Exception as e:
            self.node_ping_status[node_id] = "ERROR"
            self.last_command_status = f"Error pinging node {node_id}: {e}"
            
        self._notify()

    def ping_all(self):
        """One `pingnode all`, not one command per node.

        This used to walk the configured nodes individually because the firmware
        answered `all` with one line per node and `Link.command()` reads exactly
        one — the extra lines desynced every later command. Both sides now hold
        the one-line-per-command contract, so the single command is usable.

        The firmware always walks nodes 1–4. Results for all of them are recorded
        (a node answering that the config does not know about is worth seeing),
        but the pass/fail verdict counts only CONFIGURED nodes — an absent node
        timing out is the expected answer, not a fault.
        """
        from host.protocol import commands as cmd
        if not self.is_connected or self.busy:
            return

        node_ids = set()
        if self.app_state.config:
            machine = self.app_state.config.machine
            if hasattr(machine, 'present_axes'):
                for ltr, ax in machine.present_axes():
                    node_ids.add(ax.node.node_id)
            for p in getattr(machine, 'peripherals', []):
                if getattr(p, 'present', True):
                    node_ids.add(p.node_id)

        try:
            results = cmd.ping_all(self.link)
            for nid, ok in results.items():
                self.node_ping_status[nid] = "OK" if ok else "TIMEOUT"

            expected = sorted(node_ids) or sorted(results)
            missing = [n for n in expected if not results.get(n, False)]
            unknown = [n for n, ok in sorted(results.items())
                       if ok and node_ids and n not in node_ids]

            if missing:
                self.last_command_status = (
                    "Timeout: node" + ("s " if len(missing) > 1 else " ")
                    + ", ".join(str(n) for n in missing))
            elif unknown:
                self.last_command_status = (
                    f"OK: all nodes ({len(expected)}) — also answering, "
                    f"not in config: {', '.join(str(n) for n in unknown)}")
            else:
                self.last_command_status = f"OK: all nodes ({len(expected)})"
        except Exception as e:
            self.last_command_status = f"Error pinging all nodes: {e}"

        self._notify()

    def _send_node_command(self, cmd_fn, node_id: int, label: str) -> bool:
        """Like _send_command, but for a per-node control-plane command
        (enable/disable <id>) — cmd_fn takes (link, node_id)."""
        if not self.is_connected or self.busy:
            self.last_command_status = "Ignored: link busy or disconnected"
            self._notify()
            return False

        try:
            ok, reason = cmd_fn(self.link, node_id)
            self.last_command_status = (f"OK: {label} {node_id}" if ok
                                        else f"Rejected: {label} {node_id} — {reason}")
            self._notify()
            return ok
        except Exception as e:
            self.last_command_status = f"Error: {label} {node_id}: {e}"
            self._notify()
            return False

    def enable_node(self, node_id: int):
        from host.protocol import commands as cmd
        self._send_node_command(cmd.enable, node_id, "enable")

    def disable_node(self, node_id: int):
        from host.protocol import commands as cmd
        self._send_node_command(cmd.disable, node_id, "disable")

    # ---------------------------------------------------------
    # 4b. Manual jogging — an OPEN session
    # ---------------------------------------------------------
    # The operator drives the machine in real time by holding a button. There is
    # no predetermined destination, so the packet sequence cannot be known up
    # front: it is produced in response to input that has not happened yet, and
    # the session ends by truncation rather than exhaustion
    # (docs/comms_architecture.md §2.3).

    def jog_click(self, ltr: str, sign: int, dist: float, rate: float):
        """One click of a jog button = move `dist` units on `ltr`.

        Clicking again while the machine is still moving extends the move
        instead of stopping and restarting it — the blend. Clicking the opposite
        direction aborts: the Pico ramps to rest from its actual velocity and
        keeps position. It does not then move the other way; a second click
        does that, once the machine is IDLE again.
        """
        if self._gui_op is not None:
            self.last_command_status = "Rejected: job in progress"
            self._notify()
            return
        if not self.is_connected or not self.app_state.config:
            return

        machine = self.app_state.config.machine
        axes = dict(machine.present_axes())
        if ltr not in axes:
            return
        if self.machine_state and not self.machine_state.enabled(ltr):
            self.last_command_status = f"Rejected: {ltr.upper()} axis is disabled"
            self._notify()
            return

        ax = axes[ltr]
        steps = int(round(abs(dist) * ax.steps_per_unit))
        if steps <= 0:
            return

        with self._jog_lock:
            src = self._jog_source
            if src is not None:
                if (src.ltr, src.sign) == (ltr, sign):
                    if src.add(steps):                  # blend into the live move
                        self.last_command_status = (
                            f"Jog {ltr.upper()} blend x{src.clicks}")
                        self._notify()
                        return
                else:
                    src.cancel()                        # reversal — coast to a stop
                    self.last_command_status = f"Jog {ltr.upper()} cancelled"
                    self._notify()
                    return
                # add() refused: the source finished as we clicked. Fall through
                # and start a fresh session below.

            src = _ClickJogSource(machine, ax, ltr, sign, rate, link=self.link)
            src.add(steps)
            src.clicks = 1
            self._jog_source = src
            threading.Thread(target=self._jog_run, args=(src,), daemon=True).start()

    def _jog_run(self, source):
        """Runs one open jog session until its distance is spent."""
        self.busy = True
        self.last_command_status = f"JOG {source.ltr.upper()}{'+' if source.sign > 0 else '-'}"
        self._notify()
        try:
            self.link.reset_seq()
            sess = self.link.session(source, window=16)
            self._jog_session = sess
            ok = sess.run()
            self.last_command_status = (
                f"Jog done ({source.steps_total} steps, {source.clicks} click(s), "
                f"{source.emitted} pkts)" if ok else "Jog failed")
        except Exception as e:
            self.last_command_status = f"Jog error: {e}"
        finally:
            with self._jog_lock:
                self._jog_source = None
                self._jog_session = None
            self.busy = False
            self._notify()

    # ---------------------------------------------------------
    # 4. Data Plane (Jogging)
    # ---------------------------------------------------------
    def jog(self, ltr: str, sign: int, dist: float, rate: float):
        """Queue a jog burst for a specific axis."""
        from host.protocol.packets import make_jog

        # Proportional to feed rate rather than a fixed value — a flat accel is
        # either too gentle at low feed or (per prior UI) coarse/violent at high
        # feed, since make_jog only has ~10ms to spend per velocity step.
        accel = max(rate * 8.0, 50.0)

        if self._gui_op is not None:
            # A job owns `link`. Queuing anyway would let _jog_worker stream onto
            # the port while the job worker is mid-stream — two writers on one
            # serial port, and the jog's seqreset resets the Pico's duplicate-guard
            # counter mid-job, silently dropping job steps. Queuing behind another
            # in-flight jog is fine: they share the same single-writer worker.
            self.last_command_status = "Rejected: job in progress"
            self._notify()
            return

        if not self.app_state.config or not self.app_state.config.machine:
            return

        # Get axis config
        machine = self.app_state.config.machine
        axes = dict(machine.present_axes())
        if ltr not in axes:
            return

        # Reject if the axis is not enabled
        if self.machine_state and not self.machine_state.enabled(ltr):
            self.last_command_status = f"Rejected: {ltr.upper()} axis is disabled"
            self._notify()
            return

        ax = axes[ltr]

        # Calculate in steps
        feed_sps = rate * ax.steps_per_unit
        accel_sps2 = accel * ax.steps_per_unit
        dist_steps = int(dist * sign * ax.steps_per_unit)
        if dist_steps == 0:
            return

        # Build the vector
        vec = [0, 0, 0, 0]
        idx_map = {"x": 0, "y": 1, "z": 2, "a": 3}
        vec[idx_map[ltr]] = dist_steps

        # Build the complete accel->cruise->decel burst up front and enqueue it
        # as one atomic unit. Splitting a single jog across two stream() calls
        # (as an earlier "blending" attempt did) leaves a real gap on hardware:
        # stream() only blocks for ACK, not for physical motion, so the Pico's
        # buffer drains and the machine visibly stops before the next call
        # arrives. One call per jog is what the old jog_ui.py/jog.py proved
        # smooth; rapid clicks simply queue up and run back-to-back.
        packets = make_jog(tuple(vec), feed_sps, accel_sps2, machine.f_cpu)
        if not packets:
            return

        self.jog_q.put((f"JOGGING {ltr.upper()} {sign*dist:+.1f}", packets))
        self.last_command_status = f"Queued Jog {ltr.upper()}"
        self._notify()

    def _jog_worker(self):
        """Background thread that sends jog bursts while pausing the polling loop."""
        while True:
            try:
                label, packets = self.jog_q.get()

                # Signal busy so polling loop skips
                self.busy = True
                self.last_command_status = label

                if self.jog_dump_to_file:
                    # Bypass the link entirely — append this burst's packets to
                    # jog_output.bin instead of streaming them anywhere.
                    try:
                        with open(self.jog_dump_path, "ab") as f:
                            for p in packets:
                                f.write(struct.pack("<H", len(p)))
                                f.write(p)
                        self.last_command_status = f"Dumped {len(packets)} pkts -> {self.jog_dump_path}"
                    except Exception as e:
                        self.last_command_status = f"Jog dump error: {e}"
                    self.jog_q.task_done()
                    self.busy = False
                    continue

                if not self.is_connected or not self.link:
                    self.jog_q.task_done()
                    self.busy = False
                    continue

                try:
                    # The wire protocol requires seqreset before each stream
                    self.link.command("seqreset")
                    # Send the packets using the built-in Go-Back-N sender
                    success = self.link.stream(packets)
                    if success:
                        self.last_command_status = f"Jog complete ({len(packets)} pkts)"
                    else:
                        self.last_command_status = "Jog aborted/failed"
                except Exception as e:
                    self.last_command_status = f"Jog error: {e}"

                self.jog_q.task_done()
                self.busy = False

            except Exception as e:
                self.busy = False

    # ---------------------------------------------------------
    # 5. Job Execution
    # ---------------------------------------------------------
    def run_job(self):
        """Starts a background thread to execute the loaded plan."""
        # jog_q.empty() closes the race where _jog_worker flips `busy` False
        # in the gap between draining two queued jogs — a job must not start
        # while jogs are still pending behind it (see jog()'s single-writer note).
        if self.busy or not self.jog_q.empty() or not self.is_connected or not self.app_state.plan:
            return
            
        plan = self.app_state.plan
        machine = self.app_state.config.machine
        link = self.link
        
        # Build the initial summary to persist in the log
        op_count = len(plan.operations)
        summary = "\n".join(f"  {i+1}. {op.tool}  ({len(op.packets)} segments)" for i, op in enumerate(plan.operations))
        initial_log = f"Plan summary ({op_count} operations):\n{summary}\n"
        
        self._gui_op = GuiOperator(self)
        self.job_notes = [initial_log, "Starting job..."]
        self.busy = True
        self.last_command_status = "RUNNING JOB"
        self._notify()
        
        gui_op = self._gui_op

        def _on_progress(status, pos):
            # Called from this same worker thread by send_plan's _wait_state
            # poll — no second reader of `link` needed. Both fields come from
            # one STATUS_RSP, so they describe the same instant. (_poll_worker
            # also runs throughout now; this just reports at the job's cadence
            # rather than the poller's.)
            self.machine_state = status
            if pos is not None:
                self.machine_pos_steps = pos
            self._notify()

        def _worker():
            try:
                from host.execution.job_runner import send_plan
                from host.execution.preflight import preflight

                gui_op.note("--- PRE-FLIGHT ---")
                profile = plan.operations[0].profile
                pf = preflight(link, machine, profile, require_idle=True)
                gui_op.note(str(pf))

                if not pf.ok:
                    gui_op.note("\nPre-flight failed. Job aborted.")
                    return

                gui_op.note("\n--- EXECUTION ---")
                ok, msg = send_plan(plan, machine, link, gui_op, on_progress=_on_progress)
                gui_op.note("\nDone: " + msg if ok else "\nFailed: " + msg)
            except Exception as e:
                gui_op.note(f"error: {e}")
            finally:
                self.busy = False
                self._gui_op = None
                self._notify()
                
        threading.Thread(target=_worker, daemon=True).start()

    def pause_job(self):
        from host.protocol import commands as cmd
        self._send_command(cmd.pause)

    def resume_job(self):
        from host.protocol import commands as cmd
        self._send_command(cmd.resume)

    def cancel_job(self):
        from host.protocol import commands as cmd
        self._send_command(cmd.cancel)
