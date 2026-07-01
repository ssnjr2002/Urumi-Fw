import tkinter as tk
from tkinter import ttk

class CollapsibleWrapper(ttk.Frame):
    """
    Wraps a given widget class in a foldable frame with a drag handle.
    """
    def __init__(self, parent, text, widget_class, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        self.text = text
        self.is_collapsed = False
        
        # Header
        self.header = ttk.Frame(self)
        self.header.pack(fill="x", pady=(4, 0))
        
        self.toggle_btn = ttk.Button(self.header, text="▼", width=3, command=self.toggle)
        self.toggle_btn.pack(side="left", padx=(0, 4))
        
        # Drag handle
        self.title_lbl = ttk.Label(self.header, text=text, font=("TkDefaultFont", 10, "bold"), cursor="fleur")
        self.title_lbl.pack(side="left", fill="x", expand=True)
        
        # Content frame (with a slight border for visual grouping)
        self.content_frame = ttk.Frame(self, relief="groove", borderwidth=2)
        self.content_frame.pack(fill="both", expand=True, padx=4, pady=4)
        
        # Instantiate the actual view widget inside the content frame
        self.inner_widget = widget_class(self.content_frame)
        self.inner_widget.pack(fill="both", expand=True, padx=4, pady=4)
        
    def toggle(self):
        self.is_collapsed = not self.is_collapsed
        if self.is_collapsed:
            self.content_frame.pack_forget()
            self.toggle_btn.config(text="▶")
        else:
            self.content_frame.pack(fill="both", expand=True, padx=4, pady=4)
            self.toggle_btn.config(text="▼")
            
class ReorderableContainer(ttk.Frame):
    """
    Manages a list of CollapsibleWrappers and allows dragging to reorder them.
    """
    def __init__(self, parent, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        self.wrappers = []
        self._drag_data = {"y": 0, "wrapper": None}

    def add_widget(self, text, widget_class) -> CollapsibleWrapper:
        wrapper = CollapsibleWrapper(self, text, widget_class)
        wrapper.pack(fill="x", padx=4, pady=4)
        self.wrappers.append(wrapper)
        
        # Bind drag events to the title label
        wrapper.title_lbl.bind("<ButtonPress-1>", lambda e, w=wrapper: self.on_drag_start(e, w))
        wrapper.title_lbl.bind("<B1-Motion>", self.on_drag_motion)
        wrapper.title_lbl.bind("<ButtonRelease-1>", self.on_drag_stop)
        
        return wrapper

    def on_drag_start(self, event, wrapper):
        self._drag_data["wrapper"] = wrapper
        self._drag_data["y"] = event.y_root

    def on_drag_motion(self, event):
        if not self._drag_data["wrapper"]:
            return
            
        delta_y = event.y_root - self._drag_data["y"]
        wrapper = self._drag_data["wrapper"]
        idx = self.wrappers.index(wrapper)
        
        # Swap up if dragged past a threshold
        if delta_y < -40 and idx > 0:
            self.wrappers[idx], self.wrappers[idx - 1] = self.wrappers[idx - 1], self.wrappers[idx]
            self._repack()
            self._drag_data["y"] = event.y_root
            
        # Swap down
        elif delta_y > 40 and idx < len(self.wrappers) - 1:
            self.wrappers[idx], self.wrappers[idx + 1] = self.wrappers[idx + 1], self.wrappers[idx]
            self._repack()
            self._drag_data["y"] = event.y_root

    def on_drag_stop(self, event):
        self._drag_data["wrapper"] = None

    def _repack(self):
        for w in self.wrappers:
            w.pack_forget()
        for w in self.wrappers:
            # Re-pack in the new order
            w.pack(fill="x", padx=4, pady=4)
