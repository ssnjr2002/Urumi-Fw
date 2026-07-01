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
        
        # Add basic mousewheel support for Windows
        self.canvas.bind_all("<MouseWheel>", lambda e: self.canvas.yview_scroll(int(-1*(e.delta/120)), "units"))

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
        # TODO: Map buttons to self.session methods once OnlineSession is created.
        pass

    def _update_ui(self):
        # TODO: Unpack state from self.session and update the views.
        pass

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
