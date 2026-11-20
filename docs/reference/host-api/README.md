# Host API

The public interface of `urumi-host`, the TypeScript library in `web/`. How
the package is built and run is on [host build](../../system/host-build.md);
what it does is on [toolpath](../../system/toolpath.md),
[wire](../../system/wire.md) and [jobs](../../system/jobs.md).

The library has six subpaths and no root import: every import names its
subpath. Each subpath gets its own page here as it is reworked
([plan](../../plans/host-api.md)); until then, its `index.ts` lists what it
exports.

## Index

| Subpath | Holds | Depends on |
|---|---|---|
| `urumi-host/transport` | the `Transport` interface | — |
| `urumi-host/transport/webserial` | `WebSerialTransport` | `urumi-host/transport` |
| `urumi-host/transport/node` | `NodeTransport` | `urumi-host/transport` |
| `urumi-host/transport/sim` | `SimTransport` | `urumi-host/transport`, `urumi-host/packets` |
| `urumi-host/packets` | packet layouts, encoders and decoders, the CRCs, `MachineStatus` and its enums | — |
| `urumi-host/config` | the controller config: load, validate, save, encode | `urumi-host/packets` |
| `urumi-host/toolpath` | an SVG layer to a job's packets | `urumi-host/packets`, `urumi-host/config` |
| `urumi-host/link` | `Link`, job sessions, names for codes, text-command helpers, waiting for the machine | `urumi-host/transport`, `urumi-host/packets`, `urumi-host/config` |

No subpath depends on one below it in this table, except the sim on
`urumi-host/packets`.

```ts
import { WebSerialTransport } from "urumi-host/transport/webserial";
import { loadConfig } from "urumi-host/config";
import { prepareBezierJob } from "urumi-host/toolpath";
import { Link, JobSource } from "urumi-host/link";

const link = new Link(await WebSerialTransport.requestAndOpen());
```

## Units

| Quantity | Unit |
|---|---|
| Position, length, depth, thickness | mm |
| Feed | mm/s |
| Angle | degrees, except where a field says radians |
| Time | ms |
| Coordinates | machine work coordinates, Y up |

## Errors

Two kinds:

* **Results** for input a caller should expect to be wrong, such as a
  config file: `{ ok: true, … }` or `{ ok: false, errors }`, where `errors`
  is a list of strings for a person to read. `loadConfig()` returns one.
  `prepareBezierJob()` returns `{ error }` instead, until its rework.
* **Throws** for a refusal from the controller, a timeout, or a programming
  mistake. A config refusal throws `ConfigTransferError`, whose `reason` is
  the `CFG_NACK_*` code, or `null` when the controller didn't answer.

## Waiting

A `Link` call that needs a reply returns a promise and takes a timeout in
ms (`timeoutMs`). A call that needs none, such as `abort()` or `cjog()`,
returns at once. The controller acknowledges a command before its motion
ends, so a script that must wait for the machine polls the status with
`settle()` or `waitAtRest()`.
