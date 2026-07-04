from typing import Optional

from host.ui.observable import Observable

class AppState(Observable):
    """
    Global shared state for the Root App.
    Acts as the bridge between the Offline Setup and Online Execution tabs.
    """
    def __init__(self):
        super().__init__()
        # --- State Data ---
        self._config = None
        self._active_plan_path: Optional[str] = None
        self._is_sim: bool = False

    @property
    def config(self):
        """The currently loaded and validated Machine Config."""
        return self._config
        
    @config.setter
    def config(self, val):
        self._config = val
        self._notify()
        
    @property
    def active_plan_path(self):
        """The absolute path to the currently active .plan file."""
        return self._active_plan_path
        
    @active_plan_path.setter
    def active_plan_path(self, val: Optional[str]):
        self._active_plan_path = val
        self._notify()

    @property
    def plan(self):
        """The currently loaded Plan object."""
        return getattr(self, '_plan', None)
        
    @plan.setter
    def plan(self, val):
        self._plan = val
        self._notify()

    @property
    def is_sim(self) -> bool:
        return self._is_sim

    @is_sim.setter
    def is_sim(self, val: bool):
        self._is_sim = val
        self._notify()
