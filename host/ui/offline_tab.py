import tkinter as tk
from tkinter import ttk
import sys
import os

# Allow running this file directly for preview by adding the project root to sys.path
if __name__ == "__main__":
    sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))

from host.ui.config_view import ConfigView
from host.ui.svg_view import SvgView
from host.ui.plan_view import PlanView
from host.ui.offline_session import OfflineSession
from host.ui.draggable_container import ReorderableContainer


class OfflineTab(ttk.Frame):
    """
    The main Offline (Preparation & Planning) UI tab.
    Acts as the Controller, bridging the dumb UI views to the OfflineSession logic.
    """
    def __init__(self, parent, session: OfflineSession, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        self.session = session
        self._build_ui()
        self._bind_logic()

    def _build_ui(self):
        # Create a scrollable canvas
        self.canvas = tk.Canvas(self, highlightthickness=0)
        self.scrollbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.scrollbar.set)
        
        self.scrollbar.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        
        # Inner frame to hold the actual views
        self.scrollable_frame = ttk.Frame(self.canvas)
        self.scrollable_window = self.canvas.create_window((0, 0), window=self.scrollable_frame, anchor="nw")
        
        # Configure scrolling boundaries when inner frame resizes
        self.scrollable_frame.bind(
            "<Configure>",
            lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        )
        
        # Keep inner frame the same width as the canvas
        self.canvas.bind(
            "<Configure>",
            lambda e: self.canvas.itemconfig(self.scrollable_window, width=e.width)
        )
        
        # Add basic mousewheel support for Windows
        self.canvas.bind_all("<MouseWheel>", lambda e: self.canvas.yview_scroll(int(-1*(e.delta/120)), "units"))

        # Create the Reorderable Container
        self.container = ReorderableContainer(self.scrollable_frame)
        self.container.pack(fill="both", expand=True, padx=4, pady=4)

        # Wrap and add each view to the container
        self.config_wrapper = self.container.add_widget("1. Machine Configuration", ConfigView)
        self.config_view = self.config_wrapper.inner_widget
        
        self.svg_wrapper = self.container.add_widget("2. SVG Load & Validation", SvgView)
        self.svg_view = self.svg_wrapper.inner_widget
        
        self.plan_wrapper = self.container.add_widget("3. Plan Management", PlanView)
        self.plan_view = self.plan_wrapper.inner_widget

    def _bind_logic(self):
        # 1. Bind UI events to Session actions
        self.config_view.load_btn.config(command=self.session.load_config)
        
        def _do_load_svg():
            import tkinter.filedialog as fd
            path = fd.askopenfilename(filetypes=[("SVG Files", "*.svg")])
            if path:
                self.session.load_svg(path)
                
        self.svg_view.load_btn.config(command=_do_load_svg)
        
        def _do_load_plan():
            import tkinter.filedialog as fd
            path = fd.askopenfilename(filetypes=[("Plan Files", "*.plan")])
            if path:
                self.session.load_plan(path)
                
        def _do_generate_plan():
            import tkinter.filedialog as fd
            path = fd.asksaveasfilename(
                defaultextension=".plan",
                filetypes=[("Plan Files", "*.plan")],
                initialfile="output.plan"
            )
            if path:
                self.session.generate_plan(path)
                
        self.plan_view.load_btn.config(command=_do_load_plan)
        self.plan_view.generate_btn.config(command=_do_generate_plan)
        
        # 2. Subscribe to Session state changes to update the UI
        self.session.subscribe(self._update_ui)
        self._update_ui() # Initial sync

    def _update_ui(self):
        # --- Update Config View ---
        if self.session.has_valid_config:
            # We can show a little info about the loaded machine
            self.config_view.status_var.set(f"Status: Valid (Loaded '{self.session.config.machine.__class__.__name__}')")
            self.config_view.status_lbl.config(foreground="green")
            self.svg_view.load_btn.config(state="normal")
            
            # --- Update SVG View (when config is valid) ---
            if self.session.has_valid_svg:
                self.svg_view.file_var.set(self.session.svg_file)
                self.svg_view.status_var.set("Status: Valid SVG Loaded")
                self.svg_view.status_lbl.config(foreground="green")
                self.svg_view.limits_var.set(f"Bounding Box: {self.session.svg_bounds}")
                
                # Populate Layers Treeview
                self.svg_view.layers_tree.delete(*self.svg_view.layers_tree.get_children())
                for layer in getattr(self.session, 'svg_layers', []):
                    self.svg_view.layers_tree.insert("", "end", values=(layer["name"], layer["match"]))
            else:
                self.svg_view.file_var.set("(no file loaded)")
                self.svg_view.limits_var.set("Bounding Box: —")
                self.svg_view.layers_tree.delete(*self.svg_view.layers_tree.get_children())
                
                if self.session.svg_error:
                    self.svg_view.status_var.set(f"Status: Error - {self.session.svg_error}")
                    self.svg_view.status_lbl.config(foreground="red")
                else:
                    self.svg_view.status_var.set("Status: Config Valid. Waiting for SVG...")
                    self.svg_view.status_lbl.config(foreground="black")
                    
            # --- Update Plan View (when config is valid) ---
            self.plan_view.load_btn.config(state="normal")
            
            if self.session.has_valid_svg:
                self.plan_view.generate_btn.config(state="normal")
            else:
                self.plan_view.generate_btn.config(state="disabled")
                
            if self.session.has_valid_plan:
                self.plan_view.file_var.set(self.session.plan_file)
                self.plan_view.status_var.set("Status: Valid Plan Loaded")
                self.plan_view.status_lbl.config(foreground="green")
                self.plan_view.header_var.set(f"Version: {self.session.plan_version} | Unique Tools: {self.session.plan_n_tools} | Total Ops: {self.session.plan_n_ops}")
                
                # Populate Manifest Listbox
                self.plan_view.tools_list.delete(0, "end")
                for tool in getattr(self.session, 'plan_tools', []):
                    self.plan_view.tools_list.insert("end", tool)
                    
                # Populate Operations Treeview
                self.plan_view.ops_tree.delete(*self.plan_view.ops_tree.get_children())
                for op in getattr(self.session, 'plan_ops', []):
                    self.plan_view.ops_tree.insert("", "end", values=(op["idx"], op["tool"], op["packets"]))
            else:
                self.plan_view.file_var.set("(no plan loaded)")
                self.plan_view.header_var.set("Version: — | Unique Tools: — | Total Ops: —")
                self.plan_view.tools_list.delete(0, "end")
                self.plan_view.ops_tree.delete(*self.plan_view.ops_tree.get_children())
                
                if self.session.plan_error:
                    self.plan_view.status_var.set(f"Status: Error - {self.session.plan_error}")
                    self.plan_view.status_lbl.config(foreground="red")
                else:
                    if self.session.has_valid_svg:
                        self.plan_view.status_var.set("Status: Config & SVG Valid. Ready to Generate or Load Plan.")
                    else:
                        self.plan_view.status_var.set("Status: Config Valid. Load an SVG to Generate, or Load an existing Plan.")
                    self.plan_view.status_lbl.config(foreground="black")
                    
        else:
            if self.session.config_error:
                self.config_view.status_var.set(f"Status: Error - {self.session.config_error}")
                self.config_view.status_lbl.config(foreground="red")
            else:
                self.config_view.status_var.set("Status: Not Loaded")
                self.config_view.status_lbl.config(foreground="black")
            
            # --- Update SVG View (when config is invalid) ---
            self.svg_view.load_btn.config(state="disabled")
            self.svg_view.file_var.set("(no file loaded)")
            self.svg_view.status_var.set("Status: Waiting for Valid Config")
            self.svg_view.status_lbl.config(foreground="gray")
            self.svg_view.limits_var.set("Bounding Box: —")
            self.svg_view.layers_tree.delete(*self.svg_view.layers_tree.get_children())
            
            # --- Update Plan View (when config is invalid) ---
            self.plan_view.load_btn.config(state="disabled")
            self.plan_view.generate_btn.config(state="disabled")
            self.plan_view.file_var.set("(no plan loaded)")
            self.plan_view.status_var.set("Status: Waiting for Valid Config")
            self.plan_view.status_lbl.config(foreground="gray")
            self.plan_view.header_var.set("Version: — | Unique Tools: — | Total Ops: —")
            self.plan_view.tools_list.delete(0, "end")
            self.plan_view.ops_tree.delete(*self.plan_view.ops_tree.get_children())

# A simple runner to preview the complete offline layout
if __name__ == "__main__":
    root = tk.Tk()
    root.title("Controller Preview: Full Offline Tab")
    root.geometry("650x800")
    
    session = OfflineSession()
    tab = OfflineTab(root, session)
    tab.pack(fill="both", expand=True)
    
    root.mainloop()
