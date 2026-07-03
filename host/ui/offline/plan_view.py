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
        
        # Job Overrides (tier-3, optional): per-tool feed/accel patch applied
        # right before Generate — only for tools present in the loaded SVG.
        self.overrides_frame = ttk.LabelFrame(self, text="Job Overrides (optional)")
        self.overrides_frame.grid(row=3, column=0, columnspan=3, padx=8, pady=(0, 8), sticky="ew")
        self._override_vars = {}  # tool_name -> {"feed_max": StringVar, "accel": StringVar}
        self._set_no_override_tools()

        # Configure weights
        self.details_frame.rowconfigure(2, weight=1)
        self.details_frame.columnconfigure(0, weight=1)

        self.rowconfigure(2, weight=1)
        self.columnconfigure(2, weight=1)

    def _set_no_override_tools(self):
        ttk.Label(self.overrides_frame, text="(load an SVG to set per-tool feed/accel for this job)").grid(
            row=0, column=0, padx=4, pady=4, sticky="w")

    def set_override_tools(self, tool_names: list, defaults: dict = None):
        """
        Rebuilds the override rows for exactly the tools present in the
        loaded SVG's layers, pre-filled with the resolved config's current
        feed_max/accel for each tool (defaults: {tool_name: {"feed_max":
        float, "accel": float}}, e.g. session.config.tool_profiles) so the
        operator edits a real starting value rather than a blank field.
        """
        for w in self.overrides_frame.winfo_children():
            w.destroy()
        self._override_vars = {}

        if not tool_names:
            self._set_no_override_tools()
            return

        defaults = defaults or {}
        ttk.Label(self.overrides_frame, text="Tool").grid(row=0, column=0, padx=4, pady=2, sticky="w")
        ttk.Label(self.overrides_frame, text="Feed max (mm/s)").grid(row=0, column=1, padx=4, pady=2)
        ttk.Label(self.overrides_frame, text="Accel").grid(row=0, column=2, padx=4, pady=2)
        for i, name in enumerate(tool_names, start=1):
            d = defaults.get(name, {})
            ttk.Label(self.overrides_frame, text=name).grid(row=i, column=0, padx=4, pady=2, sticky="w")
            feed_var = tk.StringVar(value=str(d.get("feed_max", "")))
            accel_var = tk.StringVar(value=str(d.get("accel", "")))
            ttk.Entry(self.overrides_frame, textvariable=feed_var, width=10).grid(row=i, column=1, padx=4, pady=2)
            ttk.Entry(self.overrides_frame, textvariable=accel_var, width=10).grid(row=i, column=2, padx=4, pady=2)
            self._override_vars[name] = {"feed_max": feed_var, "accel": accel_var}

    def get_overrides(self) -> dict:
        """
        {tool_name: {field: float}} for every numeric entry present (fields
        are pre-filled with the resolved config's current value, so in
        practice every row is included — whatever's in the box is what
        generate uses). Blank fields are still omitted, so clearing one is
        equivalent to not overriding it. Only checks "is this a number" —
        physical validity (positive feed_max, etc.) is host.config.validate's
        job, run inside apply_tool_overrides() after this returns. Raises
        ValueError (naming the offending tool/field) on a non-numeric entry.
        """
        result = {}
        for name, fields in self._override_vars.items():
            patch = {}
            for field_name, var in fields.items():
                text = var.get().strip()
                if not text:
                    continue
                try:
                    patch[field_name] = float(text)
                except ValueError:
                    raise ValueError(f"job override {name}.{field_name}: '{text}' is not a number")
            if patch:
                result[name] = patch
        return result

# A simple runner to preview the layout directly
if __name__ == "__main__":
    root = tk.Tk()
    root.title("Layout Preview: PlanView")
    root.geometry("550x300")
    
    view = PlanView(root)
    view.pack(fill="both", expand=True, padx=10, pady=10)
    
    root.mainloop()
