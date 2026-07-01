# UI Architecture

The host GUI is split into two primary MVC contexts: the **Offline Tab** (authoring and job preparation) and the **Online Tab** (machine execution and jogging). Both follow a strict matrix pattern where components Define an entity, Monitor its State/Status, and Expose Controls gated by that status.

## 1. Offline UI Architecture (Dependency-Gated Workflow)

The Offline tab acts as a linear setup wizard. Each step exposes specific UI feedback and unlocks the subsequent step upon successful validation.

| Workflow Step | Prerequisite Gate | User Action | Success State (UI Feedback) |
| :--- | :--- | :--- | :--- |
| **1. Load Configuration** | *None* | Select & load `config.py` | Config validated. Displays Machine class name. **Unlocks Step 2.** |
| **2. Ingest Artwork** | Valid Configuration | Select & load `.svg` file | Parses layers and matches them to configured tool profiles. Displays bounding box. **Unlocks Step 3.** |
| **3. Generate Plan** | Valid Config + Valid SVG | Click "Generate .plan..." | Compiles SVG into MicroSegments and saves binary `.plan`. Displays Tool Manifest and Operations sequence. **Ready for Online Execution.** |
| *(Optional) Inspect Plan* | Valid Configuration | Click "Load .plan..." | Bypasses Step 2 & 3 to inspect an existing binary `.plan`. |

## 2. Online UI Architecture

| Section | Definition (What is it?) | State/Status (Monitoring) | Control (Interaction) |
| :--- | :--- | :--- | :--- |
| **Master** | COM Port | *Single button encapsulates control & state* | *Single button encapsulates control & state* |
| | State & Reason | Display the current state and reason | State Machine Controls: ESTOP, Alarm, Unalarm etc |
| | Master Enable | Display the state (all present axes Homed & Enabled) | Button to enable all, disable all |
| | Master Global Command | Master Global Last Command Ack Nack Display | Global Machine Controls: SetOrigin, other commands etc |
| **Bus Nodes** | BusNode: id, role, present | If present, monitor last ping and last enabled status | If present, separate enable button and disable button. Also ping button (if present) |
| **Axis Nodes** | Axis Setup & Info *(per present axis)* | Display axis, node id, monitor Homed state, Position (mm) | SetOrigin for this axis |
| | Axis Jogging *(per present axis)* | *(Implicitly uses Axis Position state)* | Jog controls: increment, decrement, set value for relative position adjustment, target rate, target accel |
| **Job Execution** | Preflight: Assuming all offline checks are done | *No controls, just display all the preflight checks and their status* | *(No controls)* |
| | Job State: Are we in preflight? Paused? Running? | *Pause & Resume button and state encapsulated, Start & Cancel button and state encapsulated* | *Pause & Resume button and state encapsulated, Start & Cancel button and state encapsulated* |

## Bridge: AppState

An `AppState` object bridges the two isolated tabs, acting as the single source of truth for properties that cross the boundary (e.g., the currently loaded `Config` from Offline mode that dictates what axes to build in the Online mode, and the generated `.plan` file that Online mode executes).
