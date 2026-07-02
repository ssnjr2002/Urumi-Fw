import tkinter as tk
from tkinter import ttk

class MasterView(ttk.Frame):
    """
    The View for the Master section of the Online Tab.
    Displays global machine state, connection, and top-level controls.
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
        # Row 0: COM Port
        # ---------------------------------------------------------
        ttk.Label(self, text="COM Port").grid(row=0, column=0, padx=8, pady=8, sticky="w")
        
        self.port_var = tk.StringVar(value="Simulator")
        self.port_combo = ttk.Combobox(self, textvariable=self.port_var, values=["Simulator"], width=15)
        self.port_combo.grid(row=0, column=1, padx=8, pady=8, sticky="w")
        
        self.connect_btn = ttk.Button(self, text="Connect")
        self.connect_btn.grid(row=0, column=2, padx=8, pady=8, sticky="w")

        # ---------------------------------------------------------
        # Row 1: State & Reason
        # ---------------------------------------------------------
        ttk.Label(self, text="State & Reason").grid(row=1, column=0, padx=8, pady=4, sticky="w")
        
        self.state_var = tk.StringVar(value="DISCONNECTED")
        self.state_lbl = ttk.Label(self, textvariable=self.state_var, font=("TkDefaultFont", 10, "bold"))
        self.state_lbl.grid(row=1, column=1, padx=8, pady=4, sticky="w")
        
        state_ctrl_frm = ttk.Frame(self)
        state_ctrl_frm.grid(row=1, column=2, padx=8, pady=4, sticky="w")
        
        self.estop_btn = tk.Button(state_ctrl_frm, text="ESTOP", bg="#cc2222", fg="white", font=("TkDefaultFont", 9, "bold"))
        self.estop_btn.pack(side="left", padx=2)
        
        self.unalarm_btn = ttk.Button(state_ctrl_frm, text="Unalarm")
        self.unalarm_btn.pack(side="left", padx=2)

        # ---------------------------------------------------------
        # Row 2: Axes Enabled
        # ---------------------------------------------------------
        ttk.Label(self, text="Axes Enabled").grid(row=2, column=0, padx=8, pady=4, sticky="w")
        
        self.enabled_status_var = tk.StringVar(value="—")
        ttk.Label(self, textvariable=self.enabled_status_var).grid(row=2, column=1, padx=8, pady=4, sticky="w")
        
        enable_frm = ttk.Frame(self)
        enable_frm.grid(row=2, column=2, padx=8, pady=4, sticky="w")
        
        self.enable_all_btn = ttk.Button(enable_frm, text="Enable All")
        self.enable_all_btn.pack(side="left", padx=2)
        
        self.disable_all_btn = ttk.Button(enable_frm, text="Disable All")
        self.disable_all_btn.pack(side="left", padx=2)

        # ---------------------------------------------------------
        # Row 3: Axes Homed
        # ---------------------------------------------------------
        ttk.Label(self, text="Axes Homed").grid(row=3, column=0, padx=8, pady=4, sticky="w")
        
        self.homed_status_var = tk.StringVar(value="—")
        ttk.Label(self, textvariable=self.homed_status_var).grid(row=3, column=1, padx=8, pady=4, sticky="w")
        
        homed_ctrl_frm = ttk.Frame(self)
        homed_ctrl_frm.grid(row=3, column=2, padx=8, pady=4, sticky="w")
        
        self.set_origin_btn = ttk.Button(homed_ctrl_frm, text="Set Origin (All)")
        self.set_origin_btn.pack(side="left", padx=2)

        # ---------------------------------------------------------
        # Row 4: Master Global Command
        # ---------------------------------------------------------
        ttk.Label(self, text="Last Command Status").grid(row=4, column=0, padx=8, pady=8, sticky="w")
        
        self.cmd_status_var = tk.StringVar(value="—")
        ttk.Label(self, textvariable=self.cmd_status_var).grid(row=4, column=1, padx=8, pady=8, sticky="w")

# Simple runner for preview
if __name__ == "__main__":
    root = tk.Tk()
    root.title("Preview: MasterView")
    root.geometry("650x250")
    
    # Configure grid weights for the root window so it expands properly
    root.columnconfigure(0, weight=1)
    root.rowconfigure(0, weight=1)
    
    mv = MasterView(root)
    mv.grid(row=0, column=0, sticky="nsew", padx=10, pady=10)
    
    root.mainloop()
