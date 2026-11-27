# Offline Blueprints

Use the expected SHA-256 of `blueprint.manifest` from the package author or another trusted channel. The workspace and parent directories for file outputs must already exist. The CLI can run from any current directory.

```text
brw blueprint preview kit --sha256 DIGEST --workspace project --json
brw blueprint apply kit --sha256 DIGEST --workspace project --json
```

`apply --dry-run` also previews. Each invocation verifies the manifest bytes, loads and hashes every declared file, and builds the complete plan before it executes a step. The JSON `steps` array records the operation, operands and call path. On failure, `completed` counts successful steps and `detail` identifies the failing input or call site. Completed operations retain their results.

When a `run`, `check` or `change` command fails after it starts, the JSON report sets `commandStarted` and `mayHaveSideEffects` to `true`. For example, a failed Release build can leave a partial archive alongside a completed Debug archive. Inspect the workspace, correct the cause and run the same `apply` command again. It starts at the first step, checks each desired state again, skips changes to satisfied states and repeats every unconditional build or test action it reaches.

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
| `unpack` | `payload`, `path` | Extracts a verified archive and publishes its contents as a new directory in the workspace. |
| `ssh` | `target`, `command`, optional `port` and `timeoutSeconds` | On Windows, runs one POSIX shell command on the SSH service authorized for this invocation. |

For example, declare `requires\trun` (with a tab) and list `build.cmake` as a payload beside the child module. This step runs CMake with the verified script and a single argument containing a space.

```js
{ op: "run", program: "cmake", args: ["-P", { payload: "build.cmake" }, "Release build"] }
```

The argument list accepts up to 128 entries, including at most one script reference. Digby resolves the reference from the declaring module and stages the verified bytes in a temporary file for this invocation. Scripts can use the working directory for project files. The temporary location contains only the standalone script. String arguments pass directly to the program, including empty strings and literal `|`. To execute a pipeline, select an interpreter such as `sh` with `["-c", scriptText]` or `powershell.exe` with `["-NoProfile", "-Command", scriptText]`. Child output goes to stderr so the CLI report remains readable on stdout. A failed launch or nonzero exit stops the plan.

The planning runtime has no filesystem, process or network bindings. A bundle may declare up to 128 files. The reader limits the manifest to 128 KiB, each module to 1 MiB, each payload to 64 MiB and loaded member data to 128 MiB. Nested calls, plan size, JavaScript heap, stack and execution time are also bounded. An unavailable capability, invalid import or exceeded planning limit fails before application.

## Unpack an archive into the workspace

Declare `requires\tunpack` in the manifest (with a tab). List the archive as a payload with its SHA-256. The payload path is relative to the declaring module. The output path is relative to the workspace.

```js
export default () => [
  { op: "unpack", payload: "./assets.tar.gz", path: "assets" },
  { op: "replace", path: "assets/config.txt", from: "PORT=0", to: "PORT=8080", count: 1 }
];
```

This example also requires the `replace` capability. Preview checks the archive without creating files. Apply expands it in a sibling staging directory and publishes the complete tree with one rename. The next step can then read `assets/config.txt`. An existing output directory or file produces `destination-exists`. The parent must exist when the unpack step executes. A prior unpack can supply it.

The standalone CLI uses the same Digby plan. Supply the SHA-256 of the **archive bytes** obtained from a trusted source. For example, from any current directory:

```text
brw unpack assets.tar.gz assets --sha256 DIGEST --workspace project --dry-run --json
brw unpack assets.tar.gz assets --sha256 DIGEST --workspace project --json
```

Both commands accept tar, gzip-compressed tar and ZIP. Entries must be ordinary files or directories with portable package paths. Paths differing only in ASCII case, links and repeated entries produce errors. The limits are 64 MiB of archive bytes, 256 MiB of expanded file data and 4,096 entries and paths (including implicit directories). A corrupt or out-of-bounds entry aborts publication, even if earlier files have already been staged.

For a gzip-compressed tar, Digby verifies each gzip member's CRC and uncompressed length through the end of the stream. The entire expanded tar stream, including its headers and padding, is limited to 320 MiB.

The CLI accepts UTF-8 paths. A successful JSON report includes `operation`, `payload`, `sha256` and the workspace-relative `path`. Its `status` is `planned` during preview and `applied` after publication. On an error it returns a nonzero exit code, `status: "failed"` and a stable `error` code.

Publication requires a filesystem that can atomically rename a directory while refusing replacement. On Linux, use a native filesystem such as ext4 when a mounted Windows drive in WSL reports `publish-directory`.

## Explicit SSH steps

Declare `requires\tssh` in the manifest (with a tab). This capability uses the Windows system OpenSSH client. A host without that client rejects a required SSH capability before any step runs. In the module, specify the remote user, host and command. The default port is 22 and the default timeout is 30 seconds.

```js
export default () => [
  { op: "ssh", target: "build@wsl.example", port: 2222, command: "uname -s" }
];
```

Supply the authorization, identity file and trusted host key file on the command line. Keep the two operator-managed files outside the Blueprint's output directories. Every selected SSH step must match the authorized user, host and port. A target mismatch stops the plan before any step runs.

```powershell
brw blueprint preview kit --sha256 $digest --workspace project --allow-target build@wsl.example:2222 --identity C:\keys\build --known-hosts C:\keys\wsl_hosts --json
```

Preview reports the target, command and pending host verification without opening a connection. To execute the reviewed bundle, use `apply` with the same options. Remote output goes to stderr. Each SSH result identifies the step by its Blueprint call path. A confirmed exit sets `status` to `completed` and records `remoteExit`. A nonzero exit stops the Blueprint. If the connection drops after dispatch and the exit is unconfirmed, the step reports `unknown` and `remoteExit: null`. Digby stops there without retrying. Check the remote state before applying the Blueprint again. [SSH identity and trust files](README.md#run-an-explicit-ssh-operation-on-windows)

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

## Check a host requirement

Declare the `host` capability and place the check before the steps that need it.
The example also requires the `run` capability for the build.
Here the driver utility prints its current mode. The build requires the exact
value `compute`.

```js
export default () => [
  {
    op: "host", required: "compute",
    check: {program: "driverctl", args: ["get-mode"]},
    repair: {program: "driverctl", args: ["set-mode", "compute"]}
  },
  {op: "run", program: "cmake", args: ["--build", "."]}
];
```

The read-only check prints one nonempty UTF-8 line to stdout, using the
same size limit and line endings as a named output. Exit zero and an exact
match with `required`, including case and spaces, satisfy the requirement.
The optional `unmet` list names additional exit codes that indicate a
mismatch. These must also return the detected value. Any other exit code,
a launch failure or an invalid line stops the blueprint. The command can
send diagnostics to stderr. Both `check` and `repair` can consume earlier
named outputs, use verified script payloads, or run locked tools. Declare
output producers in an unconditional `run` step.

Omit `repair` to check the host without changing it. On a mismatch, preview
and apply report `host-requirement-unmet` with the `detected` and `required`
values. With a repair, preview shows the command and its position between
the initial check and the recheck in `onMismatch`. Preview defers the check
if an earlier action could affect it. Apply runs the repair only on a
mismatch, then runs the same check again. A second mismatch reports
`recheck-unmet` with the new observation. If the recheck fails, the report
records `repairCompleted` and `mayHaveSideEffects` so the operator can
inspect changes made by the repair. Later steps start only after the check passes.

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

### Run one action with a locked version

Declare the tool before the command that uses it. Use `tool` in place of
`program` in a `run` step or in a desired state's `check` or `change` command.
The name refers to the earlier tool declaration. Declare the capabilities
used by the steps in the manifest.

```js
export default () => [
  {op: "tool", registry: "sdk", name: "builder", version: "2.*"},
  {op: "run", tool: "builder", args: ["build", "Release build"]}
];
```

The action runs the entry from the locked version's package directory.
Other versions can coexist. For example, if the user's `builder` command
points to version 1, it still launches version 1 after this action.
For an archive, the entry runs alongside the files shipped with it.
For the duration of the action, its child process can find the locked direct
and transitive dependencies by their executable names on `PATH`.
The tool step retains the location of its verified artifact for later actions
in that invocation, including after an `env` step selects another cache.

### Switch the user's default command

Declare the `default` capability and add a separate target state after the
tool declaration. This example uses the same host lock as the action above.

```js
export default () => [
  {op: "tool", registry: "sdk", name: "builder", version: "2.*"},
  {op: "default", tool: "builder"}
];
```

Preview shows the locked version and the full user command path in the
step's `entry` field. It reports whether the entry is already selected, or
defers the check if an earlier action could change it. Apply creates or
switches that entry. The path is
`builder.exe` on Windows or `builder` on Linux and macOS under
`BRW_BIN_DIR`, which defaults to the user's `.local/bin` directory.
Applying the same selection again keeps the entry pointed at that version.
Commands launched through this entry also use its locked direct and transitive
dependencies. Selecting a different dependency version for a single action
leaves the public command's selection in place until the next explicit switch.
If the selected installed program or one of its archived files has changed,
preview and apply report a conflict.
An unrelated file occupying the command path causes the switch to fail.

On Windows, a default with locked dependencies stores its private search
path in the launcher's `.target` record. Existing target-only records are
supported. Use this version of `brw` or newer to manage an entry written
with a dependency search path.

## Build and test

The [Windows MinGW-w64 devkit example](../docs/windows-devkit.md) installs a preseeded toolchain offline and runs C and C++ programs after relocating the session entry and tools together.

On Windows, prepare the pinned sources with `scripts/fetch-deps.ps1`. The MinGW-w64 build enables Blueprints by default.

On Linux and macOS, run the following commands from the repository root with a native C compiler, CMake 3.19 or newer, curl and tar. The preparation script verifies the pinned QuickJS, libgit2, libarchive and zlib archives in `.local/deps/sources` and downloads any missing archives. With the archives present, preparation, build and tests run offline.

```sh
sh scripts/fetch-blueprint-deps.sh
cmake -S brw -B .local/brw-blueprint -DBRW_BLUEPRINT=ON -DBUILD_TESTING=ON
cmake --build .local/brw-blueprint --parallel
ctest --test-dir .local/brw-blueprint -R blueprint --output-on-failure
```
