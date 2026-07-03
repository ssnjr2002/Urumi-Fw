import tkinter as tk
from tkinter import ttk

class ConfigView(ttk.Frame):
    """
    The View for the Machine Configuration step of the Offline Tab.
    """
    def __init__(self, parent, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        self._build_ui()

    def _build_ui(self):
        # Using a StringVar so the business logic can easily update it later.
        # wraplength ensures long validation errors wrap to a new line instead of stretching the window.
        self.status_var = tk.StringVar(value="Status: Not Loaded. (A valid config is required to unlock Online Execution)")
        self.status_lbl = ttk.Label(self, textvariable=self.status_var, wraplength=560)
        self.status_lbl.grid(row=0, column=0, columnspan=4, padx=8, pady=8, sticky="w")

        # Mode selector — which "no path" default a Load action resolves to
        self.mode_var = tk.StringVar(value="production")
        mode_frame = ttk.Frame(self)
        mode_frame.grid(row=1, column=0, padx=8, pady=4, sticky="w")
        ttk.Radiobutton(mode_frame, text="Production", variable=self.mode_var, value="production").pack(side="left", padx=(0, 12))
        ttk.Radiobutton(mode_frame, text="Simulator", variable=self.mode_var, value="sim").pack(side="left")

        # Action Row
        self.load_defaults_btn = ttk.Button(self, text="Load Defaults")
        self.load_defaults_btn.grid(row=1, column=2, padx=4, pady=4, sticky="e")

        self.load_file_btn = ttk.Button(self, text="Load from File...")
        self.load_file_btn.grid(row=1, column=3, padx=8, pady=4, sticky="e")

        # ── Resolved-config summary panel ───────────────────────────────────
        # Optional/removable: this block (label + Text widget + set_summary())
        # plus its one call site in offline/tab.py's _update_ui
        # (config_view.set_summary(...)) can be deleted together without
        # touching load logic or any other widget here.
        ttk.Label(self, text="Resolved Config:").grid(row=2, column=0, padx=8, pady=(8, 0), sticky="w")
        self.summary_text = tk.Text(self, width=70, height=9, state="disabled", font=("TkFixedFont", 9))
        self.summary_text.grid(row=3, column=0, columnspan=4, padx=8, pady=(0, 4), sticky="ew")
        # ── end summary panel ───────────────────────────────────────────────

        # Validation errors — hidden (grid_remove) whenever there are none
        self.errors_label = ttk.Label(self, text="Validation Errors:", foreground="#a00")
        self.errors_text = tk.Text(self, width=70, height=5, state="disabled",
                                    font=("TkFixedFont", 9), foreground="#a00")
        self.errors_label.grid(row=4, column=0, padx=8, pady=(4, 0), sticky="w")
        self.errors_text.grid(row=5, column=0, columnspan=4, padx=8, pady=(0, 8), sticky="ew")
        self.errors_label.grid_remove()
        self.errors_text.grid_remove()

        self.columnconfigure(1, weight=1)

    def set_summary(self, text: str):
        """Helper to safely replace the read-only summary Text widget's contents."""
        self._set_text(self.summary_text, text)

    def set_errors(self, errors: list):
        """Shows the validation-errors panel with one bullet per error, or hides it if errors is empty."""
        if not errors:
            self.errors_label.grid_remove()
            self.errors_text.grid_remove()
            return
        self.errors_label.grid()
        self.errors_text.grid()
        self._set_text(self.errors_text, "\n".join(f"- {e}" for e in errors))

    @staticmethod
    def _set_text(widget: tk.Text, text: str):
        widget.config(state="normal")
        widget.delete("1.0", "end")
        widget.insert("1.0", text)
        widget.config(state="disabled")

# A simple runner to preview the layout directly
if __name__ == "__main__":
    root = tk.Tk()
    root.title("Layout Preview: ConfigView")
    root.geometry("620x420")

    view = ConfigView(root)
    view.pack(fill="both", expand=True, padx=10, pady=10)

    view.set_summary("f_cpu: 150000000 Hz    jog_feed: 80.0 mm/s    z_feed: 20.0 mm/s\n\nAxes:\n  X: node=1  steps/unit=160.0  max_rate=80.0  accel=1000.0  invert=True")
    view.set_errors(["machine.x: steps_per_unit must be positive (got 0)", "duplicate node_id 3: used by both machine.z and machine.a"])

    root.mainloop()
