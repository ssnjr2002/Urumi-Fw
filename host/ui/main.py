import tkinter as tk
from tkinter import ttk
import sys
import os

# Ensure the root directory is on sys.path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))

from host.ui.app_state import AppState
from host.ui.offline_session import OfflineSession
from host.ui.offline_tab import OfflineTab
from host.ui.online_tab import OnlineTab

class App(tk.Tk):
    """
    The Root Application.
    Manages the top-level window, the global AppState, and the Tab navigation.
    """
    def __init__(self):
        super().__init__()
        self.title("Fabrication Machine Controller")
        self.geometry("900x800")
        
        # 1. Initialize the global shared state
        self.app_state = AppState()
        self.app_state.subscribe(self._on_app_state_changed)
        
        # 2. Create the main Notebook (Tabs container)
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill="both", expand=True)
        
        # 3. Initialize Offline Tab (Preparation)
        self.offline_session = OfflineSession(self.app_state)
        self.offline_tab = OfflineTab(self.notebook, self.offline_session)
        self.notebook.add(self.offline_tab, text="Offline Setup")
        
        # 4. Initialize Online Tab (Execution)
        # TODO: Replace with the real OnlineSession once it's implemented.
        class DummyOnlineSession:
            def __init__(self, app_state):
                self.app_state = app_state
                
        self.online_session = DummyOnlineSession(self.app_state)
        self.online_tab = OnlineTab(self.notebook, self.online_session)
        
        # Start with the Online tab disabled (gated) until a config is loaded
        self.notebook.add(self.online_tab, text="Online Execution (Locked)", state="disabled")
        
    def _on_app_state_changed(self):
        """Called whenever the global AppState changes."""
        # Check if the gate should be unlocked
        if self.app_state.config is not None:
            self.notebook.tab(self.online_tab, text="Online Execution", state="normal")
        else:
            self.notebook.tab(self.online_tab, text="Online Execution (Locked)", state="disabled")
            
        # Switch to Online tab automatically if a plan is loaded and we are not already on it
        # (Optional UX sugar: when user hits Generate Plan, we could jump them to the execution view)

if __name__ == "__main__":
    app = App()
    app.mainloop()
