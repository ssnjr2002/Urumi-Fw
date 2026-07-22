import threading
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

# Temporary jog timing instrumentation. Set JOG_DEBUG=1 to print a per-jog
# breakdown (click -> reset_seq -> session -> machine drain) to stdout.
import os
from host.protocol.state import MachineState
JOG_DEBUG = os.environ.get("JOG_DEBUG") == "1"
_T_BOOT = time.monotonic()      # common clock for the poll / UI traces


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

    # The queue must outlast the control loop. Status arrives every ~100 ms, so
    # that is how long the source can be flying blind; a buffer shallower than
    # that MUST underflow no matter how good the estimate is. The old
    # LOW_WATER=4 x CHUNK_MS=20 capped the queue at 80 ms — below the poll
    # period — so the ring drained to empty on every jog and the machine
    # visibly stopped and restarted. That was the root cause; the estimator
    # tuning that preceded it was treating a symptom.
    #
    # Depth is affordable now because cancel is a soft abort (§4.5): the Pico
    # flushes the ring on one byte, so a deeper queue no longer costs
    # responsiveness. Under the old coast-to-a-stop it would have.
    LOW_WATER = 16       # segments — ~320 ms at CHUNK_MS, ~3 poll intervals
    CHUNK_MS  = 20       # motion per emitted packet
    V_START   = 50.0     # steps/s — rest velocity, matches make_jog
    LEAD_US   = 250_000  # keep at most this much motion-time queued ahead (µs)
    MAX_BURST = 16       # packets per pull() — bounds one call, LEAD_US bounds
                         # the queue. Without a burst the source cannot outpace
                         # its own pull cadence (see pull()).

    def __init__(self, machine, axis, ltr, sign, rate, link=None, dump_path=None):
        self.machine, self.axis = machine, axis
        self.ltr, self.sign, self.rate = ltr, sign, rate
        self.link = link
        # Debug tee: when set, every packet handed to the wire is ALSO appended
        # here. It cannot be an "instead of the wire" mode — pacing below is
        # closed-loop on machine telemetry, so with no link there are no status
        # samples and the emitted stream would not resemble a real jog. A
        # capture of what was genuinely sent is the only honest artifact.
        self.dump_path = dump_path
        self.emitted = 0
        self.steps_total = 0        # steps actually committed to the wire
        self.clicks = 1

        self._remaining = 0.0       # steps still to travel
        self._v = self.V_START      # current velocity, ramped across chunks
        self._finished = False
        self._cancelled = False     # reversal/stop — the Pico is ramping
        self._lock = threading.Lock()
        self._wake = threading.Event()

        # Pacing: the report ANCHORS, the clock INTERPOLATES.
        #
        # `queued_us` is authoritative but only as fresh as the last status
        # sample (~100 ms), while pull() runs as fast as the ack loop allows —
        # so it cannot be used alone without dumping the move between polls.
        # A pure wall-clock estimate cannot be used either: it subtracts elapsed
        # time whether or not the machine was executing, so every dry spell
        # biases it low PERMANENTLY. Measured: it drifted until the ring held
        # 260 ms against an 80 ms target, and the over-fill caused more dry
        # spells, which drifted it further.
        #
        # So each fresh sample resets the anchor, and between samples we add
        # what we have emitted and subtract what has elapsed. Error is bounded
        # by one poll interval instead of accumulating.
        self._trace = []            # JOG_DEBUG only: (t, steps, v_avg, ms)
        self._t_open = time.monotonic()
        self._anchor_us = 0.0       # queued_us as of the last sample we used
        self._anchor_t  = None      # monotonic() when that sample landed
        self._anchor_stamp = -1     # which sample it was
        self._since_anchor_s = 0.0  # motion-seconds emitted since then

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
        # axis.invert is a WIRING correction, so it has to apply to every source
        # of motion or the machine has two disagreeing coordinate frames. The
        # planner applies it, so it defines the frame and jog conforms.
        #
        # This was missing until now, here and in the OnlineSession.jog path
        # before it: X, Z and A are invert=True in the default config, so jog
        # drove those axes opposite to a job commanding the same direction. It
        # survived because every hardware run has been Pico-only — position
        # counters advance identically either way, so nothing short of a motor
        # could show it.
        inv = -1 if getattr(self.axis, "invert", False) else 1
        vec[self._idx] = int(inv * self.sign * steps)
        interval = max(1, min(int(self.machine.f_cpu / max(v_avg, 1.0)),
                              self.machine.f_cpu))
        pkt = pack_jog(_JMS(dx=vec[0], dy=vec[1], dz=vec[2], da=vec[3],
                            interval=interval, flags=MSEG_FLAG_NONE))
        if self.dump_path:
            # Length-prefixed, same framing verify_packets.py reads. Best-effort:
            # a debug capture must never take down a move in progress.
            try:
                with open(self.dump_path, "ab") as f:
                    f.write(struct.pack("<H", len(pkt)))
                    f.write(pkt)
            except Exception:
                pass
        return pkt

    # -- the source contract --------------------------------------------------

    def _lead_us(self, ctx):
        """Motion time queued ahead of the machine, in µs.

        Anchored on the newest status sample, extrapolated to now:

            lead = reported_at_sample + emitted_since - elapsed_since

        Re-anchoring on every fresh sample is what stops the estimate drifting.
        Before any sample arrives there is nothing to anchor to, so it falls
        back to pure extrapolation from zero — correct at the start of a move,
        which is the only time it is used that way.
        """
        queued, stamp, at = ctx.queued_sample
        if queued is not None and stamp != self._anchor_stamp:
            self._anchor_stamp   = stamp
            self._anchor_us      = float(queued)
            self._anchor_t       = at
            self._since_anchor_s = 0.0

        if self._anchor_t is None:
            return max(0.0, self._since_anchor_s * 1e6)

        elapsed_us = (time.monotonic() - self._anchor_t) * 1e6
        return max(0.0, self._anchor_us + self._since_anchor_s * 1e6 - elapsed_us)

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
            return (buf is not None and buf > 0) or self._lead_us(ctx) > 0
        return queued > 0 or self._lead_us(ctx) > 0

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

        # Pace on the anchored estimate (see _lead_us). buf_count stays as the
        # hard ceiling — the ring is finite regardless of what any time-based
        # measure claims.
        buf = ctx.buf_count
        if self._lead_us(ctx) >= self.LEAD_US or (buf is not None and buf >= self.LOW_WATER):
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
               and self._lead_us(ctx) < self.LEAD_US):

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
            self._since_anchor_s += steps / v_avg
            self.emitted += 1
            if JOG_DEBUG:
                self._trace.append((round(time.monotonic() - self._t_open, 3),
                                    steps, round(v_avg, 1),
                                    round(steps / v_avg * 1000, 1)))
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
        self.connected_at = None   # monotonic() at link open; drives the UI timer
        self.polling_error: Optional[str] = None
        self.busy = False  # Set to True when job/jog is streaming
        
        # --- Command State ---
        self.last_command_status = "—"
        self.node_ping_status = {}
        
        # --- Jogging State ---
        # Debug capture: when on, jog packets are ALSO appended to jog_output.bin
        # (length-prefixed, same framing verify_packets.py reads) as they go to
        # the wire. Read at the start of each jog, so toggling it mid-move does
        # not take effect until the next click.
        self.jog_dump_to_file = False
        self.jog_dump_path = "jog_output.bin"

        # --- Manual jog state ---
        self._jog_source = None      # the open session's PacketSource, if running
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
        # Tk event loop. It runs CONTINUOUSLY, including while a job or jog is
        # streaming — see _poll_worker's docstring for why that is safe now.
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
            # Free-running session clock — the UI reads this to display elapsed
            # time. Set only on a successful open, so the display cannot imply a
            # connection that is not there.
            self.connected_at = time.monotonic()
        except Exception as e:
            self.link = None
            self.connection_error = str(e)
            self.connected_at = None

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
            self.connected_at = None
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
                t0 = time.monotonic()
                st = self.link.get_status(timeout=0.5)
                dt = time.monotonic() - t0
                self.machine_state = st
                if st.pos is not None:
                    self.machine_pos_steps = st.pos
                self.polling_error = None
                if JOG_DEBUG:
                    # DATA-side timeline: when the poller learned each fact.
                    print(f"[poll {time.monotonic()-_T_BOOT:7.3f}] "
                          f"rtt {dt*1000:5.1f}ms  {st.state.name:7s} "
                          f"buf {st.buf_count:3d} queued {st.queued_us:7d} "
                          f"pos {st.pos[0]}", flush=True)
            except Exception as e:
                self.polling_error = str(e)
                if JOG_DEBUG:
                    print(f"[poll {time.monotonic()-_T_BOOT:7.3f}] ERROR {e}", flush=True)

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
    # 4. Manual jogging — an OPEN session
    # ---------------------------------------------------------
    # One click = one fixed distance, so each click's distance IS known up front.
    # The session is still open (docs/comms_architecture.md §2.3) because the
    # TOTAL is not: a click arriving mid-move extends the move rather than
    # starting a new one, so the packet sequence depends on input that has not
    # happened yet. It ends when the distance is spent and the machine has
    # drained, or early by truncation on a reversal.

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

            src = _ClickJogSource(
                machine, ax, ltr, sign, rate, link=self.link,
                dump_path=self.jog_dump_path if self.jog_dump_to_file else None)
            src.add(steps)
            src.clicks = 1
            self._jog_source = src
            # Claim the port here, under the lock, NOT in _jog_run. Setting it
            # in the thread leaves a window where this jog is committed but
            # `busy` is still False, and run_job()'s guard would wave a job
            # through onto the same port — two writers, plus a seqreset landing
            # mid-job. Publish the claim before the thread that acts on it.
            self.busy = True
            threading.Thread(target=self._jog_run, args=(src,), daemon=True).start()

    def _jog_run(self, source):
        """Runs one open jog session until its distance is spent.

        `busy` is already True — jog_click set it under the lock before
        spawning us. The `finally` below is what clears it.
        """
        t_click = time.monotonic()
        _t = (lambda: f"{t_click - self.connected_at:.3f}s"
              if self.connected_at else "?")
        self.last_command_status = (
            f"JOG {source.ltr.upper()}{'+' if source.sign > 0 else '-'} "
            f"started at {_t()}")
        self._notify()

        dbg = JOG_DEBUG
        pos0 = list(self.machine_pos_steps or [0, 0, 0, 0])
        if dbg:
            print(f"\n=== JOG {source.ltr.upper()}{'+' if source.sign>0 else '-'} "
                  f"{source.rate} u/s ===")
            print(f"  steps_per_unit {source.axis.steps_per_unit}  "
                  f"feed {source.feed_sps:.1f} sps  accel {source.accel_sps2:.0f} sps^2")
            print(f"  requested {source._remaining:.0f} steps "
                  f"-> expected {source._remaining / max(source.feed_sps,1):.2f} s of motion")
            print(f"  start pos {pos0}")

        try:
            self.link.reset_seq()
            t_seq = time.monotonic()
            sess = self.link.session(source, window=16)
            ok = sess.run()
            t_sess = time.monotonic()
            # Stamp the same clock the on-screen timer shows, so "what the UI
            # said" and "what actually happened" can be compared without a
            # stopwatch — which is the whole reason the timer exists.
            span = t_sess - t_click
            end_at = (f"{t_sess - self.connected_at:.3f}s"
                      if self.connected_at else "?")
            self.last_command_status = (
                f"Jog done in {span:.3f}s (ended {end_at}) — "
                f"{source.steps_total} steps, {source.clicks} click(s), "
                f"{source.emitted} pkts" if ok else "Jog failed")

            if dbg:
                # Wait for the machine itself to report IDLE with an empty ring —
                # the session ending only means the last packet was ACKed.
                t_drain = None
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    st = self.machine_state
                    if st and st.state == MachineState.IDLE and not st.buf_count:
                        t_drain = time.monotonic()
                        break
                    time.sleep(0.01)
                pos1 = list(self.machine_pos_steps or [0, 0, 0, 0])
                moved = [b - a for a, b in zip(pos0, pos1)]
                print(f"  reset_seq        {(t_seq  - t_click)*1000:8.0f} ms")
                print(f"  session.run()    {(t_sess - t_seq  )*1000:8.0f} ms")
                if t_drain:
                    print(f"  machine drain    {(t_drain - t_sess)*1000:8.0f} ms")
                    print(f"  TOTAL click->idle{(t_drain - t_click)*1000:8.0f} ms")
                print(f"  moved {moved}  ({source.steps_total} steps commanded, "
                      f"{source.emitted} pkts, {source.clicks} click(s))")
                tr = source._trace
                if tr:
                    span = tr[-1][0] - tr[0][0]
                    motion_ms = sum(r[3] for r in tr)
                    print(f"  emitted {len(tr)} pkts over {span*1000:.0f} ms wall, "
                          f"carrying {motion_ms:.0f} ms of motion")
                    print("   t(s)  steps  v_avg(sps)  motion(ms)")
                    for r in tr[:6]:
                        print(f"   {r[0]:5.3f} {r[1]:6d} {r[2]:11.1f} {r[3]:11.1f}")
                    if len(tr) > 12:
                        print(f"   ... {len(tr)-12} more ...")
                    for r in tr[-6:]:
                        print(f"   {r[0]:5.3f} {r[1]:6d} {r[2]:11.1f} {r[3]:11.1f}")
        except Exception as e:
            self.last_command_status = f"Jog error: {e}"
            if dbg:
                import traceback; traceback.print_exc()
        finally:
            with self._jog_lock:
                self._jog_source = None
            self.busy = False
            self._notify()

    # ---------------------------------------------------------
    # 5. Job Execution
    # ---------------------------------------------------------
    def run_job(self):
        """Starts a background thread to execute the loaded plan."""
        # `busy` alone is the interlock now that jogs no longer queue: jog_click
        # sets it under _jog_lock before spawning the runner, and _jog_run's
        # `finally` clears it, so there is no gap where a committed jog looks
        # idle. A job and a jog must never both hold the port — two writers, and
        # the jog's seqreset would reset the Pico's duplicate guard mid-job.
        if self.busy or not self.is_connected or not self.app_state.plan:
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
