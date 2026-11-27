# urumi-host

The host library for Urumi. It turns an SVG into a job, encodes the
controller's config, and talks to the controller over USB. It runs in a
modern browser and in Node 22.12+. TypeScript, ESM.

The controller plans and runs the motion, homing, probing, parking and tool
changes. This library prepares the job and sends it.

## Quick start

From `web/`:

```sh
pnpm install
pnpm test
pnpm demo
```

`pnpm demo` opens the machine console, a plain page over this library. Plug
in the controller and connect to it from the page.

The other scripts (`build`, `typecheck`, `lint`, `gen:runtime`,
`demo:dist`) are explained on [host build](../docs/system/host-build.md).

## Subpaths

Every import names its subpath; there is no root `urumi-host`. Each
transport has its own subpath, so a Node program never pulls in browser
globals.

| Subpath | Holds |
|---|---|
| [`urumi-host/transport`](../docs/reference/host-api/transport.md) | the `Transport` interface; `/webserial`, `/node` and `/sim` the transports |
| [`urumi-host/packets`](../docs/reference/host-api/packets.md) | the binary records the controller sends and takes |
| [`urumi-host/config`](../docs/reference/host-api/config.md) | the controller's config: JSON to config and back, and the encoded config |
| [`urumi-host/toolpath`](../docs/reference/host-api/toolpath.md) | an SVG to a job's layers |
| [`urumi-host/link`](../docs/reference/host-api/link.md) | the `Link`, job [sessions](../docs/reference/host-api/session.md) and the text [commands](../docs/reference/host-api/commands.md) |

Units, errors and waiting are on the [host API](../docs/reference/host-api/README.md)
page.

## Use it in your program

Two samples that do the same thing: connect to the controller, print its
state, and ping every node on the bus. One is a web page, the other a Node
program.

### Prerequisites

* Node 22.12 or later, and pnpm.
* The controller flashed and plugged in over USB (see the
  [root README](../README.md#quick-start)).
* For the page: a browser with the Web Serial API, such as Chrome or Edge.

### Build the library

The package isn't on npm. Build it from this folder:

```sh
pnpm install
pnpm build
```

Rebuild and reinstall it in your project after pulling changes to the repo.

### In a browser

The page needs a browser with the Web Serial API: Chrome, Edge or Opera.
Firefox and Safari don't have it. It also needs a bundler to resolve the
imports; this uses Vite. In a new folder:

```sh
npm init -y
npm pkg set type=module
npm install --install-links <path-to-repo>/web
npm install -D vite
```

Save as `index.html`:

```html
<!doctype html>
<button id="connect">Connect</button>
<pre id="out"></pre>
<script type="module" src="main.js"></script>
```

Save as `main.js`:

```js
import { Link } from "urumi-host/link";
import { WebSerialTransport } from "urumi-host/transport/webserial";

const out = document.querySelector("#out");
const print = (line) => { out.textContent += line + "\n"; };

document.querySelector("#connect").onclick = async () => {
  const link = new Link(await WebSerialTransport.request());
  try {
    const status = await link.freshStatus();
    print(`state: ${status.stateName}`);

    const cmd = link.commands();
    await cmd.ping();
    for (const [addr, result] of await cmd.pingAll()) {
      print(`node ${addr}: ${result}`);
    }
  } finally {
    await link.close();
  }
};
```

Serve it:

```sh
npx vite
```

Open the address Vite prints, click **Connect** and choose the controller's
port. The browser opens the port picker only from a click, and only on
`localhost` or HTTPS. It shows the state, then one line per bus address.

### In Node

In a new folder:

```sh
npm init -y
npm pkg set type=module
npm install --install-links <path-to-repo>/web serialport
```

`--install-links` copies the package in. Without it npm links the folder,
and the library then can't find your `serialport`.

Save as `hello.js`:

```js
import { Link } from "urumi-host/link";
import { NodeSerialTransport } from "urumi-host/transport/node";

const port = process.argv[2] ?? "/dev/ttyACM0";
const link = new Link(await NodeSerialTransport.open(port));

try {
  const status = await link.freshStatus();
  console.log(`state: ${status.stateName}`);

  const cmd = link.commands();
  await cmd.ping();
  for (const [addr, result] of await cmd.pingAll()) {
    console.log(`node ${addr}: ${result}`);
  }
} finally {
  await link.close();
}
```

Run it with the controller's port:

```sh
node hello.js COM5
```

It prints the same lines, such as:

```
state: IDLE
node 1: ok
node 2: ok
node 3: timeout
```

### Next steps

* Run a job: [`buildJob()`](../docs/reference/host-api/toolpath.md) turns
  an SVG into layers, and a [session](../docs/reference/host-api/session.md)
  sends them.
* Move the machine: the [commands](../docs/reference/host-api/commands.md),
  such as `home()` and `goto()`.
* Read and write the config: [`pullConfig()` and `pushConfig()`](../docs/reference/host-api/link.md#linkpullconfig).

## Docs

* [Host API](../docs/reference/host-api/README.md): every subpath, its exports and their behaviour.
* [Host build](../docs/system/host-build.md): the toolchain, the scripts and the demo.
* [Toolpath](../docs/system/toolpath.md): how an SVG becomes a job, and which SVG elements are read.
* [Wire](../docs/system/wire.md) and [jobs](../docs/system/jobs.md): what goes over USB and how a job runs.
* [Packets](../docs/reference/packets.md) and [commands](../docs/reference/commands.md): the byte layouts and the text commands.
