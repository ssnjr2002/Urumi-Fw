# Transports

The byte link under a [`Link`](link.md): the `Transport` interface and the
three transports that implement it. Each transport is its own subpath, so a
Node program never loads browser code and a page never loads Node's.

| Import | Holds | Runs in |
|---|---|---|
| `urumi-host/transport` | [`Transport`](#transport) | anywhere |
| `urumi-host/transport/webserial` | [`WebSerialTransport`](#webserialtransport) | a browser with Web Serial |
| `urumi-host/transport/node` | [`NodeSerialTransport`](#nodeserialtransport) | Node, with `serialport` installed |
| `urumi-host/transport/sim` | [`SimTransport`](#simtransport) | anywhere |

The caller opens the port; `new Link(transport)` takes it from there, the
same in every environment. One `Link` per transport.

**Checked against `e8de999`.** To see what may have changed since:

```bash
git diff e8de999 -- web/src/transport/
```

```ts
import { Link } from "urumi-host/link";
import { WebSerialTransport } from "urumi-host/transport/webserial";

const link = new Link(await WebSerialTransport.request());
```

# `Transport`

```ts
import type { Transport } from "urumi-host/transport";
```

A byte link to the controller. Implement it to run a `Link` over anything
else: a socket, a recording.

```ts
interface Transport {
  write(bytes: Uint8Array): Promise<void>;
  read(): AsyncIterable<Uint8Array>;
  close(): Promise<void>;
}
```

## Methods

| Method | |
|---|---|
| `write(bytes)` | send bytes; resolves once the port has accepted them. Bytes from one call are never interleaved with another's. |
| `read()` | the bytes from the controller, as chunks in arrival order. Chunk edges need not fall on frame edges. Ends when the port closes. The `Link` calls it once. |
| `close()` | close the port; resolves once closed. `read()` ends. |

# `WebSerialTransport`

```ts
import { WebSerialTransport } from "urumi-host/transport/webserial";
```

The browser's serial port, through the Web Serial API (Chrome, Edge,
Opera). Made by one of two static methods; there is no public constructor.

## Methods

| Method | |
|---|---|
| [`request()`](#webserialtransportrequest) | show the port picker, open the chosen port |
| [`fromPort()`](#webserialtransportfromport) | wrap a port the caller has opened |

### `WebSerialTransport.request()`

Shows the browser's port picker, opens the port the user chooses, and wraps
it. The browser allows the picker only from a user gesture, such as a click.

**Syntax**

```ts
WebSerialTransport.request(baudRate?: number): Promise<WebSerialTransport>
```

**Parameters**

* `baudRate`: default 115200. The controller's USB port ignores it.

**Return value**

A transport over the open port.

**Exceptions**

The browser's, passed through:

* `NotFoundError` if the user closes the picker without choosing.
* `SecurityError` if not called from a user gesture.
* `NetworkError` if the port is in use or can't be opened.

**Example**

```ts
connectButton.onclick = async () => {
  link = new Link(await WebSerialTransport.request());
};
```

### `WebSerialTransport.fromPort()`

Wraps a port the caller has chosen and opened. Use it to reconnect without
the picker (`navigator.serial.getPorts()` lists ports the user has already
granted), to pick with your own filters, or to keep the `SerialPort` object
for `getInfo()`, `forget()` or its `disconnect` event.

**Syntax**

```ts
WebSerialTransport.fromPort(port: SerialPort): WebSerialTransport
```

**Parameters**

* `port`: an open `SerialPort`.

**Return value**

A transport over the port.

**Exceptions**

* `Error` if `port` is not open.

**Example**

```ts
const [port] = await navigator.serial.getPorts();
await port.open({ baudRate: 115200 });
const link = new Link(WebSerialTransport.fromPort(port));
```

# `NodeSerialTransport`

```ts
import { NodeSerialTransport } from "urumi-host/transport/node";
```

A serial port in Node, through the `serialport` package. The library
doesn't depend on `serialport`; a program that uses this transport
installs it.

## Methods

| Method | |
|---|---|
| [`open()`](#nodeserialtransportopen) | open a serial port |

### `NodeSerialTransport.open()`

Loads `serialport`, opens the port at `path`, and wraps it.

**Syntax**

```ts
NodeSerialTransport.open(path: string, baudRate?: number): Promise<NodeSerialTransport>
```

**Parameters**

* `path`: the port, such as `"/dev/ttyACM0"` or `"COM5"`.
* `baudRate`: default 115200. The controller's USB port ignores it.

**Return value**

A transport over the open port, once it is open.

**Exceptions**

* `Error` naming `serialport` if the package isn't installed.
* `serialport`'s error if the port can't be opened, such as a wrong path or
  a port in use.

**Example**

```ts
const link = new Link(await NodeSerialTransport.open("/dev/ttyACM0"));
```

# `SimTransport`

```ts
import { SimTransport } from "urumi-host/transport/sim";
```

A simulated controller, for tests and work without hardware. It answers text
commands, status, config and jogs, and runs a job stream, as the firmware
does: it holds, buffers, acknowledges in batches and refuses with the
firmware's codes. It does not plan motion: each record takes a fixed time
and lands the head on its end point.

## Constructor

```ts
new SimTransport(options?: SimOptions)
```

**Parameters**

`options`, every field optional:

| Option | Type | Default | |
|---|---|---|---|
| `axesMap` | `(number \| null)[]` | none | the bus node on each motion slot, committed at boot; without it the sim boots unmapped, as the firmware does with no config |
| `busNodes` | `number[]` | `[1, 2, 3, 4]` | the bus nodes that answer; a relay to another times out |
| `ringSize` | `number` | 64 | records the controller can queue |
| `ackCoalesceMax` | `number` | 8 | records per ACK at most |
| `frameMs` | `number` | 40 | the sim's tick, ms |
| `recordMs` | `number` | `frameMs` | how long one record takes to run, ms |
| `bufferMs` | `number` | 1000 | queued time that starts a job running, ms |
| `bufferIdleMs` | `number` | 500 | quiet time after the last record that starts it running, ms |
| `tools` | `{ mounted?, zeroed? }` | none | check each layer's tool: `mounted` maps a head to its tool, `zeroed` lists heads with a Z zero. Without it, tools are not checked. |
| `toolChangeMs` | `number` | 2 × `frameMs` | how long a tool change moves, ms |
| `jogSpm` | `number[]` | `[1, 1, 1, 1]` | steps per unit on X, Y, Z, A, negative on an inverted axis |
| `cjogStepsPerS` | `number` | 2000 | jog rate at full speed, steps/s |
| `cjogTravel` | `[[lo, hi], [lo, hi]]` | none | X and Y travel in steps, where a continuous jog stops; without it a jog runs until released |
| `jogUnhomed` | `boolean` | false | allow a continuous jog on an unhomed axis |

**Example**

```ts
const link = new Link(new SimTransport({ axesMap: [1, 2, 3, null] }));
```
