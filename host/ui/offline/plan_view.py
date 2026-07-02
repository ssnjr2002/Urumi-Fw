import tkinter as tk
from tkinter import ttk

class PlanView(ttk.Frame):
    """
    UI Layout for the Plan Manager section (Offline Mode).
    Contains no business logic, just the visual elements.
    """
    def __init__(self, parent, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        self._build_ui()

    def _build_ui(self):
        # Action Row: Generate / Load
        # Generate is disabled by default since it requires valid config + SVG
        self.generate_btn = ttk.Button(self, text="Generate .plan", state="disabled")
        self.generate_btn.grid(row=0, column=0, padx=8, pady=8, sticky="w")
        
        self.load_btn = ttk.Button(self, text="Load .plan...")
        self.load_btn.grid(row=0, column=1, padx=4, pady=8, sticky="w")
        
        self.file_var = tk.StringVar(value="(no plan loaded)")
        self.file_lbl = ttk.Label(self, textvariable=self.file_var, font=("TkDefaultFont", 9, "italic"))
        self.file_lbl.grid(row=0, column=2, padx=8, pady=8, sticky="w")

        # Status Row (Compatibility checks with Config)
        self.status_var = tk.StringVar(value="Status: Waiting for Plan")
        self.status_lbl = ttk.Label(self, textvariable=self.status_var, wraplength=400)
        self.status_lbl.grid(row=1, column=0, columnspan=3, padx=8, pady=(0, 8), sticky="w")

        # Details Frame 
        self.details_frame = ttk.LabelFrame(self, text="Plan Details")
        self.details_frame.grid(row=2, column=0, columnspan=3, padx=8, pady=(0, 8), sticky="nsew")
        
        # Global header data directly from the binary spec
        self.header_var = tk.StringVar(value="Version: — | Unique Tools: — | Total Ops: —")
        self.header_lbl = ttk.Label(self.details_frame, textvariable=self.header_var, font=("TkDefaultFont", 9, "bold"))
        self.header_lbl.grid(row=0, column=0, padx=4, pady=4, sticky="w")

        # Deferred Metadata
        self.meta_var = tk.StringVar(value="Est. Time: — | Est. Distance: —")
        self.meta_lbl = ttk.Label(self.details_frame, textvariable=self.meta_var)
        self.meta_lbl.grid(row=1, column=0, padx=4, pady=(0, 4), sticky="w")

        # Paned view to show the two internal structures: Manifest and Operations
        self.pane = ttk.PanedWindow(self.details_frame, orient="horizontal")
        self.pane.grid(row=2, column=0, sticky="nsew", padx=4, pady=4)
        
        # Left side: Tool Manifest (for upfront feasibility check)
        self.tools_frame = ttk.LabelFrame(self.pane, text="Required Tools (Manifest)")
        self.pane.add(self.tools_frame, weight=1)
        
        self.tools_list = tk.Listbox(self.tools_frame, height=5)
        self.tools_list.pack(fill="both", expand=True, padx=2, pady=2)
        
        # Right side: Operations Sequence
        self.ops_frame = ttk.LabelFrame(self.pane, text="Operation Sequence")
        self.pane.add(self.ops_frame, weight=2)
        
        columns = ("op_idx", "tool", "packets")
        self.ops_tree = ttk.Treeview(self.ops_frame, columns=columns, show="headings", height=5)
        self.ops_tree.heading("op_idx", text="#")
        self.ops_tree.heading("tool", text="Tool Type")
        self.ops_tree.heading("packets", text="Packets")
        self.ops_tree.column("op_idx", width=40, anchor="center")
        self.ops_tree.column("tool", width=100)
        self.ops_tree.column("packets", width=80, anchor="e")
        self.ops_tree.pack(side="left", fill="both", expand=True, padx=2, pady=2)
        
        self.ops_scroll = ttk.Scrollbar(self.ops_frame, orient="vertical", command=self.ops_tree.yview)
        self.ops_scroll.pack(side="right", fill="y")
        self.ops_tree.configure(yscrollcommand=self.ops_scroll.set)
        
        # Configure weights
        self.details_frame.rowconfigure(2, weight=1)
        self.details_frame.columnconfigure(0, weight=1)
        
        self.rowconfigure(2, weight=1)
        self.columnconfigure(2, weight=1)

# A simple runner to preview the layout directly
if __name__ == "__main__":
    root = tk.Tk()
    root.title("Layout Preview: PlanView")
    root.geometry("550x300")
    
    view = PlanView(root)
    view.pack(fill="both", expand=True, padx=10, pady=10)
    
    root.mainloop()
