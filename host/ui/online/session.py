import threading
import queue
import struct
import time
from typing import Callable, List, Optional

try:
    from serial.tools import list_ports
except ImportError:
    list_ports = None

from host.protocol.link import Link
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
        Background thread: polls machine state/position at ~400ms while idle.
        Runs off the Tk thread so a wedged Pico (blocking serial reads, up to
        Link.command's 1s timeout per call) degrades to a stale UI instead of
        freezing the whole GUI. Skips itself while busy — the jog worker and
        job worker each own `link` exclusively for their duration.
        """
        while True:
            time.sleep(0.4)
            if not self.is_connected or self.busy:
                continue

            from host.protocol import commands as cmd
            try:
                self.machine_state = cmd.get_status(self.link)
                self.machine_pos_steps = cmd.get_pos(self.link)
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
        # `pingnode all` is a firmware bring-up convenience that replies with one
        # line per node, but Link.command() only ever reads a single line — the
        # extra lines desync the next read. Ping each configured node individually
        # instead, which matches the wire protocol's one-line-per-command contract.
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
            all_ok = True
            for node_id in sorted(node_ids):
                ok = cmd.ping_node(self.link, node_id)
                self.node_ping_status[node_id] = "OK" if ok else "TIMEOUT"
                all_ok = all_ok and ok
            self.last_command_status = "OK: ping all nodes" if all_ok else "Timeout/Error: ping all nodes"
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
            # poll — no second reader of `link` needed. Publishes the live
            # state/position _poll_worker would otherwise have shown, since
            # _poll_worker skips itself entirely while self.busy is True.
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
