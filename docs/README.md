# Urumi docs

Urumi is a flatbed CNC cutter. A gantry carries heads over the bed; each
head has a Z that lifts and lowers its tool and an A that turns the tool's
holder. A node on the RS485 bus drives each axis or device, the controller (an
RP2350 Pico) plans motion and runs the machine, and the host (`web/src`) sends
it jobs and commands over USB.

## Folders

| Folder | Holds |
| --- | --- |
| `machine/` | what you see the machine do and what to expect: one page per thing you start, watch and end: [homing](machine/homing.md) and [positioning](machine/positioning.md) |
| `reference/` | exact strings, each page marked with the commit it was checked against: [commands](reference/commands.md) |
| `plans/` | one plan per change, with a section per branch; done with once its branches merge |
| `archive/` | docs from hosts and designs the project has left; its README names what replaced each |

The docs directly in `docs/` are design notes, audits and measurements written
before these folders. Each moves out when a page replaces it.

## Notation

Each string is written the way you meet it:

| Written as | Is |
| --- | --- |
| `home unhomed` | a command, as you type it |
| `→ err estop` | the controller's reply |
| `get homefail`, `→ homefail=1` | a status key: asked for with `get`, read as `key=value` |
| `machine.x.homing.backoffDist` | a config field, as its full path in `config/controller.jsonc` |
| `<node>` | a placeholder you fill in |

In config paths, `<linear>` is any linear axis (`x`, `y` or `heads[].z`),
`<rotary>` any rotary axis (`heads[].a`), and `<axis>` either.
