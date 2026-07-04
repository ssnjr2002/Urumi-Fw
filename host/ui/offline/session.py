import os
import importlib
from typing import Callable, List, Optional

from host.ui.app_state import AppState

class OfflineSession:
    """
    Business logic manager for the Offline Preparation & Planning phase.
    Acts as the single source of truth for state. UI components bind to this.
    """
    def __init__(self, app_state: AppState):
        self._app_state = app_state
        self._callbacks: List[Callable] = []

        # We subscribe to app_state so our UI updates if the global state changes
        self._app_state.subscribe(self._notify)

        # --- Configuration State ---
        self.config_error: Optional[str] = None
        self.config_errors: list = []  # structured validate() findings, for the UI error panel

        # --- SVG State ---
        self.svg_file: Optional[str] = None
        self.svg_path: Optional[str] = None
        self.svg_error: Optional[str] = None
        self.svg_bounds: Optional[str] = None
        self.svg_layers: list = []

        # --- Plan State ---
        self.plan = None
        self.plan_file: Optional[str] = None
        self.plan_error: Optional[str] = None
        self.plan_version: Optional[int] = None
        self.plan_n_tools: Optional[int] = None
        self.plan_n_ops: Optional[int] = None
        self.plan_tools: list = []
        self.plan_ops: list = []

    def subscribe(self, callback: Callable):
        """UI components register here to be notified of state changes."""
        self._callbacks.append(callback)

    def _notify(self):
        """Fire all callbacks when state changes."""
        for cb in self._callbacks:
            cb()

    @property
    def config(self):
        return self._app_state.config

    @config.setter
    def config(self, val):
        self._app_state.config = val

    # ---------------------------------------------------------
    # 1. Configuration Management
    # ---------------------------------------------------------
    def load_config(self, path: Optional[str] = None, is_sim: bool = False):
        """
        Loads and validates the machine configuration for the given mode.

        path=None resolves to the mode's default: pipeline.config.default()
        for production, host/diagnostics/sim_machine.toml (the canonical sim
        fixture) for simulator. A path always wins when given, in either
        mode -- "Load from File" opens a picker for an arbitrary TOML
        regardless of which radio is selected.

        Always sets self.config_errors (list[str], empty on success) in
        addition to self.config/self.config_error, so the UI's
        validation-error panel has a structured list to render instead of
        parsing config_error's text.
        """
        try:
            import host.config as host_config

            if path:
                loaded_config, errors = host_config.load_with_errors(path)
            elif is_sim:
                sim_toml = os.path.join(
                    os.path.dirname(os.path.dirname(os.path.dirname(__file__))),
                    "diagnostics", "sim_machine.toml",
                )
                loaded_config, errors = host_config.load_with_errors(sim_toml)
            else:
                # Imported here (not at module level) to allow reloading if
                # the user edits pipeline/config.py while the app is open.
                import pipeline.config as config
                importlib.reload(config)
                loaded_config = config.default()
                errors = host_config.validate(loaded_config)
                if errors:
                    loaded_config = None

            self.config_errors = errors
            if loaded_config is not None:
                self._app_state.is_sim = is_sim
                self.config = loaded_config
                self.config_error = None
            else:
                self.config = None
                self.config_error = "; ".join(errors) if errors else "Unknown config error"

        except Exception as e:
            self.config = None
            self.config_errors = [str(e)]
            self.config_error = str(e)

        self._notify()

    # config_summary_text() is self-contained: it only reads self.config and
    # returns a string. Its one call site is offline/tab.py's _update_ui
    # (self.config_view.set_summary(...)) -- delete both together to remove
    # the resolved-config summary panel without touching anything else.
    def config_summary_text(self) -> str:
        """Formats self.config for the read-only summary panel."""
        if self.config is None:
            return ""
        cfg = self.config
        m = cfg.machine
        lines = [
            f"f_cpu: {m.f_cpu} Hz    jog_feed: {m.jog_feed} mm/s    z_feed: {m.z_feed} mm/s",
            "",
            "Axes:",
        ]
        for ltr, axis in m.present_axes():
            lines.append(
                f"  {ltr.upper()}: node={axis.node.node_id}  steps/unit={axis.steps_per_unit}  "
                f"max_rate={axis.max_rate}  accel={axis.accel}  invert={axis.invert}"
            )

        profile = m.head.profile
        lines.append("")
        lines.append(f"Mounted tool: {profile.name}  (feed_max={profile.feed_max}  accel={profile.accel})")

        lines.append("")
        lines.append("Tool presets:")
        for name in sorted(cfg.tool_profiles):
            p = cfg.tool_profiles[name]
            lines.append(
                f"  {name}: feed_max={p.feed_max}  accel={p.accel}  "
                f"jog_feed={p.jog_feed}  z_feed={p.z_feed}"
            )

        if m.peripherals:
            lines.append("")
            lines.append("Peripherals:")
            for p in m.peripherals:
                lines.append(f"  node={p.node_id}  role={p.role}  present={p.present}")

        return "\n".join(lines)

    # ---------------------------------------------------------
    # 2. SVG Management
    # ---------------------------------------------------------
    def load_svg(self, path: str):
        """Loads an SVG, extracts layers, and performs basic tool matching."""
        if not self.has_valid_config:
            self.svg_error = "Config must be valid before loading SVG."
            self._notify()
            return

        try:
            from host.production.normalise import load_svg_mm_layers
            from pipeline.config import TOOL_PROFILES_BY_TYPE

            layers_mm, viewport = load_svg_mm_layers(path)

            self.svg_file = os.path.basename(path)
            self.svg_path = path

            # Simple bounding box string format (skipping strict limits check for now as requested)
            w_mm, h_mm = viewport[4], viewport[5]
            self.svg_bounds = f"{w_mm:.1f} x {h_mm:.1f} mm"

            # Attempt to match layer names to known tools in the config
            valid_tool_names = [t.name.lower() for t in TOOL_PROFILES_BY_TYPE.values()]

            self.svg_layers = []
            for layer_name in layers_mm.keys():
                match_status = "Unknown Tool"
                display_name = layer_name

                if layer_name == "":
                    display_name = "(default layer)"
                    match_status = "No Tool Specified"
                elif layer_name.lower() in valid_tool_names:
                    match_status = "Valid Match"

                self.svg_layers.append({"name": display_name, "match": match_status})

            self.svg_error = None

        except Exception as e:
            self.svg_file = None
            self.svg_error = f"Failed to load SVG: {e}"
            self.svg_layers = []

        self._notify()

    # ---------------------------------------------------------
    # 3. Plan Management
    # ---------------------------------------------------------
    def load_plan(self, path: str):
        if not self.has_valid_config:
            self.plan_error = "Config must be valid to verify a plan against it."
            self._notify()
            return

        try:
            from host.production.plan_io import load_plan as plan_io_load

            # This handles the magic/version check and throws ValueErrors on unrecognised tools
            plan_obj = plan_io_load(path, self.config.machine)

            self.plan_file = os.path.basename(path)
            self.plan = plan_obj
            self.plan_error = None
            self._app_state.plan = plan_obj
            self._app_state.active_plan_path = path

            # Extract header info
            self.plan_version = 1

            # Extract tools (Manifest)
            unique_tools = list(dict.fromkeys(op.profile.name for op in plan_obj.operations))
            self.plan_tools = unique_tools
            self.plan_n_tools = len(unique_tools)

            # Extract operations
            self.plan_n_ops = len(plan_obj.operations)
            self.plan_ops = []
            for i, op in enumerate(plan_obj.operations):
                self.plan_ops.append({
                    "idx": i + 1,
                    "tool": op.tool,
                    "packets": len(op.packets)
                })

        except Exception as e:
            self.plan = None
            self.plan_file = None
            self.plan_error = f"Failed to load plan: {e}"
            self.plan_tools = []
            self.plan_ops = []

        self._notify()

    def generate_plan(self, output_path: str, feed_max=None, a_max=None,
                       jog_feed=None, lift_height=0.0, z_feed=None):
        """
        feed_max/a_max/jog_feed/z_feed/lift_height forward straight through to
        plan_job()'s subpaths_to_packets() call for every block — the same
        scalar params that function already accepts (None = fall back to the
        tool's profile / machine defaults). Uniform across the whole job.
        """
        if not self.has_valid_config or not self.has_valid_svg:
            self.plan_error = "Config and SVG must be valid to generate a plan."
            self._notify()
            return

        try:
            from host.production.planner import plan_job
            from host.production.plan_io import save_plan

            cfg = self.config

            # Same compile path as bake.py: orchestrate_layers + subpaths_to_packets
            # per block. A layer whose name resolves to no tool raises here (a
            # mislabelled layer should surface as an error, not be silently
            # skipped) — reported through the same except below as everything else.
            # overrides=tool_profiles so a TOML [tools.*] patch applies to every
            # layer's tool, not just whichever is mounted on the head.
            new_plan = plan_job(self.svg_path, cfg.machine,
                                 overrides=cfg.tool_profiles,
                                 quality=cfg.quality,
                                 feed_max=feed_max, a_max=a_max,
                                 jog_feed=jog_feed, z_feed=z_feed,
                                 lift_height=lift_height)

            save_plan(new_plan, output_path)

            # Immediately load the generated plan back into the UI for inspection
            self.load_plan(output_path)

        except Exception as e:
            self.plan = None
            self.plan_error = f"Generation failed: {e}"
            self._notify()

    @property
    def has_valid_config(self) -> bool:
        return self.config is not None

    @property
    def has_valid_svg(self) -> bool:
        return self.svg_file is not None and self.svg_error is None

    @property
    def has_valid_plan(self) -> bool:
        return self.plan is not None and self.plan_error is None
