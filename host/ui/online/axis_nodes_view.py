import tkinter as tk
from tkinter import ttk

from host.ui.tooltip import ToolTip

class AxisNodesView(ttk.Frame):
    """
    The View for the Axis Nodes section of the Online Tab.
    Dynamically generates rows for each axis (X, Y, Z, A, etc.) defined in the config.
    """
    def __init__(self, parent, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        
        # Dictionaries to hold references to dynamic widgets/vars keyed by axis letter
        self.axis_name_labels = {}
        self.axis_info_labels = {}
        self.axis_tooltips = {}
        self.axis_node_ids = {}
        self.homed_vars = {}
        self.pos_vars = {}
        self.set_origin_btns = {}
        
        # Jogging specific vars/widgets
        self.jog_dist_vars = {}
        self.jog_rate_vars = {}
        self.jog_dec_btns = {}
        self.jog_inc_btns = {}
        
        # Debug: route jog packets to jog_output.bin instead of the sim/COM
        # link — sits with the jog controls it affects rather than in the
        # global machine-state panel.
        debug_frm = ttk.Frame(self)
        debug_frm.pack(side="bottom", fill="x", padx=8, pady=(0, 8))
        self.dump_jog_var = tk.BooleanVar(value=False)
        self.dump_jog_chk = ttk.Checkbutton(debug_frm, text="Also copy jog packets to jog_output.bin",
                                             variable=self.dump_jog_var)
        self.dump_jog_chk.pack(side="left")

        # Frame to hold the dynamic rows
        self.inner_frame = ttk.Frame(self)
        self.inner_frame.pack(fill="both", expand=True, padx=8, pady=8)
        
        # Configure columns to match our UI Architecture Matrix
        self.inner_frame.columnconfigure(0, weight=1, minsize=150)
        self.inner_frame.columnconfigure(1, weight=1, minsize=150)
        self.inner_frame.columnconfigure(2, weight=2, minsize=450) # Wider for jog controls
        
    def populate(self, axes):
        """
        Populates both Setup & Info and Jogging rows for each axis.
        Expects a list of tuples: (letter, axis_config).
        """
        # Clear existing rows if any
        for widget in self.inner_frame.winfo_children():
            widget.destroy()
            
        self.axis_name_labels.clear()
        self.axis_info_labels.clear()
        self.axis_tooltips.clear()
        self.axis_node_ids.clear()
        self.homed_vars.clear()
        self.pos_vars.clear()
        self.set_origin_btns.clear()
        self.jog_dist_vars.clear()
        self.jog_rate_vars.clear()
        self.jog_dec_btns.clear()
        self.jog_inc_btns.clear()
        
        if not axes:
            ttk.Label(self.inner_frame, text="No axes configured.", font=("TkDefaultFont", 9, "italic")).grid(row=0, column=0, padx=4, pady=4, sticky="w")
            return
            
        for idx, (letter, ax_config) in enumerate(axes):
            # We use 3 rows per axis: Setup, Jogging, and a Separator
            row_setup = idx * 3
            row_jog = idx * 3 + 1
            row_sep = idx * 3 + 2
            
            unit = "deg" if getattr(ax_config, "rotary", False) else "mm"
            
            # =========================================================
            # ROW 1: SETUP & INFO
            # =========================================================
            
            # Column 0: Definition
            name_frm = ttk.Frame(self.inner_frame)
            name_frm.grid(row=row_setup, column=0, padx=4, pady=(8, 2), sticky="nw")
            
            lbl = ttk.Label(name_frm, text=f"Axis {letter.upper()} (Node {ax_config.node.node_id})", font=("TkDefaultFont", 9, "bold"))
            lbl.pack(side="left")
            self.axis_name_labels[letter] = lbl
            
            info_lbl = ttk.Label(name_frm, text="", foreground="#0078D7", cursor="hand2")
            info_lbl.pack(side="left", padx=(4, 0))
            self.axis_info_labels[letter] = info_lbl
            
            self.axis_tooltips[letter] = ToolTip(info_lbl)
            self.axis_node_ids[letter] = ax_config.node.node_id
            # ---------------------------------------------------------
            # Column 1: State/Status (Homed)
            # ---------------------------------------------------------
            homed_var = tk.StringVar(value="Homed: —")
            self.homed_vars[letter] = homed_var
            ttk.Label(self.inner_frame, textvariable=homed_var).grid(row=row_setup, column=1, padx=4, pady=(8, 2), sticky="nw")
            
            # Column 2: Control (Set Origin)
            setup_ctrl_frm = ttk.Frame(self.inner_frame)
            setup_ctrl_frm.grid(row=row_setup, column=2, padx=4, pady=(8, 2), sticky="nw")
            
            origin_btn = ttk.Button(setup_ctrl_frm, text=f"Set Origin ({letter.upper()})")
            origin_btn.pack(side="left", padx=2)
            self.set_origin_btns[letter] = origin_btn

            # =========================================================
            # ROW 2: JOGGING & POSITION
            # =========================================================
            
            # Column 0: Left blank to visually tuck under the axis label
            
            # Column 1: State/Status (Position)
            pos_var = tk.StringVar(value=f"Pos: — {unit}")
            self.pos_vars[letter] = pos_var
            ttk.Label(self.inner_frame, textvariable=pos_var).grid(row=row_jog, column=1, padx=4, pady=(2, 8), sticky="nw")
            
            # Column 2: Control (Jogging)
            jog_frm = ttk.Frame(self.inner_frame)
            jog_frm.grid(row=row_jog, column=2, padx=4, pady=(2, 8), sticky="nw")
            
            # Decrement (-)
            dec_btn = ttk.Button(jog_frm, text="-", width=3)
            dec_btn.pack(side="left", padx=2)
            self.jog_dec_btns[letter] = dec_btn
            
            # Distance Value
            dist_var = tk.DoubleVar(value=90.0 if getattr(ax_config, "rotary", False) else 10.0)
            self.jog_dist_vars[letter] = dist_var
            ttk.Entry(jog_frm, textvariable=dist_var, width=6).pack(side="left", padx=2)
            ttk.Label(jog_frm, text=unit).pack(side="left", padx=(0, 6))
            
            # Increment (+)
            inc_btn = ttk.Button(jog_frm, text="+", width=3)
            inc_btn.pack(side="left", padx=2)
            self.jog_inc_btns[letter] = inc_btn
            
            # Rate Value
            ttk.Label(jog_frm, text="Rate:").pack(side="left", padx=(12, 2))
            rate_var = tk.DoubleVar(value=ax_config.max_rate if getattr(ax_config, "max_rate", None) else 60.0)
            self.jog_rate_vars[letter] = rate_var
            ttk.Entry(jog_frm, textvariable=rate_var, width=6).pack(side="left", padx=2)
            ttk.Label(jog_frm, text=f"{unit}/s").pack(side="left", padx=(0, 6))

            # =========================================================
            # ROW 3: VISUAL SEPARATOR
            # =========================================================
            if idx < len(axes) - 1:
                ttk.Separator(self.inner_frame, orient="horizontal").grid(row=row_sep, column=0, columnspan=3, sticky="ew", pady=6)

# Simple runner for preview
if __name__ == "__main__":
    from dataclasses import dataclass
    
    @dataclass
    class MockNode:
        node_id: int
        
    @dataclass
    class MockAxis:
        node: MockNode
        rotary: bool = False
        max_rate: float = 150.0
        
    root = tk.Tk()
    root.title("Preview: AxisNodesView (Full)")
    root.geometry("800x350")
    
    view = AxisNodesView(root)
    view.pack(fill="both", expand=True, padx=10, pady=10)
    
    # Mock data to preview the dynamic generation
    mock_axes = [
        ("x", MockAxis(node=MockNode(1), max_rate=250.0)),
        ("y", MockAxis(node=MockNode(2), max_rate=250.0)),
        ("a", MockAxis(node=MockNode(4), rotary=True, max_rate=60.0))
    ]
    view.populate(mock_axes)
    
    root.mainloop()
