import threading
import queue
from typing import Callable, List, Optional

try:
    from serial.tools import list_ports
except ImportError:
    list_ports = None

from host.protocol.link import Link
from host.ui.app_state import AppState
from host.job_runner import Operator

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

class OnlineSession:
    """
    Business logic manager for the Online Execution phase.
    Handles the serial link, background polling, and job streaming.
    """
    def __init__(self, app_state: AppState):
        self.app_state = app_state
        self._callbacks: List[Callable] = []
        
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
        self.jog_q = queue.Queue()
        self._worker = threading.Thread(target=self._jog_worker, daemon=True)
        self._worker.start()
        
        # --- Job State ---
        self.job_notes = []
        self.pending_mount = None
        self.mount_ok = True
        self.mount_event = threading.Event()
        self._gui_op = None
        
    def subscribe(self, callback: Callable):
        """UI components register here to be notified of state changes."""
        self._callbacks.append(callback)
        
    def _notify(self):
        """Fire all callbacks when state changes."""
        for cb in self._callbacks:
            cb()
            
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
        """Close the active link."""
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
    def poll_status(self):
        """Called periodically by the UI thread to fetch state without blocking long."""
        if not self.is_connected or self.busy:
            return
            
        from host.protocol import commands as cmd
        try:
            self.machine_state = cmd.get_state(self.link)
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
        from host.protocol import commands as cmd
        if not self.is_connected or self.busy:
            return
            
        try:
            ok = cmd.ping_node(self.link, "all")
            if ok:
                self.last_command_status = "OK: pingnode all"
                if self.app_state.config:
                    machine = self.app_state.config.machine
                    if hasattr(machine, 'present_axes'):
                        for ltr, ax in machine.present_axes():
                            self.node_ping_status[ax.node.node_id] = "OK"
                    peripherals = getattr(machine, 'peripherals', [])
                    for p in peripherals:
                        if getattr(p, 'present', True):
                            self.node_ping_status[p.node_id] = "OK"
            else:
                self.last_command_status = "Timeout/Error: pingnode all"
        except Exception as e:
            self.last_command_status = f"Error pinging all nodes: {e}"
            
        self._notify()

    def enable_node(self, node_id: int):
        self.last_command_status = f"Rejected: node-level enable not supported by Phase 1 protocol"
        self._notify()
        
    def disable_node(self, node_id: int):
        self.last_command_status = f"Rejected: node-level disable not supported by Phase 1 protocol"
        self._notify()

    # ---------------------------------------------------------
    # 4. Data Plane (Jogging)
    # ---------------------------------------------------------
    def jog(self, ltr: str, sign: int, dist: float, rate: float, accel: float):
        """Queue a jog burst for a specific axis."""
        from host.protocol.packets import make_jog
        
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
        
        # Build the vector
        vec = [0, 0, 0, 0]
        idx_map = {"x": 0, "y": 1, "z": 2, "a": 3}
        vec[idx_map[ltr]] = dist_steps
        
        # Generate packets
        packets = make_jog(tuple(vec), feed_sps, accel_sps2, machine.f_cpu)
        if not packets:
            return
            
        # Enqueue the burst
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
        
        def _worker():
            try:
                from host.job_runner import send_plan
                from host.preflight import preflight
                
                gui_op.note("--- PRE-FLIGHT ---")
                profile = plan.operations[0].profile
                pf = preflight(link, machine, profile, require_idle=True)
                gui_op.note(str(pf))
                
                if not pf.ok:
                    gui_op.note("\nPre-flight failed. Job aborted.")
                    return
                    
                gui_op.note("\n--- EXECUTION ---")
                ok, msg = send_plan(plan, machine, link, gui_op)
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
