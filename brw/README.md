# brw native package sources

Build and test on Windows with MinGW-w64, CMake 3.25+, and Windows PowerShell 5.1. Run these commands from the repository root with the MinGW-w64 tools on PATH.

~~~powershell
./scripts/fetch-deps.ps1
cmake -S brw -B .local/brw-build -G "MinGW Makefiles"
cmake --build .local/brw-build --parallel
ctest --test-dir .local/brw-build --output-on-failure
~~~

### Shared Digby tests

The [CI workflow](../.github/workflows/ci.yml) builds the same C99 core on Windows
with MinGW-w64, Linux with GCC and macOS with Apple Clang. The C-only build sets
`CXX` to a nonexistent compiler and checks that CMake selected no C++ compiler.
Enable `BRW_BLUEPRINT` and `BUILD_TESTING` on all hosts and `BRW_TEST_GIT` on
POSIX to run the following CTest contracts.

| CTest name | Observable behavior |
| --- | --- |
| `digby_plan`, `blueprint_plan` | Plan order, completed steps, failure location and missing capabilities before writes |
| `digby_literal`, `blueprint_cli` | UTF-8 byte offsets, file copy and text replacement, archived tool and sibling contents, and preservation of the previous command when an archive lacks its declared entry |
| `git_worktree` | Actual commits after clone and fast-forward, refusal of dirty or diverged worktrees |

In a separate build directory, configure with `-DBRW_ENABLE_CPP_MODULE=ON`,
CMake 3.25 or newer and a C++23 compiler. The optional module formats literal search offsets into
caller-owned storage. Run `ctest --test-dir <build> -R "^digby_(literal|cpp_abi)$" --output-on-failure`
after building `literal_test` and `literal_offset_test`. Both tests use C
callers. They check search output, exact capacity, an undersized or null
buffer, and the largest representable offset. [C ABI contract](literal_offset.h)

#### Verification on native hosts

The earlier Blueprint and Git worktree runs covered separate revisions.
The Linux C ABI prototype predated the optional module in this build.
[Blueprint checklist](../docs/blueprint-acceptance.md) · [Git worktree runs](https://github.com/Coh1e/burrow/issues/129#issuecomment-7149019218) · [Linux C ABI prototype](https://github.com/Coh1e/burrow/issues/115#issuecomment-7100338609)

The 2026-11-27 CI run used source revision `ec11dd2` for both Release
configurations. Each host built with Ninja, ran the shared contracts with
the module disabled, then ran `digby_literal` and `digby_cpp_abi` with
the module enabled in a fresh build directory. The job logs contain the
configure, build and test commands. [CI run](https://github.com/Coh1e/burrow/actions/runs/45936327468)

| Host | Native compilers | Execution record |
| --- | --- | --- |
| Windows Server 2025 x64 | MinGW-w64 GCC/G++ 16.2 | [Windows job](https://github.com/Coh1e/burrow/actions/runs/45936327468/job/140300264003) |
| Ubuntu 24.04 x86-64 | GCC/G++ 13.3 | [Linux job](https://github.com/Coh1e/burrow/actions/runs/45936327468/job/140300263946) |
| macOS 15 arm64 | Apple Clang 17 | [macOS job and APFS checks](https://github.com/Coh1e/burrow/actions/runs/45936327468/job/140300263973) |

Two local runs supplement the hosted checks for the same source revision.
On Windows 11 x64, MinGW-w64 GCC/G++ 16.2 built the Release Blueprint with
the module enabled. `ctest --test-dir .local/133-win-on --output-on-failure`
passed all 16 tests, including cross-volume recovery. [Windows transcript](../docs/history/digby-133/windows-ctest.txt)

The hosted Ubuntu runner skipped the private ramfs mount and 9p case-metadata
tests. On Ubuntu 26.04 under WSL2, both ran and passed with
`ctest --test-dir .local/133-linux-c -V -R "^blueprint_cli_(ramfs|unknown_case)$"`.
[WSL filesystem transcript](../docs/history/digby-133/wsl-filesystems.txt)

## Register and use a source

For verified offline bundles that combine package installation with file, text and session environment operations, see [Offline Blueprints](blueprint.md).

~~~text
brw source add NAME CATALOG
brw source list
brw source remove NAME
brw search [QUERY]
brw show [SOURCE/]NAME
brw install [SOURCE/]NAME [--dry-run] [--json]
brw list
~~~

A source registers an absolute reference to a local catalog. brw reads the catalog again on each query and install. The registration lives under the user's `XDG_CONFIG_HOME/burrow/sources`, or `%USERPROFILE%/.config/burrow/sources` by default. Removing a registration leaves the catalog, its payloads, and installed packages in place. Search matches a substring of the package name case-insensitively. Qualify the package name with its source if more than one source provides it.

Catalogs are UTF-8 without a BOM. The first line is `brw-catalog-v1` and each following line contains six required tab-separated fields, followed by an optional seventh field.

| Field | Value |
| --- | --- |
| Name | Package name and command alias |
| Version | Version directory name |
| Kind | `exe` or `archive` |
| Location | Local file in the catalog directory, or an HTTPS or HTTP URL |
| SHA-256 | 64 hexadecimal digits |
| Entry | Path to a Windows console or GUI `.exe`, `.cmd`, `.bat`, or `.ps1` to publish as the package's command. Archive entries can use a relative path such as `bin/tool.exe` |
| Depends | Comma-separated package names, such as `runtime,tools`. An omitted or empty field declares no dependencies. |

Batch entries quote each argument and reject shell metacharacters in the target path or arguments with a diagnostic and exit code 126. PowerShell entries use Windows PowerShell with the current execution policy.

Dependency names are case-insensitive. An active installed package satisfies its name. Otherwise the name must resolve to a single catalog row. Dry-run lists the packages that would be installed in dependency order. Missing, ambiguous, or cyclic dependencies stop planning before any payload is installed. Each package is committed separately, so an error in a later package leaves earlier installations available.

Uninstall checks the dependency records saved with active versions, including when their source has been removed. A local update retains the active version's declarations. Rollback checks that the selected version's dependencies are installed.

Use package names, versions, local filenames, and entry names made up of ASCII letters, digits, dots, hyphens, or underscores. A catalog should provide one row per package name. Archive payloads support TAR, gzip-compressed TAR, and ZIP with ordinary files and directories. Use forward slashes between path components. The extractor rejects links, absolute paths, and traversal components. Install uses the existing brw package and command-entry layout. `brw list` shows active packages from source and local installs. All commands shown above accept `--json`. Source add and remove also accept `--dry-run`.

Dry-run reads the local catalog and shows the Digby plan. Install reads the catalog again, downloads to a temporary file, verifies SHA-256, stages the payload, and publishes the package and entry through the local install path. A catalog supplies the expected digest and serves as the trust boundary. HTTP redirects are disabled. Ctrl+C or Ctrl+Break requests cancellation up to the start of publication. An in-flight HTTP read may take until the receive timeout to return. A rejected digest or cancelled download reports the failed step in JSON and leaves no installed version or command entry.

## Design and implementation plan

The following work extends the [first-release specification](https://github.com/Coh1e/burrow/issues/119). Record the results against the [release candidate](https://github.com/Coh1e/burrow/issues/134).

1. **Use one Digby plan for preview and execution.** For each invocation, resolve dependencies and build the ordered package and operation plan once. Text and JSON preview traverse that plan. Execution passes its steps to the backend in order and stops at the first failure. A later invocation reads the source and installed state again and rechecks preconditions before writing. Preview performs no writes or downloads and identifies conditions that still require remote verification. Start by removing the operation list duplicated in the dependency JSON preview. With the same local inputs, compare preview package and operation order with a recording backend for a single package and a dependency chain. Inject a failed step and check the executed prefix and reported failure. Change the source or installed state between invocations and check that execution honors the new state.

2. **Normalize each native catalog row into a native package description.** At the catalog boundary, decode the source identity and the seven fields documented above into named C fields, with a typed payload kind and a list of direct dependency names. Use this description in search, show, dependency resolution and install planning. Keep installed-state decisions and operation ordering in the planner. Preserve the catalog format and CLI output. Test both six- and seven-field rows, empty dependencies, qualified names and invalid field values through the reader and its consumers.

3. **Measure query latency before choosing an index or a target.** Benchmark the release build of `brw search`, `brw show` and `brw list` as separate processes against reproducible local fixtures. Vary catalog size for search/show and installed-package count for list. Include matching and missing lookups. Record the first run separately from repeated runs, report sample counts and warm p50/p95, and document cache preparation (an ordinary first run does not establish a cold filesystem cache). Retain the commands, fixture sizes, revision, build options, OS, CPU, storage and timings with the candidate evidence. Use the first baseline to decide whether a latency target or an index is warranted.

## Directories and user PATH

| Purpose | Default under `%USERPROFILE%` | Override |
| --- | --- | --- |
| Source configuration | `.config/burrow` | `XDG_CONFIG_HOME` |
| Download staging | `.cache/burrow` | `XDG_CACHE_HOME` |
| Package payloads and persistent data | `.local/share/burrow` | `XDG_DATA_HOME` |
| Installation records | `.local/state/burrow` | `XDG_STATE_HOME` |
| Command entries | `.local/bin` | `BRW_BIN_DIR` |

Each absolute XDG override is a base directory. brw appends `burrow` to it. `BRW_BIN_DIR` selects the entry directory directly. Empty and relative values use the defaults. Unusable absolute paths produce directory-specific errors. Changes apply to subsequent commands and leave existing files where they were.

`brw init --dry-run --json` shows the selected paths and whether the entry directory would be appended to the user PATH. Run `brw init` to create the entry directory and add it to the current user's persistent PATH. Running it again keeps the existing entry. brw preserves existing PATH text and the registry value type, then notifies Windows when it changes the value. Existing shells keep their current environment.

New versions have installation records in the state directory. The files used for version ownership, dependency protection, and shim launch stay with their existing version and command entry. Installations created with an earlier brw remain usable.

If installation, update or rollback stops during publication, run `brw recover --dry-run --json` with the original data, state and entry directories. Run `brw recover` to undo an uncommitted publication. A first install returns to the uninstalled state. An update or rollback restores the previous command. If the transaction already committed, recovery finishes cleaning up its private files. Recovery can itself be interrupted and run again. A pending journal blocks package changes in that state directory. An ownership conflict leaves the transaction pending and preserves the changed object.
