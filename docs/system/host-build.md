# Host build

**Checked against `4613119`.** To see what may have changed since:

```bash
git diff 4613119 -- web/package.json web/pnpm-workspace.yaml web/tsconfig.json web/tsconfig.build.json web/vitest.config.ts web/eslint.config.js web/scripts/ web/demo/
```

Commands and code names are written as the [notation](../README.md#notation)
says.

The host is the software on the PC that turns drawings into jobs and talks to
the controller over USB. It is one TypeScript package, `urumi-host`, in
`web/`. A browser page or a Node program imports it. The machine console in
`web/demo/` is one such page, and the tests and the firmware build's
pre-script are two more users.

This page explains how that package is built and run: the runtime and package
manager, the TypeScript setup, what the package exports, its dependencies,
and the tools that check it, test it, generate files from it and serve the
demo. What the code does is on the [toolpath](toolpath.md),
[wire](wire.md) and [jobs](jobs.md) pages; the tests themselves are on the
tests page.

There are four targets, each a stack of the same layers:

| Target | Runtime | Our source | Tool | Output |
|---|---|---|---|---|
| Library | Node or a browser | `src/` | `tsc` | `dist/` |
| Tests | Node | `src/`, `test/` | vitest | a pass or a failure |
| Scripts | Node | `scripts/`, `src/` | vite-node | files outside `web/` |
| Demo | a browser | `demo/`, `src/` | vite | a page on a local server |

All four install with pnpm into one `web/node_modules`. Only the library is
compiled; the other three run the TypeScript directly, stripped of types on
the fly.

## History

The package was scaffolded in July 2026 as a TypeScript port of the Python
host (`host/`, now deprecated), with pnpm and vitest from the first commit.
A week later it was made publishable as an npm library: the two configs, the
`exports` map and `dist/`. The firmware build began calling its encoder for
the flashed config in October, and the demo console arrived at the end of
that month. Until `4613119` the package carried two vites, 5 under vitest
and vite-node and 8 for the demo, and claimed Node 18, which the demo's
vite never ran on; that commit moved everything to one vite and Node 22.12.

## Node and pnpm

The package needs Node 22.12 or later (`engines`), the floor of the vite it
runs on. Nothing enforces it at install; an older Node fails when vite or
vitest starts.

Dependencies install with pnpm from `web/`:

```bash
pnpm install
```

`pnpm-lock.yaml` pins every version, and a clean install
(`pnpm install --frozen-lockfile`) refuses a lockfile that disagrees with
`package.json`. `pnpm-workspace.yaml` holds one setting, `allowBuilds:
esbuild: true`, which let esbuild run its install script; pnpm blocks
install scripts otherwise. Since vite 8 nothing installs esbuild, so the
setting does nothing. The pnpm version itself is not pinned.

A new worktree has no `web/node_modules`, so `pnpm install` comes first
there (`AGENTS.md` says the same).

## TypeScript

Two configs:

* `tsconfig.json` checks. It covers `src/`, `test/` and `scripts/`, emits
  nothing, and is what `pnpm typecheck` and the editor read. eslint doesn't
  read it: its rules work file by file, without types.
* `tsconfig.build.json` emits. It extends the first, covers only `src/`, and
  writes JavaScript, declarations and source maps to `dist/`.

Both target ES2022 with the DOM libraries, as ESM (`"type": "module"`), with
`bundler` resolution. Imports name the `.js` file of a `.ts` source
(`./geometry.js`), which is what both `tsc` and the browser resolve.

The flags that shape the code: `strict`, `noUncheckedIndexedAccess` (an
indexed read is `T | undefined`, hence the `!` after checked indexes),
`verbatimModuleSyntax` and `isolatedModules` (a type-only import says
`type`, so a tool that strips types file by file needs no type information),
and `noImplicitOverride`.

`demo/` is in neither config: it is plain JavaScript and is not type
checked. Only eslint reads it.

## The package

The package has no root entry: every import names a subpath, and each
subpath is a folder in `src/` whose `index.ts` is its public interface.
`package.json` exports eight:

| Import | Folder | What |
|---|---|---|
| `urumi-host/transport` | `transport/index.ts` | the `Transport` interface |
| `urumi-host/transport/webserial` | `transport/webserial.ts` | the browser's serial port |
| `urumi-host/transport/node` | `transport/node.ts` | a serial port in Node, through the `serialport` package |
| `urumi-host/transport/sim` | `transport/sim.ts` | a fake controller for tests and the demo |
| `urumi-host/packets` | `packets/` | the packet layouts |
| `urumi-host/config` | `config/` | the config document |
| `urumi-host/toolpath` | `toolpath/` | SVG to a job |
| `urumi-host/link` | `link/` | the connection to the controller |

Each transport is its own subpath so that a Node program never loads browser
globals and a page never loads Node's. The API reference is
[host-api](../reference/host-api/README.md). `sideEffects: false` lets a
bundler drop what a page doesn't import. `files` ships only `dist/`, the
README and the licence.

The Node transport loads `serialport` with a dynamic `import()` when it opens
a port, so nothing else needs it. Its comment calls it an optional peer
dependency, but `package.json` doesn't declare it: a Node program must
install it itself.

The package is not published. `prepublishOnly` would build it first, and
`pnpm build` builds it by hand, for the demo's `dist` mode.

## Dependencies

At runtime, one: `@msgpack/msgpack`, for the config's MessagePack
encoding.

For development:

* `typescript`: `tsc`.
* `vitest`: the test runner.
* `vite`: the demo's server, and the engine under vitest and vite-node.
* `vite-node`: runs a TypeScript script in Node, for the scripts.
* `eslint`, `@eslint/js`, `typescript-eslint`, `globals`: lint.
* `@xmldom/xmldom`: a `DOMParser` for Node. The SVG loader uses the global
  `DOMParser`, which Node lacks; `test/setup.ts` installs this one.
* `@types/node`: Node's types, for the scripts and the Node transport.

## Tools

The scripts in `package.json`, all run from `web/`:

| Script | Runs |
|---|---|
| `pnpm typecheck` | `tsc --noEmit` |
| `pnpm lint` | `eslint .` |
| `pnpm test` | `vitest run` |
| `pnpm test:watch` | `vitest` |
| `pnpm build` | `tsc -p tsconfig.build.json` |
| `pnpm gen:runtime` | `vite-node scripts/runtime-fields.ts` |
| `pnpm demo` | `vite demo --open` |
| `pnpm demo:dist` | `vite demo --mode dist --open` |

### Typecheck and lint

`pnpm typecheck` and `pnpm test` are the checks `AGENTS.md` asks for after
a change in `web/`. `pnpm lint` runs eslint's recommended rules and
typescript-eslint's over everything but `node_modules` and `dist`, with
unused names allowed when they start with `_`. The demo's files get the
browser's globals, and its vite config Node's.

### Tests

`vitest.config.ts` runs `test/**/*.test.ts` in Node, after `test/setup.ts`.
The tests page covers the suites.

### Scripts

Three scripts in `web/scripts/` write files for the firmware:

| Script | Writes | Run by |
|---|---|---|
| `config-image.ts` | `data/config.bin`, the flashed config | `scripts/pio/config_image.py`, on `pio run -e pico -t buildfs` or `uploadfs` |
| `mesh-image.ts` | `data/mesh.bin`, the bed mesh | the same pre-script, when the env names a `custom_mesh_json` |
| `runtime-fields.ts` | `src/rp2350/core0/config/runtime_fields.h` | `pnpm gen:runtime`, by hand |

`config-image.ts` uses the same encoder the host uses to push a config with
`CFG_SET`, so a flashed config and a pushed one are the same bytes past the
file header. The mesh has no push; flashing is its only way in. The pre-script calls
them as `pnpm --dir web exec vite-node <script> -- <in> <out>`, so
`uploadfs` needs pnpm on the `PATH` and `pnpm install` run in `web/`; the
[firmware build](firmware-build.md) page covers the pre-script.

`runtime-fields.ts` writes a C++ header into the controller's source from
the host's field table. The header is committed; rerun the script after the
table changes, then build the controller.

### The demo

`pnpm demo` serves `web/demo/` with vite and opens it. `demo/vite.config.js`
points each `urumi-host/*` subpath at `src/`, so a change to the library shows on reload.
`pnpm demo:dist` resolves `urumi-host` through the package's `exports`
instead, as another project would, and needs `pnpm build` first.

The page talks to the machine through the browser's serial port. Where a
browser has no port picker, `demo/bridge/` stands in: a Python bridge
(`com_bridge.py`, needs pyserial) holds the port and serves it on a local
WebSocket, and `shim.js`, pasted into the page, routes Connect to it. Its
README has the steps.

## Glossary

* **ESM:** JavaScript modules with `import` and `export`; the package uses
  nothing else.
* **`exports`:** the map in `package.json` from an import path to a file.
* **typecheck:** running `tsc` to find type errors without writing files.
* **emit:** `tsc` writing JavaScript and declarations.
* **vite:** a dev server and bundler; here, the demo's server.
* **vite-node:** vite's engine as a Node command, to run a `.ts` file
  directly.
* **lockfile:** `pnpm-lock.yaml`, the exact version of every dependency.

## Decisions

* **ESM only, with flat subpaths.** One module format, every import naming
  its layer, and the platform-specific transports split by import path. The
  alternative, one entry with runtime detection, would make every bundle
  carry both real-port transports.
* **One config to check and one to emit.** The tests and scripts are
  checked with the library but never shipped. A single config would either
  ship the tests or leave them unchecked.
* **vite-node for the scripts.** They import the library's TypeScript as
  is. The alternative, building `dist/` first, would make every `uploadfs`
  depend on a fresh build.
* **The demo reads `src/` by default.** A library edit shows on reload with
  no build; `dist` mode remains to check the package as shipped.
* **The flashed config is encoded by the host.** The firmware has no JSON
  config encoder of its own; the cost is that `uploadfs` needs this package
  installed.

## Code

* `web/package.json`, `web/pnpm-lock.yaml`, `web/pnpm-workspace.yaml`
* `web/tsconfig.json`, `web/tsconfig.build.json`
* `web/eslint.config.js`, `web/vitest.config.ts`, `web/test/setup.ts`
* `web/src/*/index.ts`
* `web/scripts/config-image.ts`, `mesh-image.ts`, `runtime-fields.ts`
* `web/demo/vite.config.js`, `web/demo/bridge/`
* `scripts/pio/config_image.py`
