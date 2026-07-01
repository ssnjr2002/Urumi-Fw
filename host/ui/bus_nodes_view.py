import tkinter as tk
from tkinter import ttk

class BusNodesView(ttk.Frame):
    """
    The View for the Bus Nodes section of the Online Tab.
    Dynamically generates rows for non-axis peripherals based on machine config.
    """
    def __init__(self, parent, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        
        # Dictionaries to hold references to dynamic widgets/vars keyed by node_id
        self.status_vars = {}
        self.ping_btns = {}
        self.enable_btns = {}
        self.disable_btns = {}
        
        # Frame to hold the dynamic rows
        self.inner_frame = ttk.Frame(self)
        self.inner_frame.pack(fill="both", expand=True, padx=8, pady=8)
        
        # Configure columns to match our UI Architecture Matrix
        self.inner_frame.columnconfigure(0, weight=1, minsize=150)
        self.inner_frame.columnconfigure(1, weight=1, minsize=150)
        self.inner_frame.columnconfigure(2, weight=2, minsize=450)
        
    def populate(self, bus_nodes):
        """
        Clears existing rows and generates new ones based on the provided list of nodes.
        Expects an iterable of objects with `node_id` and `role` attributes.
        """
        # Clear existing rows if any
        for widget in self.inner_frame.winfo_children():
            widget.destroy()
            
        self.status_vars.clear()
        self.ping_btns.clear()
        self.enable_btns.clear()
        self.disable_btns.clear()
        
        if not bus_nodes:
            ttk.Label(self.inner_frame, text="No peripherals configured.", font=("TkDefaultFont", 9, "italic")).grid(row=0, column=0, padx=4, pady=4, sticky="w")
            return
            
        for row, node in enumerate(bus_nodes):
            # ---------------------------------------------------------
            # Column 0: Definition
            # ---------------------------------------------------------
            ttk.Label(self.inner_frame, text=f"{node.role} (Node {node.node_id})").grid(row=row, column=0, padx=4, pady=4, sticky="w")
            
            # ---------------------------------------------------------
            # Column 1: State/Status
            # ---------------------------------------------------------
            var = tk.StringVar(value="Ping: —, State: —")
            self.status_vars[node.node_id] = var
            ttk.Label(self.inner_frame, textvariable=var).grid(row=row, column=1, padx=4, pady=4, sticky="w")
            
            # ---------------------------------------------------------
            # Column 2: Control
            # ---------------------------------------------------------
            ctrl_frm = ttk.Frame(self.inner_frame)
            ctrl_frm.grid(row=row, column=2, padx=4, pady=4, sticky="w")
            
            ping_btn = ttk.Button(ctrl_frm, text="Ping")
            ping_btn.pack(side="left", padx=2)
            self.ping_btns[node.node_id] = ping_btn
            
            enable_btn = ttk.Button(ctrl_frm, text="Enable")
            enable_btn.pack(side="left", padx=2)
            self.enable_btns[node.node_id] = enable_btn
            
            disable_btn = ttk.Button(ctrl_frm, text="Disable")
            disable_btn.pack(side="left", padx=2)
            self.disable_btns[node.node_id] = disable_btn

# Simple runner for preview
if __name__ == "__main__":
    from dataclasses import dataclass
    
    @dataclass
    class MockNode:
        node_id: int
        role: str
        
    root = tk.Tk()
    root.title("Preview: BusNodesView")
    root.geometry("650x200")
    
    view = BusNodesView(root)
    view.pack(fill="both", expand=True, padx=10, pady=10)
    
    # Mock data to preview the dynamic generation
    mock_nodes = [
        MockNode(node_id=10, role="Spindle Controller"),
        MockNode(node_id=11, role="Coolant Pump")
    ]
    view.populate(mock_nodes)
    
    root.mainloop()
