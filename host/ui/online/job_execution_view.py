import tkinter as tk
from tkinter import ttk

class JobExecutionView(ttk.Frame):
    """
    View for the Job Execution component.
    Provides a display for the currently loaded plan, a log console for pre-flight 
    and job progress, and controls for the job lifecycle (Run, Pause, Resume, Cancel).
    """
    def __init__(self, parent, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        self._build_ui()
        
    def _build_ui(self):
        # Frame to hold the dynamic rows (same alignment strategy as AxisNodesView)
        self.inner_frame = ttk.Frame(self)
        self.inner_frame.grid(row=0, column=0, columnspan=5, sticky="ew", padx=8, pady=4)
        
        self.inner_frame.columnconfigure(0, weight=1, minsize=150)
        self.inner_frame.columnconfigure(1, weight=1, minsize=150)
        self.inner_frame.columnconfigure(2, weight=2, minsize=450)
        
        # Row 0: Plan Name
        self.plan_name_var = tk.StringVar(value="(no plan loaded)")
        ttk.Label(self.inner_frame, text="Plan:", font=("TkDefaultFont", 9, "bold")).grid(row=0, column=0, padx=4, pady=2, sticky="w")
        self.plan_name_lbl = ttk.Label(self.inner_frame, textvariable=self.plan_name_var)
        self.plan_name_lbl.grid(row=0, column=1, padx=4, pady=2, sticky="w")
        
        self.load_btn = ttk.Button(self.inner_frame, text="Load...")
        self.load_btn.grid(row=0, column=2, padx=4, pady=2, sticky="w")
        
        # Row 1: Job Logs / Pre-Flight Output
        self.logs_text = tk.Text(self, width=65, height=8, state="disabled", font=("TkFixedFont", 9))
        self.logs_text.grid(row=1, column=0, columnspan=5, padx=8, pady=4, sticky="ew")
        
        # Row 2: Lifecycle Controls
        self.run_btn = ttk.Button(self, text="Run Job", state="disabled")
        self.run_btn.grid(row=2, column=0, padx=(8, 4), pady=(4, 8), sticky="w")
        
        self.pause_btn = ttk.Button(self, text="Pause", state="disabled")
        self.pause_btn.grid(row=2, column=1, padx=4, pady=(4, 8), sticky="w")
        
        self.resume_btn = ttk.Button(self, text="Resume", state="disabled")
        self.resume_btn.grid(row=2, column=2, padx=4, pady=(4, 8), sticky="w")
        
        self.cancel_btn = ttk.Button(self, text="Cancel", state="disabled")
        self.cancel_btn.grid(row=2, column=3, padx=4, pady=(4, 8), sticky="w")
        
        # Configure expanding column for right-alignment of future elements if needed
        self.columnconfigure(4, weight=1)

    def set_logs(self, text: str):
        """Helper to safely replace the contents of the read-only Text widget."""
        current = self.logs_text.get("1.0", "end-1c")
        if current == text:
            return
            
        # Save scroll position
        scroll_pos = self.logs_text.yview()
        
        self.logs_text.config(state="normal")
        self.logs_text.delete("1.0", "end")
        self.logs_text.insert("1.0", text)
        self.logs_text.config(state="disabled")
        
        # Restore scroll position
        self.logs_text.yview_moveto(scroll_pos[0])
        
    def append_log(self, text: str):
        """Helper to safely append to the read-only Text widget."""
        self.logs_text.config(state="normal")
        self.logs_text.insert("end", text + "\n")
        self.logs_text.see("end")
        self.logs_text.config(state="disabled")

# Simple runner for preview
if __name__ == "__main__":
    root = tk.Tk()
    root.title("Preview: Job Execution View")
    root.geometry("600x300")
    view = JobExecutionView(root)
    view.pack(fill="both", expand=True, padx=10, pady=10)
    
    view.plan_name_var.set("my_project.plan")
    view.set_logs("PRE-FLIGHT PASS\n  [OK] pico alive\n  [OK] tool 'vbit' mounted\n  [OK] axis x node 1 present")
    view.run_btn.config(state="normal")
    
    root.mainloop()
