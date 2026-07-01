from typing import Callable, List, Optional

class AppState:
    """
    Global shared state for the Root App.
    Acts as the bridge between the Offline Setup and Online Execution tabs.
    """
    def __init__(self):
        self._config = None
        self._active_plan_path: Optional[str] = None
        
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
    def active_plan_path(self, val):
        self._active_plan_path = val
        self._notify()
