# Urumi

Firmware and host software for Urumi, an open-source flatbed CNC cutter. It
has two heads and can carry several tools. A single controller runs the
machine, and each axis or device has its own small node board, all connected
over an RS485 bus.

Urumi is being developed at Super Fablab Kochi in collaboration with the MIT
Center for Bits and Atoms, as part of the
Machines that Make project.

## Hardware

* **Controller:** a Raspberry Pi Pico 2 (RP2350) with a half-duplex RS485
  transceiver, connected to the PC over USB.
* **Nodes:** one small AVR board per axis or device, each with its own RS485
  transceiver, on one bus with the controller:

  | Board | Chip | Runs |
  | --- | --- | --- |
  | ATtiny3224 stepper | ATtiny3224 | a stepper axis |
  | ATtiny3226 vacuum | ATtiny3226 | the vacuum pump, servos and probe switch |
  | DB32 stepper | AVR128DB32 | a stepper axis |
  | DB32 vacuum | AVR128DB32 | the vacuum pump, servos and probe switch |
  | DB32 knife | AVR128DB32 | the oscillating knife and its blower |

* **Stepper drivers:** a DM542, TMC2660 or DRV8825 on each stepper node, set
  by the node's env.
* **For flashing:** a USB cable for the Pico, and a serial UPDI adapter for
  the nodes.

Each board's pins: [docs/system/nodes.md](docs/system/nodes.md).

## Quick start

### Get the code

Clone this repository with the URL from its page: the **Code** button on
GitHub, or **Clone** on GitLab.

```sh
git clone <the repository's URL>
```

You need [VS Code](https://code.visualstudio.com/) with the
[PlatformIO IDE](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide)
extension, which VS Code offers when you open the folder. The
[Serial Monitor](https://marketplace.visualstudio.com/items?itemName=ms-vscode.vscode-serial-monitor)
extension is optional, for talking to the Pico by hand. For the config and
the host you also need [Node](https://nodejs.org/) 22.12 or later and
[pnpm](https://pnpm.io/), then once:

```sh
cd web
pnpm install
```

Open the repo's root folder in VS Code (File → Open Folder). PlatformIO reads
`platformio.ini` and lists every env in its sidebar.

### Flash the controller

1. Plug the Pico in over USB.
2. In the PlatformIO sidebar, open **Project Tasks → pico → General** and
   click **Upload**.
3. Then **Project Tasks → pico → Platform → Upload Filesystem Image**. This
   writes the machine's config (below) to the Pico.

### The config

The machine is described by `config/controller.jsonc`: its axes, heads,
tools and nodes. Edit it for your machine, then run **Upload Filesystem
Image** again; a plain **Upload** leaves the config on the Pico as it was.
Every field: [docs/system/config.md](docs/system/config.md).

### Flash a node

Each node has its own env, named after its type and node id. For example,
to flash the vacuum node with id 7:

1. Connect a serial UPDI adapter to the node's UPDI pin and plug the adapter
   into the PC.
2. Choose the adapter's port. Either pick it from the port selector (the plug
   icon) in VS Code's status bar, or name it in the env in `platformio.ini`:

   ```ini
   [env:vac_node7]
   upload_port = COM5
   ```

   With neither, PlatformIO picks a port itself.
3. In the PlatformIO sidebar, open **Project Tasks → vac_node7 → General**
   and click **Upload**.

An env ending in `_dbg` builds a node you can drive
from a PC with `scripts/node_console.py`, with no controller or bus.

### Run the console

The demo in `web/demo` is a machine console in the browser: connect, home,
jog, load an SVG and run it.

```sh
cd web
pnpm install   # once; skip if you did it under Get the code
pnpm demo
```

It opens in your browser. Use Chrome or Edge, which have Web Serial, and press
**Connect** to pick the Pico's port. Close the Serial Monitor first; only one
program can hold the port.

To use the host library in your own software, see [web/README.md](web/README.md).

## Folders

| Folder | Holds |
| --- | --- |
| `src/` | the firmware: the controller in `src/rp2350/`, the nodes in `src/node/`, bench firmware in `src/scratch/` |
| `lib/` | firmware libraries shared by the controller and the native tests, such as the planner |
| `include/` | shared firmware headers |
| `test/` | the firmware's native tests |
| `web/` | the host: the `urumi-host` library and its demo ([web/README.md](web/README.md)) |
| `config/` | the machine's config, `controller.jsonc` |
| `data/` | the config image built from `config/`, flashed to the controller (generated) |
| `scripts/` | build hooks, the node console and bench scripts |
| `docs/` | the docs ([docs/README.md](docs/README.md)) |

## Docs

* What the machine does and how it works: [docs/README.md](docs/README.md)
* Building the firmware: [docs/system/firmware-build.md](docs/system/firmware-build.md)
* Building the host: [docs/system/host-build.md](docs/system/host-build.md)
* The host library's API: [docs/reference/host-api/README.md](docs/reference/host-api/README.md)
* Working on the repo: [AGENTS.md](AGENTS.md)
