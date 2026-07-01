import tkinter as tk
from tkinter import ttk
import sys
import os

# Allow running this file directly for preview by adding the project root to sys.path
if __name__ == "__main__":
    sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))

from host.ui.master_view import MasterView
from host.ui.bus_nodes_view import BusNodesView
from host.ui.axis_nodes_view import AxisNodesView
from host.ui.job_execution_view import JobExecutionView
from host.ui.draggable_container import ReorderableContainer

class OnlineTab(ttk.Frame):
    """
    The main Online (Execution) UI tab.
    Acts as the Controller, bridging the dumb UI views to the OnlineSession logic.
    """
    def __init__(self, parent, session, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        self.session = session
        self._build_ui()
        self._bind_logic()

    def _build_ui(self):
        # Create a scrollable canvas
        self.canvas = tk.Canvas(self, highlightthickness=0)
        self.scrollbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.scrollbar.set)
        
        self.scrollbar.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        
        # Inner frame to hold the actual views
        self.scrollable_frame = ttk.Frame(self.canvas)
        self.scrollable_window = self.canvas.create_window((0, 0), window=self.scrollable_frame, anchor="nw")
        
        # Configure scrolling boundaries when inner frame resizes
        self.scrollable_frame.bind(
            "<Configure>",
            lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        )
        
        # Keep inner frame the same width as the canvas
        self.canvas.bind(
            "<Configure>",
            lambda e: self.canvas.itemconfig(self.scrollable_window, width=e.width)
        )
        
        # Add robust mousewheel support for Windows (handles precision trackpads and multi-tab isolation)
        def _on_mousewheel(e):
            if not self.winfo_ismapped():
                return
            units = int(-1 * (e.delta / 120))
            if units == 0 and e.delta != 0:
                units = -1 if e.delta > 0 else 1
            self.canvas.yview_scroll(units, "units")
            
        self.canvas.bind_all("<MouseWheel>", _on_mousewheel, add="+")

        # Create the Reorderable Container
        self.container = ReorderableContainer(self.scrollable_frame)
        self.container.pack(fill="both", expand=True, padx=4, pady=4)

        # Wrap and add each view to the container
        self.master_wrapper = self.container.add_widget("Master Control", MasterView)
        self.master_view = self.master_wrapper.inner_widget
        
        self.bus_nodes_wrapper = self.container.add_widget("Bus Nodes", BusNodesView)
        self.bus_nodes_view = self.bus_nodes_wrapper.inner_widget
        
        self.axis_nodes_wrapper = self.container.add_widget("Axis Nodes", AxisNodesView)
        self.axis_nodes_view = self.axis_nodes_wrapper.inner_widget
        
        self.job_execution_wrapper = self.container.add_widget("Job Execution", JobExecutionView)
        self.job_execution_view = self.job_execution_wrapper.inner_widget

    def _bind_logic(self):
        # Subscribe to AppState to dynamically build UI when the Config is loaded
        if hasattr(self.session, 'app_state'):
            self.session.app_state.subscribe(self._on_app_state_changed)
            self._on_app_state_changed()

        # Subscribe to OnlineSession for execution state changes
        if hasattr(self.session, 'subscribe'):
            self.session.subscribe(self._update_ui)
            
        # Populate available ports
        if hasattr(self.session, 'available_ports'):
            self.master_view.port_combo['values'] = self.session.available_ports
            
        # Bind Connect button
        self.master_view.connect_btn.config(command=self._on_connect_clicked)
        
        # Bind Control buttons
        if hasattr(self.session, 'enable_all'):
            self.master_view.enable_all_btn.config(command=self.session.enable_all)
            self.master_view.disable_all_btn.config(command=self.session.disable_all)
            self.master_view.set_origin_btn.config(command=self.session.set_origin_all)
            self.master_view.estop_btn.config(command=self.session.estop)
            self.master_view.unalarm_btn.config(command=self.session.unalarm)
        
        # Start the polling loop
        self._poll_loop()

    def _poll_loop(self):
        if hasattr(self.session, 'poll_status'):
            self.session.poll_status()
        self.after(400, self._poll_loop)

    def _on_connect_clicked(self):
        port = self.master_view.port_var.get()
        if hasattr(self.session, 'toggle_connect'):
            self.session.toggle_connect(port)

    def _on_app_state_changed(self):
        config = self.session.app_state.config
        if not config:
            return
            
        # Update available ports based on config type
        if hasattr(self.session, 'available_ports'):
            ports = self.session.available_ports
            self.master_view.port_combo['values'] = ports
            
            # Reset selection if the current one is no longer valid
            if ports and self.master_view.port_var.get() not in ports:
                self.master_view.port_var.set(ports[0])
        
        machine = config.machine
        
        # Populate Bus Nodes (Peripherals + Axes)
        all_nodes = []
        self.node_to_axis = {}
        if hasattr(machine, 'present_axes'):
            for ltr, ax in machine.present_axes():
                all_nodes.append(ax.node)
                self.node_to_axis[ax.node.node_id] = ltr
                
        peripherals = getattr(machine, 'peripherals', [])
        all_nodes.extend(peripherals)
        
        # Sort by node_id for predictable display
        all_nodes.sort(key=lambda n: getattr(n, 'node_id', 0))
        
        self.bus_nodes_view.populate(all_nodes)
        
        # Bind Bus Nodes Ping Buttons dynamically
        self.bus_nodes_view.ping_all_btn.config(command=self.session.ping_all)
        for node_id, btn in self.bus_nodes_view.ping_btns.items():
            btn.config(command=lambda n=node_id: self.session.ping_node(n))
            
        # Bind individual enable/disable buttons
        for node_id, btn in self.bus_nodes_view.enable_btns.items():
            btn.config(command=lambda n=node_id: self.session.enable_node(n))
        for node_id, btn in self.bus_nodes_view.disable_btns.items():
            btn.config(command=lambda n=node_id: self.session.disable_node(n))
            
        # Populate Axis Nodes
        # machine.present_axes() returns [('x', AxisConfig), ('y', AxisConfig), ...]
        if hasattr(machine, 'present_axes'):
            axes_data = []
            for ltr, axis_cfg in machine.present_axes():
                axes_data.append((ltr, axis_cfg))
            self.axis_nodes_view.populate(axes_data)
            
        # Apply the current session state (e.g. connection status) to the newly populated views
        self._update_ui()
        
    def _update_ui(self):
        if not hasattr(self.session, 'is_connected'):
            return
            
        # Master View: Connection State
        if self.session.is_connected:
            self.master_view.connect_btn.config(text="Disconnect")
            
            # Bus Nodes View: Enable Controls (if node is present)
            self.bus_nodes_view.ping_all_btn.config(state="normal")
            for node_id, btn in self.bus_nodes_view.ping_btns.items():
                is_present = getattr(self.bus_nodes_view, 'node_presence', {}).get(node_id, True)
                btn.config(state="normal" if is_present else "disabled")
            for node_id, btn in self.bus_nodes_view.enable_btns.items():
                is_present = getattr(self.bus_nodes_view, 'node_presence', {}).get(node_id, True)
                btn.config(state="normal" if is_present else "disabled")
            for node_id, btn in self.bus_nodes_view.disable_btns.items():
                is_present = getattr(self.bus_nodes_view, 'node_presence', {}).get(node_id, True)
                btn.config(state="normal" if is_present else "disabled")
            
            # Fetch polling state
            st = getattr(self.session, 'machine_state', None)
            err = getattr(self.session, 'polling_error', None)
            
            # Update Bus Nodes State Text
            for node_id, var in self.bus_nodes_view.status_vars.items():
                is_present = getattr(self.bus_nodes_view, 'node_presence', {}).get(node_id, True)
                if not is_present:
                    var.set("Ping: —, State: Not Fitted")
                else:
                    ping_stat = getattr(self.session, 'node_ping_status', {}).get(node_id, "—")
                    if st:
                        ltr = getattr(self, 'node_to_axis', {}).get(node_id)
                        if ltr:
                            state_text = "ENABLED" if st.enabled(ltr) else "DISABLED"
                        else:
                            state_text = "N/A" # Peripheral node states are opaque in Phase 1
                        var.set(f"Ping: {ping_stat}, State: {state_text}")
                    else:
                        var.set(f"Ping: {ping_stat}, State: —")
            
            # Fetch command state
            cmd_stat = getattr(self.session, 'last_command_status', "—")
            self.master_view.cmd_status_var.set(cmd_stat)
            
            if err:
                self.master_view.state_var.set(f"Error: {err}")
                self.master_view.state_lbl.config(foreground="red")
            elif st:
                # Update State & Reason
                state_name = st.state.name
                if st.alarm.value:
                    state_name = f"{state_name} ({st.alarm.name})"
                    
                _STATE_COLOR = {
                    "IDLE": "green", "RUNNING": "blue", "PAUSED": "orange",
                    "ESTOP": "red", "ALARM": "red", "HOMING": "purple",
                }
                self.master_view.state_var.set(state_name)
                self.master_view.state_lbl.config(foreground=_STATE_COLOR.get(st.state.name, "black"))
                
                # Update Enabled/Homed strings for MasterView
                config = getattr(self.session.app_state, 'config', None)
                if config and hasattr(config.machine, 'present_axes'):
                    axes = config.machine.present_axes()
                    
                    enabled_str = "".join(l.upper() for l, _ in axes if st.enabled(l)) or "—"
                    homed_str = "".join(l.upper() for l, _ in axes if st.homed(l)) or "—"
                    self.master_view.enabled_status_var.set(enabled_str)
                    self.master_view.homed_status_var.set(homed_str)
                    
                    # Update AxisNodesView positions and status
                    pos_steps = getattr(self.session, 'machine_pos_steps', None)
                    if pos_steps:
                        idx_map = {"x": 0, "y": 1, "z": 2, "a": 3}
                        for ltr, ax in axes:
                            # Position
                            if ltr in self.axis_nodes_view.pos_vars:
                                pos_val = pos_steps[idx_map[ltr]] / ax.steps_per_unit
                                unit = "deg" if getattr(ax, "rotary", False) else "mm"
                                self.axis_nodes_view.pos_vars[ltr].set(f"Pos: {pos_val:.2f} {unit}")
                                
                            # Homed status per axis
                            if ltr in self.axis_nodes_view.homed_vars:
                                is_homed = st.homed(ltr)
                                self.axis_nodes_view.homed_vars[ltr].set(f"Homed: {'Yes' if is_homed else 'No'}")
            else:
                self.master_view.state_var.set("CONNECTED")
                self.master_view.state_lbl.config(foreground="green")
        else:
            self.master_view.connect_btn.config(text="Connect")
            if self.session.connection_error:
                self.master_view.state_var.set(f"Error: {self.session.connection_error}")
                self.master_view.state_lbl.config(foreground="red")
            else:
                self.master_view.state_var.set("DISCONNECTED")
                self.master_view.state_lbl.config(foreground="black")
                
            # Clear statuses when disconnected
            self.master_view.enabled_status_var.set("—")
            self.master_view.homed_status_var.set("—")
            self.master_view.cmd_status_var.set("—")
            
            # Bus Nodes View: Disable Controls
            self.bus_nodes_view.ping_all_btn.config(state="disabled")
            for btn in self.bus_nodes_view.ping_btns.values():
                btn.config(state="disabled")
            for btn in self.bus_nodes_view.enable_btns.values():
                btn.config(state="disabled")
            for btn in self.bus_nodes_view.disable_btns.values():
                btn.config(state="disabled")

# A simple runner to preview the complete online layout
if __name__ == "__main__":
    from dataclasses import dataclass
    
    @dataclass
    class MockNode:
        node_id: int
        role: str
        
    @dataclass
    class MockAxis:
        node_id: int
        rotary: bool = False
        max_rate: float = 150.0

    class DummySession:
        pass

    root = tk.Tk()
    root.title("Controller Preview: Full Online Tab")
    root.geometry("800x900")
    
    tab = OnlineTab(root, session=DummySession())
    tab.pack(fill="both", expand=True)
    
    # Pre-populate with mock data to visualize the dynamic lists
    tab.bus_nodes_view.populate([
        MockNode(node_id=10, role="Spindle Controller"),
    ])
    tab.axis_nodes_view.populate([
        ("x", MockAxis(node_id=1, max_rate=250.0)),
        ("y", MockAxis(node_id=2, max_rate=250.0)),
        ("z", MockAxis(node_id=3, max_rate=60.0)),
        ("a", MockAxis(node_id=4, rotary=True, max_rate=60.0))
    ])
    
    root.mainloop()
