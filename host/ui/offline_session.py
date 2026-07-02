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
    def load_config(self):
        """
        Loads and validates the configuration.
        For now, we simply import the local 'config.py' module.
        """
        import importlib
        import sys
        import os
        
        stages_path = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'pipeline', 'stages'))
        if stages_path not in sys.path:
            sys.path.insert(0, stages_path)
            
        try:
            # We import here to allow reloading if the user edits the file while the app is open
            import config
            importlib.reload(config)
            
            loaded_config = config.default()
            
            # Simple validation: ensure it has a machine definition
            if not getattr(loaded_config, 'machine', None):
                raise ValueError("Config is missing a 'machine' definition.")
                
            # If we reach here, it's valid
            self._app_state.is_sim = False
            self.config = loaded_config
            self.config_error = None
            
        except Exception as e:
            self.config = None
            self.config_error = str(e)
            
        self._notify()

    def load_sim_config(self):
        """Loads the simulator configuration."""
        import importlib
        import sys
        import os
        
        stages_path = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'pipeline', 'stages'))
        if stages_path not in sys.path:
            sys.path.insert(0, stages_path)
            
        try:
            import config
            importlib.reload(config)
            
            import host.sim_config
            importlib.reload(host.sim_config)
            from host.sim_config import sim_machine
            from dataclasses import replace
            
            # Get the sim machine
            machine = sim_machine()
            
            # Wrap in PipelineConfig
            base_config = config.default()
            loaded_config = replace(base_config, machine=machine)
            
            self._app_state.is_sim = True
            self.config = loaded_config
            self.config_error = None
            
        except Exception as e:
            self.config = None
            self.config_error = f"Failed to load sim config: {e}"
            
        self._notify()

    # ---------------------------------------------------------
    # 2. SVG Management
    # ---------------------------------------------------------
    def load_svg(self, path: str):
        """Loads an SVG, extracts layers, and performs basic tool matching."""
        if not self.has_valid_config:
            self.svg_error = "Config must be valid before loading SVG."
            self._notify()
            return

        import os
        import sys
        # Ensure we can import the pipeline module from the root
        sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

        try:
            from pipeline.stages.stage2 import load_svg_mm_layers
            
            layers_mm, viewport = load_svg_mm_layers(path)
            
            self.svg_file = os.path.basename(path)
            self.svg_path = path
            
            # Simple bounding box string format (skipping strict limits check for now as requested)
            w_mm, h_mm = viewport[4], viewport[5]
            self.svg_bounds = f"{w_mm:.1f} x {h_mm:.1f} mm"
            
            # Attempt to match layer names to known tools in the config
            import config
            valid_tool_names = []
            if hasattr(config, 'TOOL_PROFILES_BY_TYPE'):
                valid_tool_names = [t.name.lower() for t in config.TOOL_PROFILES_BY_TYPE.values()]
                
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
            
        import os
        import sys
        sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
        try:
            from host.plan_io import load_plan as plan_io_load
            
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
        
    def generate_plan(self, output_path: str):
        if not self.has_valid_config or not self.has_valid_svg:
            self.plan_error = "Config and SVG must be valid to generate a plan."
            self._notify()
            return
            
        import sys
        import os
        sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
        try:
            from host.production.planner import Plan, ToolOperation
            from host.production.svg_to_packets import subpaths_to_packets
            from pipeline.stages.stage2 import load_svg_mm_layers
            import config
            
            # Reload layers from disk
            layers_mm, _ = load_svg_mm_layers(self.svg_path)
            
            ops = []
            for layer_name, subpaths in layers_mm.items():
                if not subpaths: 
                    continue
                
                # Match tool
                profile = None
                if hasattr(config, 'TOOL_PROFILES_BY_TYPE'):
                    for t in config.TOOL_PROFILES_BY_TYPE.values():
                        if t.name.lower() == layer_name.lower():
                            profile = t
                            break
                            
                if profile is None:
                    continue # Ignore layers with no matching tool profile
                    
                packets = subpaths_to_packets(subpaths, self.config.machine, profile)
                if packets:
                    ops.append(ToolOperation(tool=profile.name, profile=profile, packets=packets))
                    
            if not ops:
                raise ValueError("No layers matched valid tools, or matched layers had no valid paths.")
                
            new_plan = Plan(operations=ops)
            
            from host.plan_io import save_plan
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
