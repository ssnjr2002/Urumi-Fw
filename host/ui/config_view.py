import tkinter as tk
from tkinter import ttk

class ConfigView(ttk.LabelFrame):
    """
    UI Layout for the Configuration Management section (Offline Mode).
    Contains no business logic, just the visual elements.
    """
    def __init__(self, parent, *args, **kwargs):
        super().__init__(parent, text="1. Configuration Management", *args, **kwargs)
        self._build_ui()

    def _build_ui(self):
        # Action Row
        self.load_btn = ttk.Button(self, text="Load & Validate")
        self.load_btn.grid(row=0, column=0, padx=8, pady=8, sticky="w")
        
        # Using a StringVar so the business logic can easily update it later.
        # wraplength ensures long validation errors wrap to a new line instead of stretching the window.
        self.status_var = tk.StringVar(value="Status: Not Loaded")
        self.status_lbl = ttk.Label(self, textvariable=self.status_var, wraplength=400)
        self.status_lbl.grid(row=0, column=1, padx=8, pady=8, sticky="w")

        self.columnconfigure(1, weight=1)

# A simple runner to preview the layout directly
if __name__ == "__main__":
    root = tk.Tk()
    root.title("Layout Preview: ConfigView")
    root.geometry("550x100")
    
    view = ConfigView(root)
    view.pack(fill="both", expand=True, padx=10, pady=10)
    
    root.mainloop()
