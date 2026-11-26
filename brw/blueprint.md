# Offline Blueprints

Use the expected SHA-256 of `blueprint.manifest` from the package author or another trusted channel. The workspace and parent directories for file outputs must already exist. The CLI can run from any current directory.

```text
brw blueprint preview kit --sha256 DIGEST --workspace project --json
brw blueprint apply kit --sha256 DIGEST --workspace project --json
```

`apply --dry-run` also previews. Each invocation verifies the manifest bytes, loads and hashes every declared file, and builds the complete plan before it executes a step. The JSON `steps` array records the operation, operands and call path. On failure, `completed` counts successful steps and `detail` identifies the failing input or call site. Completed operations retain their results.

If read-only preflight cannot determine whether two planned output paths differing in ASCII case refer to the same destination, both preview and apply return `path-case-unknown` with `completed` set to zero. A confirmed duplicate copy destination returns `destination-exists`.

## Package layout

Version 1 uses an ASCII manifest with tab-separated fields and LF or CRLF lines. In this example, replace each digest placeholder with the SHA-256 of the named file's exact bytes.

```text
brw-blueprint-v1
entry	main.mjs
platform	any
arch	any
requires	copy
requires	replace
module	main.mjs	MAIN_SHA256
module	templates/file.mjs	CHILD_SHA256
payload	templates/input.txt	TEXT_SHA256
```

`platform` accepts the host name (`windows`, `linux` or `macos`) or `any`. Use `x86_64` or `arm64` for those architectures, or `any` for portable packages. Declare each capability once. Paths inside the manifest use forward slashes and ordinary files and directories. Links and traversal components are rejected.

The entry module can invoke a child more than once. Each invocation has its own arguments and position in the plan.

```js
// main.mjs
export default () => ["debug", "release"].map(mode => ({
  op: "include",
  module: "./templates/file.mjs",
  args: { mode }
}));
```

```js
// templates/file.mjs
export const inputs = ["mode"];
export default ({ mode }) => [
  { op: "copy", payload: "input.txt", path: mode + ".txt" },
  { op: "replace", path: mode + ".txt", from: "@MODE@", to: mode, count: 1 }
];
```

Both modules export a synchronous function returning steps. Relative ES imports may use helper modules listed in the manifest. Child module and payload references resolve relative to the declaring module. File output paths resolve inside the workspace. For example, the copy in the second invocation above has source `main.mjs[2] > templates/file.mjs[1]`.

| Capability | Operands | Applied effect |
| --- | --- | --- |
| `copy` | `payload`, `path` | Creates a new regular file from verified payload bytes. An existing destination fails. |
| `replace` | `path`, `from`, `to`, `count` | Publishes the literal replacement after the non-overlapping match count equals the expected count. |
| `env` | `name`, `value` | Sets the environment of the current brw process for subsequent actions. The calling shell keeps its own environment. |
| `run` | `program`, `args` | Runs the program in the caller’s `--workspace` and waits for it to exit. Preview lists its arguments. |
| `install` | `payload`, `name`, `version`, `entry` | On Windows, installs a verified native executable or supported archive through the package transaction and publishes its command entry. |

For example, declare `requires\trun` (with a tab) and list `build.cmake` as a payload beside the child module. This step runs CMake with the verified script and a single argument containing a space.

```js
{ op: "run", program: "cmake", args: ["-P", { payload: "build.cmake" }, "Release build"] }
```

The argument list accepts up to 128 entries, including at most one script reference. Digby resolves the reference from the declaring module and stages the verified bytes in a temporary file for this invocation. Scripts can use the working directory for project files. The temporary location contains only the standalone script. String arguments pass directly to the program, including empty strings and literal `|`. To execute a pipeline, select an interpreter such as `sh` with `["-c", scriptText]` or `powershell.exe` with `["-NoProfile", "-Command", scriptText]`. Child output goes to stderr so the CLI report remains readable on stdout. A failed launch or nonzero exit stops the plan.

The planning runtime has no filesystem, process or network bindings. A bundle may declare up to 128 files. The reader limits the manifest to 128 KiB, each module to 1 MiB, each payload to 64 MiB and loaded member data to 128 MiB. Nested calls, plan size, JavaScript heap, stack and execution time are also bounded. An unavailable capability, invalid import or exceeded planning limit fails before application.

## Passing outputs between calls

A build module declares its required outputs by name. Pass an `outputFile` argument to the command that produces the value.

```js
export const outputs = ["archive"];
export default ({mode}) => [{
  op: "run", program: "cmake",
  args: ["-P", {payload: "build.cmake"}, mode, {outputFile: "archive"}]
}];
```

The script receives a fresh temporary file path for this invocation and output name. Write the archive path into that file as one nonempty UTF-8 line (up to 4096 bytes including any final LF or CRLF). Digby reads it when the invocation finishes. An empty or invalid value fails the producing invocation and stops subsequent steps. The archive itself remains in the build workspace.

```js
export default () => [
  {op: "include", module: "build.mjs", args: {mode: "Debug"}},
  {op: "include", module: "build.mjs", args: {mode: "Release"}},
  {op: "include", module: "test.mjs", args: {archive: {from: 2, output: "archive"}}}
];
```

`from` counts positions in the declaring module's step list starting at one. A later run can use the reference directly in its argument list. A child can receive it as a named input and pass it on as a command argument. Preview shows the producing call path and name. Apply passes the resolved text as one argument.

Set a step or include's `platform` to `windows`, `linux`, `macos` or `any`. On a mismatch the plan reports the skipped position and reason. A selected command referencing a skipped producer fails during planning. Use an `optional` manifest field (for example, `optional<TAB>install`) for a capability only some platforms need. Selecting a step that needs an unavailable capability fails during planning.

## External desired states

Declare the `state` capability in the manifest to pair a read-only check with its change.

```js
export default () => [{
  op: "state",
  check: {program: "project-tool", args: ["check-config"]},
  unmet: [3],
  change: {program: "project-tool", args: ["configure"]}
}];
```

Both commands use the same working directory, argument passing and verified script payloads as `run`. They can consume earlier named outputs. Declare output producers in an unconditional `run` step. Supply a list of distinct unmet exit codes from 1 through 255. Exit 0 means satisfied. Any other unmapped exit code or launch failure stops the blueprint at this step.

Preview and dry-run may execute the check, so its author must ensure it is read-only. After an earlier action or planned change, preview reports `check-at-execution` with the reason `earlier-step-may-change-state`. Apply checks at the step's position, runs the change only for an unmet result, then repeats the check. A state counts as completed once confirmed satisfied. File operations following a state change validate their inputs against the resulting workspace. The report includes the state status and check exit code when available.

## Lock a tool from registered sources

Register the platform's catalog with `brw source add sdk /absolute/path/to/catalog`. The Blueprint CLI uses the existing `brw-catalog-v1` row format described in [the brw README](README.md). The catalog may list several versions of a package. Declare `requires<TAB>tool` in the manifest and select the tool in a module:

```js
export default () => [{op: "tool", registry: "sdk", name: "builder", version: "1.*"}];
```

```sh
brw blueprint lock kit --sha256 DIGEST --workspace project --json
brw blueprint preview kit --sha256 DIGEST --workspace project --json
brw blueprint apply kit --sha256 DIGEST --workspace project --json
```

Run `lock` to generate or refresh `kit/blueprint.<os>-<arch>.lock` on the target host. Commit that file alongside the manifest and review changes to both. The manifest digest covers the Blueprint modules and payloads. The separate lock file is a trusted input specifying the exact source, artifact location, digest, entry and dependency names for each resolved version.

Use an exact version, `*`, or a dotted prefix such as `1.*`. Generation selects the highest matching version, comparing digit runs numerically and other characters by ASCII value. Every occurrence of a tool contributes its requirement. Dependency names resolve across registered sources. If multiple sources offer a compatible package, qualify its declaration with `registry`. Missing packages, ambiguous versions and cycles abort generation before the current lock is replaced.

Preview and apply read the host lock before starting any step. The `locked` array lists dependencies before their consumers. A missing or conflicting record reports the package and OS/architecture. At a tool step, apply stages the pinned artifact bytes in the user's Burrow cache and verifies SHA-256 before publishing them. A cache hit requires a matching digest. An installed single-file dependency at the recorded name and version can supply the original bytes after verification. For an installed archive, reuse requires the original verified archive bytes. On Linux and macOS, HTTP(S) sources use `curl`. Ordinary apply keeps the selected versions when the catalog gains new releases. Unlocked install steps retain their existing behavior.

## Build and test

On Windows, prepare the pinned sources with `scripts/fetch-deps.ps1`. The MinGW-w64 build enables Blueprints by default.

On Linux and macOS, run the following commands from the repository root with a native C compiler, CMake 3.19 or newer, curl and tar. The preparation script verifies the pinned QuickJS and libgit2 archives in `.local/deps/sources` and downloads any missing archives. With both archives present, preparation, build and tests run offline.

```sh
sh scripts/fetch-blueprint-deps.sh
cmake -S brw -B .local/brw-blueprint -DBRW_BLUEPRINT=ON -DBUILD_TESTING=ON
cmake --build .local/brw-blueprint --parallel
ctest --test-dir .local/brw-blueprint -R blueprint --output-on-failure
```
