# brw native package sources

Build and test on Windows with MinGW-w64, CMake 3.25+, and Windows PowerShell 5.1. Run these commands from the repository root with the MinGW-w64 tools on PATH.

~~~powershell
./scripts/fetch-deps.ps1
cmake -S brw -B .local/brw-build -G "MinGW Makefiles"
cmake --build .local/brw-build --parallel
ctest --test-dir .local/brw-build --output-on-failure
~~~

## Register and use a source

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
| Entry | `.exe` path to publish as the package's command. Archive entries can use a relative path such as `bin/tool.exe` |
| Depends | Comma-separated package names, such as `runtime,tools`. An omitted or empty field declares no dependencies. |

Dependency names are case-insensitive. An active installed package satisfies its name. Otherwise the name must resolve to a single catalog row. Dry-run lists the packages that would be installed in dependency order. Missing, ambiguous, or cyclic dependencies stop planning before any payload is installed. Each package is committed separately, so an error in a later package leaves earlier installations available.

Uninstall checks the dependency records saved with active versions, including when their source has been removed. A local update retains the active version's declarations. Rollback checks that the selected version's dependencies are installed.

Use package names, versions, local filenames, and entry names made up of ASCII letters, digits, dots, hyphens, or underscores. A catalog should provide one row per package name. Archive payloads support TAR, gzip-compressed TAR, and ZIP with ordinary files and directories. Use forward slashes between path components. The extractor rejects links, absolute paths, and traversal components. Install uses the existing brw package and command-entry layout. `brw list` shows active packages from source and local installs. All commands shown above accept `--json`. Source add and remove also accept `--dry-run`.

Dry-run reads the local catalog and shows the Digby plan. Install reads the catalog again, downloads to a temporary file, verifies SHA-256, stages the payload, and publishes the package and entry through the local install path. A catalog supplies the expected digest and serves as the trust boundary. HTTP redirects are disabled. Ctrl+C or Ctrl+Break requests cancellation up to the start of publication. An in-flight HTTP read may take until the receive timeout to return. A rejected digest or cancelled download reports the failed step in JSON and leaves no installed version or command entry.
