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
        # ── Inspect Plan — load an existing .plan and see what's in it ──────
        self.inspect_frame = ttk.LabelFrame(self, text="Inspect Plan")
        self.inspect_frame.grid(row=0, column=0, padx=8, pady=8, sticky="nsew")

        self.load_btn = ttk.Button(self.inspect_frame, text="Load .plan...")
        self.load_btn.grid(row=0, column=0, padx=4, pady=4, sticky="w")

        self.file_var = tk.StringVar(value="(no plan loaded)")
        self.file_lbl = ttk.Label(self.inspect_frame, textvariable=self.file_var, font=("TkDefaultFont", 9, "italic"))
        self.file_lbl.grid(row=0, column=1, padx=8, pady=4, sticky="w")

        self.status_var = tk.StringVar(value="Status: Waiting for Plan")
        self.status_lbl = ttk.Label(self.inspect_frame, textvariable=self.status_var, wraplength=400)
        self.status_lbl.grid(row=1, column=0, columnspan=2, padx=4, pady=(0, 4), sticky="w")

        # Details Frame
        self.details_frame = ttk.LabelFrame(self.inspect_frame, text="Plan Details")
        self.details_frame.grid(row=2, column=0, columnspan=2, padx=4, pady=(0, 4), sticky="nsew")

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

        self.details_frame.rowconfigure(2, weight=1)
        self.details_frame.columnconfigure(0, weight=1)
        self.inspect_frame.rowconfigure(2, weight=1)
        self.inspect_frame.columnconfigure(1, weight=1)

        # ── Generate Plan — scalar job params forwarded straight through to
        # plan_job()/subpaths_to_packets() (which already accepts exactly
        # these five as optional overrides; None there falls back to the
        # tool's profile / machine defaults). No per-tool variants — one
        # value applies to the whole job. ──────────────────────────────────
        self.generate_frame = ttk.LabelFrame(self, text="Generate Plan")
        self.generate_frame.grid(row=1, column=0, padx=8, pady=(0, 8), sticky="ew")

        self._param_vars = {}
        params = [
            ("feed_max",    "Feed max (mm/s)"),
            ("a_max",       "Accel max (mm/s^2)"),
            ("jog_feed",    "Jog feed (mm/s)"),
            ("z_feed",      "Z feed (mm/s)"),
            ("lift_height", "Lift height (mm)"),
        ]
        for i, (key, label) in enumerate(params):
            row, col = divmod(i, 2)
            var = tk.StringVar()
            ttk.Label(self.generate_frame, text=label + ":").grid(
                row=row, column=col * 2, padx=(4, 2), pady=2, sticky="w")
            ttk.Entry(self.generate_frame, textvariable=var, width=10).grid(
                row=row, column=col * 2 + 1, padx=(0, 8), pady=2, sticky="w")
            self._param_vars[key] = var

        # lift_height has a real default (0 = draw-through, not a "None ->
        # inherit" sentinel like the other four) — pre-fill it so blank
        # doesn't silently mean something different from what's shown.
        self._param_vars["lift_height"].set("0.0")

        next_row = (len(params) + 1) // 2
        self.generate_btn = ttk.Button(self.generate_frame, text="Generate .plan", state="disabled")
        self.generate_btn.grid(row=next_row, column=0, columnspan=4, padx=4, pady=(8, 4), sticky="w")

        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)

    def get_job_params(self) -> dict:
        """
        {"feed_max": float|None, "a_max": float|None, "jog_feed": float|None,
        "z_feed": float|None, "lift_height": float} straight from the Generate
        Plan entries -- blank means None (subpaths_to_packets' own fallback
        kicks in) for the first four; lift_height blank means 0.0 (its real
        default, not a sentinel). Raises ValueError (naming the field) on a
        non-numeric entry.
        """
        result = {}
        for key, var in self._param_vars.items():
            text = var.get().strip()
            if not text:
                result[key] = 0.0 if key == "lift_height" else None
                continue
            try:
                result[key] = float(text)
            except ValueError:
                raise ValueError(f"'{text}' is not a number for {key}")
        return result

# A simple runner to preview the layout directly
if __name__ == "__main__":
    root = tk.Tk()
    root.title("Layout Preview: PlanView")
    root.geometry("650x500")

    view = PlanView(root)
    view.pack(fill="both", expand=True, padx=10, pady=10)

    root.mainloop()
