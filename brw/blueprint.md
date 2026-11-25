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
| `install` | `payload`, `name`, `version`, `entry` | On Windows, installs a verified native executable or supported archive through the package transaction and publishes its command entry. |

The planning runtime has no filesystem, process or network bindings. A bundle may declare up to 128 files. The reader limits the manifest to 128 KiB, each module to 1 MiB, each payload to 64 MiB and loaded member data to 128 MiB. Nested calls, plan size, JavaScript heap, stack and execution time are also bounded. An unavailable capability, invalid import or exceeded planning limit fails before application.

## Build and test

On Windows, prepare the pinned sources with `scripts/fetch-deps.ps1`. The MinGW-w64 build enables Blueprints by default.

On Linux and macOS, run the following commands from the repository root with a native C compiler, CMake 3.19 or newer, curl and tar. The preparation script verifies the pinned QuickJS and libgit2 archives in `.local/deps/sources` and downloads any missing archives. With both archives present, preparation, build and tests run offline.

```sh
sh scripts/fetch-blueprint-deps.sh
cmake -S brw -B .local/brw-blueprint -DBRW_BLUEPRINT=ON -DBUILD_TESTING=ON
cmake --build .local/brw-blueprint --parallel
ctest --test-dir .local/brw-blueprint -R blueprint --output-on-failure
```
