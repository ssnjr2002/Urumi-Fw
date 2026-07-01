import tkinter as tk
from tkinter import ttk

class JobExecutionView(ttk.Frame):
    """
    The View for the Job Execution section of the Online Tab.
    Displays preflight checks and provides job lifecycle controls (Start, Pause, Cancel).
    """
    def __init__(self, parent, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        self._build_ui()

    def _build_ui(self):
        # Configure columns to match our UI Architecture Matrix
        self.columnconfigure(0, weight=1, minsize=150)
        self.columnconfigure(1, weight=1, minsize=150)
        self.columnconfigure(2, weight=2, minsize=450)
        
        # ---------------------------------------------------------
        # Row 0: Preflight
        # ---------------------------------------------------------
        ttk.Label(self, text="Preflight Checks").grid(row=0, column=0, padx=8, pady=8, sticky="nw")
        
        # The Preflight status spans across Status and Control columns.
        # We use a Text widget because preflight logic usually outputs multiple lines.
        self.pf_text = tk.Text(self, width=50, height=5, state="disabled", font=("TkFixedFont", 9), bg="#f0f0f0")
        self.pf_text.grid(row=0, column=1, columnspan=2, padx=8, pady=8, sticky="we")
        
        # ---------------------------------------------------------
        # Row 1: Job State & Controls
        # ---------------------------------------------------------
        ttk.Label(self, text="Job State").grid(row=1, column=0, padx=8, pady=8, sticky="w")
        
        # State and Controls are encapsulated in one frame spanning the remaining columns
        job_frm = ttk.Frame(self)
        job_frm.grid(row=1, column=1, columnspan=2, padx=8, pady=8, sticky="we")
        
        self.job_state_var = tk.StringVar(value="PREFLIGHT")
        self.job_state_lbl = ttk.Label(job_frm, textvariable=self.job_state_var, width=12, font=("TkDefaultFont", 10, "bold"))
        self.job_state_lbl.pack(side="left", padx=(0, 16))
        
        # This button toggles between "Start Job" and "Cancel Job"
        self.start_cancel_btn = ttk.Button(job_frm, text="Start Job")
        self.start_cancel_btn.pack(side="left", padx=4)
        
        # This button toggles between "Pause" and "Resume", greyed out when not running
        self.pause_resume_btn = ttk.Button(job_frm, text="Pause", state="disabled")
        self.pause_resume_btn.pack(side="left", padx=4)
        
    def set_preflight_text(self, text: str):
        """Helper to safely update the read-only preflight text widget."""
        self.pf_text.config(state="normal")
        self.pf_text.delete("1.0", "end")
        self.pf_text.insert("1.0", text)
        self.pf_text.config(state="disabled")

# Simple runner for preview
if __name__ == "__main__":
    root = tk.Tk()
    root.title("Preview: JobExecutionView")
    root.geometry("650x250")
    
    root.columnconfigure(0, weight=1)
    
    view = JobExecutionView(root)
    view.grid(row=0, column=0, sticky="nsew", padx=10, pady=10)
    
    # Mocking some output
    view.set_preflight_text("Ready.\n - Config Matches\n - Plan Loaded\n - Axes Homed")
    
    root.mainloop()
