from typing import Callable, List, Optional

class AppState:
    """
    Global shared state for the Root App.
    Acts as the bridge between the Offline Setup and Online Execution tabs.
    """
    def __init__(self):
        # --- State Data ---
        self._config = None
        self._active_plan_path: Optional[str] = None
        self._is_sim: bool = False
        
        self._subscribers: List[Callable] = []
        
    def subscribe(self, callback: Callable):
        """Register a callback to be fired whenever the global state changes."""
        if callback not in self._subscribers:
            self._subscribers.append(callback)
            
    def _notify(self):
        for cb in self._subscribers:
            cb()
            
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
