from typing import Callable, List


class Observable:
    """Shared subscribe/_notify plumbing for the UI's state-holder classes."""

    def __init__(self):
        self._subscribers: List[Callable] = []

    def subscribe(self, callback: Callable):
        """Register a callback to be fired whenever this object's state changes."""
        if callback not in self._subscribers:
            self._subscribers.append(callback)

    def _notify(self):
        for cb in self._subscribers:
            cb()
