import threading
from typing import Callable, List, Optional

try:
    from serial.tools import list_ports
except ImportError:
    list_ports = None

from host.protocol.link import Link
from host.ui.app_state import AppState

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
