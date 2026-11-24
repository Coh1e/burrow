# Windows settings

Use the registry commands for two Burrow settings under `HKCU\Software\Burrow\Settings`. The names are case-insensitive. `Profile` holds a `REG_SZ` string. `Enabled` holds a `REG_DWORD` supplied as an unsigned decimal number from 0 to 4294967295.

~~~powershell
$key = 'HKCU\Software\Burrow\Settings'
brw registry get $key Profile REG_SZ --json
brw registry set $key Profile REG_SZ work --expect-missing --dry-run --json
brw registry set $key Profile REG_SZ work --expect-missing
brw registry set $key Profile REG_SZ personal --expect work
brw registry remove $key Profile REG_SZ --expect personal
brw registry set $key Enabled REG_DWORD 1 --expect-missing
~~~

A registry write or deletion requires the expected old value or an explicit expectation that the value is missing. A missing value reads as JSON null. An empty string is a present value. The preview shows the key, name, type, old value and proposed value. Execution checks authorization, type and the old value again.

## Environment and PATH

~~~powershell
brw env user set BRW_PROFILE work --expect-missing
brw env user remove BRW_PROFILE --expect work
brw env user path-add 'C:\Tools\bin' --dry-run --json
brw env user path-remove 'C:\Tools\bin'
brw env session set BRW_PROFILE temporary -- cmd.exe /d /c set BRW_PROFILE
~~~

Both scopes support set, remove, path-add and path-remove. Environment preconditions are optional. PATH operations take one absolute entry, compare entries case-insensitively, preserve unrelated entries and retain the registry string type. User scope writes the current user's persistent environment and broadcasts the change. Existing processes retain their own environment.

Session scope takes a child executable and arguments after `--`. Only the brw process and the child process tree inherit the change. The command returns the child's exit code. Place `--dry-run` or `--json` before the separator. Preview leaves the child unstarted. In JSON mode the child's output goes to stderr and the result on stdout includes its exit code.

## Interrupted writes

Persistent operations write a journal in the selected Burrow state directory. With the same directory settings, run `brw recover --dry-run --json` to preview recovery, then `brw recover --json` to apply it. Recovery restores the exact previous value and type when the current value still matches the interrupted write. It reports `recovered-with-user-changes` and retains a later edit. A subsequent recovery reports `no-pending`.


## Windows verification

The default CTest suite runs the settings CLI in a private registry hive. To exercise the Windows Kernel Transaction Manager, configure the build with `-DBRW_TEST_USER_REGISTRY=ON` and run `ctest --test-dir <build> -R "^brw_settings_user_registry$" --output-on-failure` as an ordinary user. This opt-in test creates a randomly named `HKCU\Software\Burrow-Tests-<id>` subtree, puts both the settings key and the fixture's Environment key inside it, and removes the subtree on exit.
