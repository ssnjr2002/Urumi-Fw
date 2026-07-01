import tkinter as tk
from tkinter import ttk

class SvgView(ttk.LabelFrame):
    """
    UI Layout for the SVG Management section (Offline Mode).
    Contains no business logic, just the visual elements.
    """
    def __init__(self, parent, *args, **kwargs):
        super().__init__(parent, text="2. SVG Management", *args, **kwargs)
        self._build_ui()

    def _build_ui(self):
        # Row 0: Load Action
        self.load_btn = ttk.Button(self, text="Load SVG...")
        self.load_btn.grid(row=0, column=0, padx=8, pady=8, sticky="w")
        
        self.file_var = tk.StringVar(value="(no file loaded)")
        self.file_lbl = ttk.Label(self, textvariable=self.file_var, font=("TkDefaultFont", 9, "italic"))
        self.file_lbl.grid(row=0, column=1, padx=8, pady=8, sticky="w")

        # Row 1: Global Status (e.g., Validation results, out of bounds warnings)
        self.status_var = tk.StringVar(value="Status: Waiting for Config & File")
        self.status_lbl = ttk.Label(self, textvariable=self.status_var, wraplength=400)
        self.status_lbl.grid(row=1, column=0, columnspan=2, padx=8, pady=(0, 8), sticky="w")

        # Row 2: Details Frame (Bounding Box & Layers)
        self.details_frame = ttk.LabelFrame(self, text="Inspection & Soft Limits")
        self.details_frame.grid(row=2, column=0, columnspan=2, padx=8, pady=(0, 8), sticky="nsew")
        
        # Bounding box info
        self.limits_var = tk.StringVar(value="Bounding Box: —")
        self.limits_lbl = ttk.Label(self.details_frame, textvariable=self.limits_var)
        self.limits_lbl.grid(row=0, column=0, padx=4, pady=4, sticky="w")

        # Treeview to display SVG layers and whether they match a valid tool in the config
        columns = ("layer", "tool_match")
        self.layers_tree = ttk.Treeview(self.details_frame, columns=columns, show="headings", height=4)
        self.layers_tree.heading("layer", text="SVG Layer")
        self.layers_tree.heading("tool_match", text="Config Tool Match")
        self.layers_tree.column("layer", width=150)
        self.layers_tree.column("tool_match", width=150)
        self.layers_tree.grid(row=1, column=0, padx=4, pady=4, sticky="nsew")

        # Scrollbar for the layers tree
        self.scrollbar = ttk.Scrollbar(self.details_frame, orient="vertical", command=self.layers_tree.yview)
        self.scrollbar.grid(row=1, column=1, sticky="ns")
        self.layers_tree.configure(yscrollcommand=self.scrollbar.set)

        # Configure weights for resizing
        self.details_frame.rowconfigure(1, weight=1)
        self.details_frame.columnconfigure(0, weight=1)
        
        self.rowconfigure(2, weight=1)
        self.columnconfigure(1, weight=1)

# A simple runner to preview the layout directly
if __name__ == "__main__":
    root = tk.Tk()
    root.title("Layout Preview: SvgView")
    root.geometry("550x300")
    
    view = SvgView(root)
    view.pack(fill="both", expand=True, padx=10, pady=10)
    
    root.mainloop()
