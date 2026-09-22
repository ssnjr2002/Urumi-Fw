# Urumi Controller Development Guide

Project: Open-source firmware and control software for a flatbed CNC cutter.

---

## Project Architecture

### Firmware

#### Build System: PlatformIO 

1. On Windows:
    * Location: ~/.platformio/penv/Scripts/pio.exe

**Configuration Files**:

* `platformio.ini`: Main build configuration

Contains config for 4 MCU types:

* Nodes:
    * ATtiny 3224: `attiny_base`
    * ATtiny 3226: `attiny3226_base`
    * AVR128DB32: `avr128db_base`
* Controller:
    * RP2350: `env:pico`
* Scratch: Miscellaneous envs used for testing specific things.

Node Types:

* Core (shared across all types): `node_core`
* Vacuum Controller Node: `type_vacuum`
* Knife Controller Node: `type_knife`
* Stepper Controller Nodes: `type_stepper` 

Node Environments:

* Combination of 
    * MCU
    * Node type 
    * Build Flags

Node Build Flags:

* All nodes:
    * `NODE_ID`: Specifies the node addressing id of a node.
    * `NODE_DEBUG_CONSOLE`: Address a node over USB Serial instead of RS485 Bus through a python script (host/node_console.py).
    * `NODE_HAS_PROBE_REPLY`: Allow a special stream reply mode which relays probe switch state. 
    * `RS485_USE_XDIR`: Turn on XDIR, an automatic hardware direction switch for half duplex RS485 buses. 
    * `NODE_HAS_LASER`: Node has a laser attached to it which can be toggled on or off.
* Stepper nodes:
    * `DM542`: Specify the stepper driver as a DM542 driver.
    * `TMC_2660`: Specify the stepper driver as a TMC 2660 driver.
    * `TMC_CURRENT`: Specify the current setting value for TMC 2660 driver.
    * `TMC_MICROSTEPPING`: Specify the microstepping value for TMC 2660 driver.
    * `DRV8825`: Specify the stepper driver as a DRV8825 driver.
    * `DRV_MICROSTEPPING`: Specify the microstepping value for DRV8825 driver.
    * `HAS_LIMIT_SWITCH`: Specify that a node has a limit switch and will support linear homing.
    * `LIMIT_ACTIVE_HIGH`: Specify if the limit switch signal is high when active.
    * `HAS_HALL_INDEX`: Specify that a node has hall effect sensor and may support rotary homing.
* Vacuum nodes:
    * `BOARD_DB32_VACUUM`: Specify that this board is a AVR128DB32 vacuum board.

### Software

In web/src

---

## Coding Standards

### Comment Style

* Keep comments short and write them for the merged state, as if the code had always worked this way.
* Remove before/after narration, investigation measurements, and rationale that belongs in the commit message.
* Keep only non-obvious mechanism, field/parameter meaning, or the reason a special case exists.

---

## Git Workflow

### Branch Naming Convention

**For feature/fix branches**:

```text
feature/<short-description>       # New features
fix/<issue-number>-<description>  # Bug fixes
refactor/<component-name>         # Code refactoring
```

**Examples**:

- `feature/tool-duty-limits`
- `fix/soft-limits`
- `refactor/config-storage`

### Commit Message Format

**Pattern**:

```text
<type>: <short summary (50 chars max)>

<optional detailed description>
```

**Types**: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `perf`

**Example**:

```text
feat: add tool duty limits

Implements a cooldown for the tool for the defined period.

Tested the happy path and it works.
```

### When to Commit

**DO commit when**:

- User explicitly requests: "commit these changes"

**DO NOT commit when**:

- Build fails or has warnings
- Experimenting or debugging in progress
- User hasn't explicitly requested commit

**Rule**: **If uncertain, ASK before committing.**

---