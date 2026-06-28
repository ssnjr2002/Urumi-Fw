"""
host — Phase 1 host application package.

Subpackages:
  protocol/    the one component that owns the Pico USB link and speaks the
               frozen wire contract (see docs/wire_protocol.md).
  production/  offline pipeline orchestration (SVG -> packets, validators).
  diagnostics/ comms simulators and bring-up checks.
Plus gui.py (operator frontend) and cli.py (thin scripting frontend).

Pipeline bridge
───────────────
The pipeline stages live in pipeline/stages and are NOT packaged — they import
each other (and config) by flat module name. Production code under host/ reuses
them, so importing this package puts pipeline/stages on sys.path once, here, and
the production modules keep their flat `from config import ...` imports working
without packaging the pipeline. Host-internal code uses absolute package imports
(`from host.protocol.packets import ...`).
"""

import os as _os
import sys as _sys

_PIPELINE_STAGES = _os.path.normpath(
    _os.path.join(_os.path.dirname(__file__), "..", "pipeline", "stages"))
if _PIPELINE_STAGES not in _sys.path:
    _sys.path.insert(0, _PIPELINE_STAGES)
