"""
host — Phase 1 host application package.

Subpackages:
  protocol/    the one component that owns the Pico USB link and speaks the
               frozen wire contract (see docs/wire_protocol.md).
  production/  offline, bake-time: SVG -> Plan -> .plan file (parse,
               normalise, repair, orchestrate, planner, plan_io, validators).
  execution/   online, run-time: stream a Plan to the Pico (job_runner,
               preflight).
  diagnostics/ comms simulators and bring-up checks.
Plus gui.py (operator frontend).

Pipeline bridge
───────────────
pipeline/ is a real package (pipeline/stages/, pipeline/data/) living beside
host/ at the repo root. Production code imports it directly, e.g.
`from pipeline.config import default`. No sys.path manipulation is
needed here — the repo root just has to be importable, same as for `host`
itself. Host-internal code uses absolute package imports throughout
(`from host.protocol.packets import ...`).
"""
