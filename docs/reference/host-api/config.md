# Config

The controller's config ([config](../../system/config.md)) in its three
forms, and the functions between them:

| Form | Is | Made by |
|---|---|---|
| **JSON** | the file a person edits, JSON or JSONC (`config/controller.jsonc`) | [`saveConfig()`](#saveconfig) |
| **config** | a `ControllerConfig` in memory, every default filled in | [`loadConfig()`](#loadconfig), [`decodeConfig()`](#encodeconfig-decodeconfig) |
| **encoded config** | the bytes the controller stores; `Link.pushConfig()` sends them | [`encodeConfig()`](#encodeconfig-decodeconfig) |

```ts
import { loadConfig, saveConfig } from "urumi-host/config";
```

**Checked against `058616d`.** To see what may have changed since:

```bash
git diff 058616d -- web/src/config/
```

# Functions

| Function | |
|---|---|
| [`loadConfig()`](#loadconfig) | JSON to a config, validated |
| [`validateConfig()`](#validateconfig) | check a config built or changed in code |
| [`saveConfig()`](#saveconfig) | a config to JSON |
| [`encodeConfig()`](#encodeconfig-decodeconfig) | a config to the encoded config |
| [`decodeConfig()`](#encodeconfig-decodeconfig) | the encoded config to a config |

### `loadConfig()`

Reads JSON, fills in the defaults, and validates the result.

**Syntax**

```ts
loadConfig(json: string): LoadResult

type LoadResult =
  | { ok: true; config: ControllerConfig; warnings: readonly string[] }
  | { ok: false; errors: readonly string[] }
```

**Parameters**

* `json`: the file's text. Comments, `//` and `/* */`, are allowed.

**Return value**

The config and its warnings, or every error found. A warning names
something legal but probably meant otherwise, such as a target above its
axis's ceiling. Errors and warnings are sentences for a person to read.

**Example**

```ts
const r = loadConfig(await file.text());
if (!r.ok) return show(r.errors);
await link.pushConfig(r.config);
```

### `validateConfig()`

The checks `loadConfig()` runs after reading, for a config built or changed
in code.

**Syntax**

```ts
validateConfig(config: ControllerConfig): ValidationResult

interface ValidationResult {
  errors: readonly string[];
  warnings: readonly string[];
}
```

**Return value**

Every error and warning; the config is valid when `errors` is empty.

### `saveConfig()`

**Syntax**

```ts
saveConfig(config: ControllerConfig): string
```

**Return value**

The config as JSON, with its defaults written out. `loadConfig()` reads it
back to the same config.

**Example**

Download the config the controller holds:

```ts
const config = await link.pullConfig();
const url = URL.createObjectURL(new Blob([saveConfig(config)], { type: "application/json" }));
```

### `encodeConfig()`, `decodeConfig()`

`Link.pushConfig()` and `pullConfig()` call these; a caller needs them only
for the bytes themselves.

**Syntax**

```ts
encodeConfig(config: ControllerConfig): Uint8Array
decodeConfig(bytes: Uint8Array): ControllerConfig
```

**Return value**

The encoded config; the config.

**Exceptions**

* `decodeConfig()`: `Error` if `bytes` isn't an encoded config of this
  version. It trusts the shape otherwise: the controller stores what was
  pushed.

# Types

`ControllerConfig` is the whole config. Its fields are described on
[config](../../system/config.md); in JSON, `toolProfiles` is written
`tools`.

```ts
interface ControllerConfig {
  readonly machine: MachineConfig;
  readonly quality: QualityConfig;
  readonly toolProfiles: Readonly<Record<string, ToolProfile>>;
}
```

| Part | Types |
|---|---|
| the machine | `MachineConfig`, `JobFeed`, `JobBuffer`, `StoredPositions`, `WorkArea`, `LaserPointer` |
| an axis | `AxisConfig`, `HomingConfig` (`LinearHoming` or `RotaryHoming`), `ProbeConfig`, `OpTarget` |
| the bus | `BusNode`, `NodeType` |
| a head | `ToolHead`, `ReferencePoint`, `ToolOffset` |
| a tool | `ToolProfile`, `ToolType`, `ToolPower`, `DutyLimits` |
| the bed | `BedPoint`, `ProbeSwitch` |
| the toolpath | `QualityConfig` |

`NodeType` and `ToolType` are const objects, each with a type of the same
name for its values.

# Values

| Name | |
|---|---|
| `PEN`, `KNIFE`, `CREASE`, `REVOLVER_PEN` | the built-in tool profiles |
| `TOOL_PROFILES` | the same, by the name JSON and an SVG layer use |
| `DEFAULTS` | the values JSON may leave out |
