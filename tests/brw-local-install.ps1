# Red when: preview writes, rejection publishes, version switching fails, or uninstall removes persistent data.
param(
    [Parameter(Mandatory=$true)][string] $Brw,
    [Parameter(Mandatory=$true)][string] $Fixture,
    [Parameter(Mandatory=$true)][string] $Gui,
    [string] $InterruptBrw,
    [Parameter(Mandatory=$true)][string] $Scratch
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'invoke-burrow.ps1')
function Assert($condition, [string] $message) {
    if (!$condition) { throw $message }
}

function Get-Sha256([string] $path) {
    $stream = [IO.File]::OpenRead($path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '')
    } finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

$root = Join-Path $Scratch ([guid]::NewGuid().ToString('N').Substring(0, 12))
$profile = Join-Path $root 'clean profile'
$inputDir = Join-Path $root 'input'
New-Item -ItemType Directory -Path $profile, $inputDir | Out-Null
Copy-Item -LiteralPath $Fixture -Destination (Join-Path $inputDir 'payload.exe')
$archive = Join-Path $root 'package.tar'
$invalid = Join-Path $root 'invalid.tar'
$tar = Join-Path $env:SystemRoot 'System32/tar.exe'
& $tar -cf $archive --format ustar -C $inputDir payload.exe
Assert ($LASTEXITCODE -eq 0) 'Cannot create local archive'
New-Item -ItemType File -Path (Join-Path $inputDir 'extra.txt') | Out-Null
& $tar -cf $invalid --format ustar -C $inputDir payload.exe extra.txt
Assert ($LASTEXITCODE -eq 0) 'Cannot create second archive'
# Make the second entry unsafe after the valid payload has been staged.
$bytes = [IO.File]::ReadAllBytes($invalid)
$offset = [int](512 + 512 * [Math]::Ceiling((Get-Item -LiteralPath $Fixture).Length / 512))
Assert ([Text.Encoding]::ASCII.GetString($bytes, $offset, 9) -eq 'extra.txt') 'Unexpected fixture header'
[Array]::Clear($bytes, $offset, 100)
[Text.Encoding]::ASCII.GetBytes('nested/../extra.txt').CopyTo($bytes, $offset)
for ($i = $offset + 148; $i -lt $offset + 156; $i++) { $bytes[$i] = 32 }
$sum = ($bytes[$offset..($offset + 511)] | Measure-Object -Sum).Sum
$checksum = [Convert]::ToString([int]$sum, 8).PadLeft(6, '0') + [char]0 + ' '
[Text.Encoding]::ASCII.GetBytes($checksum).CopyTo($bytes, ($offset + 148))
[IO.File]::WriteAllBytes($invalid, $bytes)
$sha = Get-Sha256 $archive
$unsafeSha = Get-Sha256 $invalid

$env:USERPROFILE = $profile
'XDG_CONFIG_HOME','XDG_CACHE_HOME','XDG_DATA_HOME','XDG_STATE_HOME','BRW_BIN_DIR' |
    ForEach-Object { Remove-Item -LiteralPath "Env:$_" -ErrorAction SilentlyContinue }

function Invoke-Install([string] $package, [string] $path, [string] $sha, [switch] $Preview) {
    $arguments = @('install', $package, '1.0', $path, $sha, 'payload.exe', '--json')
    if ($Preview) { $arguments += '--dry-run' }
    $output = & $Brw @arguments
    $code = $LASTEXITCODE
    [pscustomobject]@{ Code = $code; Result = ($output | ConvertFrom-Json) }
}

$preview = Invoke-Install 'local-tool' $archive $sha -Preview
Assert ($preview.Code -eq 0 -and $preview.Result.status -eq 'planned') 'Preview failed'
Assert (($preview.Result.operations -join ',') -eq 'verify,extract,publish') 'Unexpected preview plan'
Assert (@(Get-ChildItem -LiteralPath $profile -Force).Count -eq 0) 'Preview wrote to the profile'

$wrong = Invoke-Install 'wrong-digest' $archive ('0' * 64)
Assert ($wrong.Code -ne 0 -and $wrong.Result.error -eq 'digest-mismatch') 'Digest mismatch was not reported'
Assert (@(Get-ChildItem -LiteralPath $profile -Force).Count -eq 0) 'Digest mismatch wrote to the profile'

$unsafe = Invoke-Install 'unsafe-path' $invalid $unsafeSha
Assert ($unsafe.Code -ne 0 -and $unsafe.Result.error -eq 'archive-path') 'Unsafe archive path was not reported'
$packages = Join-Path $profile '.local/share/burrow/packages'
$bin = Join-Path $profile '.local/bin'
Assert (!(Test-Path -LiteralPath (Join-Path $packages 'unsafe-path/1.0'))) 'Unsafe archive published a version'
Assert (!(Test-Path -LiteralPath (Join-Path $bin 'unsafe-path.exe'))) 'Unsafe archive published a command'
Assert (!(Test-Path -LiteralPath (Join-Path $bin 'unsafe-path.exe.target'))) 'Unsafe archive published entry metadata'
Assert (@(Get-ChildItem -LiteralPath (Join-Path $packages 'unsafe-path') -Force).Count -eq 0) 'Unsafe archive left staged payload'

$installed = Invoke-Install 'local-tool' $archive $sha
Assert ($installed.Code -eq 0 -and $installed.Result.status -eq 'installed') 'Install failed'
Assert (($installed.Result.operations -join ',') -eq ($preview.Result.operations -join ',')) 'Preview and install plans differ'
Assert (Test-Path -LiteralPath (Join-Path $packages 'local-tool/1.0/payload.exe')) 'Version payload missing'
$shim = Join-Path $bin 'local-tool.exe'
$output = & $shim 'two words'
Assert ($LASTEXITCODE -eq 37 -and $output -eq 'two words') 'Command entry did not preserve argument and exit code'

function Invoke-Probe($alias, $arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $alias
    $info.Arguments = $arguments
    $info.WorkingDirectory = $root
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    Invoke-ProcessCapture $info -InputLine 'input'
}
New-Item -ItemType File -Path (Join-Path $root 'cwd.marker') | Out-Null
$probe = Invoke-Probe $shim ('--probe "" "' + [char]0x706b + ' star" "a\"b" "ends \\" last')
Assert ($probe.Code -eq 41 -and $probe.out.TrimEnd() -ceq 'output' -and $probe.err.TrimEnd() -ceq 'diagnostic') 'Native argv, cwd, or stream forwarding failed'

$again = Invoke-Install 'local-tool' $archive $sha
Assert ($again.Code -ne 0 -and $again.Result.error -eq 'destination-exists') 'Existing version is not protected'

function Invoke-Lifecycle([string[]] $arguments) {
    $output = & $Brw @arguments --json
    $code = $LASTEXITCODE
    [pscustomobject]@{ Code = $code; Result = ($output | ConvertFrom-Json) }
}

function Assert-Active([string] $entry) {
    $output = & $shim 'two words'
    Assert ($LASTEXITCODE -eq 37 -and $output -eq 'two words') 'Active entry is not runnable'
    $bytes = [IO.File]::ReadAllBytes("$shim.target")
    $target = [Text.Encoding]::Unicode.GetString($bytes).TrimEnd([char]0)
    Assert ($target -eq (Join-Path $packages "local-tool/$entry")) 'Unexpected active version or entry'
    Assert ((Get-Content -LiteralPath $data) -eq 'keep this') 'Persistent data changed'
}

$dataDir = Join-Path $profile '.local/share/burrow/persist/local-tool'
New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
$data = Join-Path $dataDir 'user.txt'
[IO.File]::WriteAllText($data, 'keep this')
Assert-Active '1.0/payload.exe'

# The first brw installer wrote only the active sidecar. Exercise that layout.
foreach ($record in '.brw-entry', '.brw-files', '.brw-shim') {
    Remove-Item -LiteralPath (Join-Path $packages "local-tool/1.0/$record")
}
$failed = Invoke-Lifecycle @('update', 'local-tool', '2.0', $invalid, $unsafeSha, 'payload.exe')
Assert ($failed.Code -ne 0 -and $failed.Result.error -eq 'archive-path') 'Update did not reject invalid archive'
Assert (!(Test-Path -LiteralPath (Join-Path $packages 'local-tool/2.0'))) 'Failed update published a version'
Assert-Active '1.0/payload.exe'

$archive2 = Join-Path $root 'package2.tar'
Copy-Item -LiteralPath $Fixture -Destination (Join-Path $inputDir 'replacement.exe')
& $tar -cf $archive2 --format ustar -C $inputDir replacement.exe
Assert ($LASTEXITCODE -eq 0) 'Cannot create second version'
$updateArgs = @('update', 'local-tool', '2.0', $archive2, (Get-Sha256 $archive2), 'replacement.exe')
$sidecarLock = [IO.File]::Open("$shim.target", 'Open', 'Read', 'Read')
try {
    $blocked = Invoke-Lifecycle $updateArgs
    Assert ($blocked.Code -ne 0 -and $blocked.Result.error -eq 'publish-entry') 'Update overwrote a locked entry'
} finally {
    $sidecarLock.Dispose()
}
Assert (!(Test-Path -LiteralPath (Join-Path $packages 'local-tool/2.0'))) 'Failed entry switch left a published version'
Assert-Active '1.0/payload.exe'
$updated = Invoke-Lifecycle $updateArgs
Assert ($updated.Code -eq 0 -and $updated.Result.status -eq 'updated') 'Update failed'
Assert-Active '2.0/replacement.exe'

$rolledBack = Invoke-Lifecycle @('rollback', 'local-tool', '1.0')
Assert ($rolledBack.Code -eq 0 -and $rolledBack.Result.status -eq 'rolled-back') 'Rollback failed'
Assert-Active '1.0/payload.exe'
Assert (Test-Path -LiteralPath (Join-Path $packages 'local-tool/2.0/replacement.exe')) 'Rollback removed retained version'
$unknown = Invoke-Lifecycle @('rollback', 'local-tool', '9.0')
Assert ($unknown.Code -ne 0 -and $unknown.Result.error -eq 'version-not-installed') 'Rollback accepted missing version'
Assert-Active '1.0/payload.exe'

$previewRemoval = Invoke-Lifecycle @('uninstall', 'local-tool', '--dry-run')
Assert ($previewRemoval.Code -eq 0 -and $previewRemoval.Result.status -eq 'planned') 'Uninstall preview failed'
Assert-Active '1.0/payload.exe'
$foreign = Join-Path $packages 'local-tool/2.0/notes.txt'
[IO.File]::WriteAllText($foreign, 'user file')
$conflict = Invoke-Lifecycle @('uninstall', 'local-tool')
Assert ($conflict.Code -ne 0 -and $conflict.Result.error -eq 'package-conflict') 'Uninstall accepted an unowned file'
Assert ((Get-Content -LiteralPath $foreign) -eq 'user file') 'Uninstall altered an unowned file'
Assert-Active '1.0/payload.exe'
Remove-Item -LiteralPath $foreign
$payloadLock = [IO.File]::Open((Join-Path $packages 'local-tool/2.0/replacement.exe'), 'Open', 'Read', 'Read')
try {
    $lockedRemoval = Invoke-Lifecycle @('uninstall', 'local-tool')
    Assert ($lockedRemoval.Code -ne 0 -and $lockedRemoval.Result.error -eq 'package-conflict') 'Uninstall accepted a locked payload'
    Assert-Active '1.0/payload.exe'
} finally {
    $payloadLock.Dispose()
}
$removed = Invoke-Lifecycle @('uninstall', 'LOCAL-TOOL')
Assert ($removed.Code -eq 0 -and $removed.Result.status -eq 'uninstalled') 'Uninstall failed'
Assert (!(Test-Path -LiteralPath $shim) -and !(Test-Path -LiteralPath "$shim.target")) 'Uninstall left an entry'
Assert (!(Test-Path -LiteralPath (Join-Path $packages 'local-tool'))) 'Uninstall left installed versions'
Assert ((Get-Content -LiteralPath $data) -eq 'keep this') 'Uninstall removed persistent data'

$longEntry = ('x' * 123) + '.exe'
$longArchive = Join-Path $root 'long-entry.tar'
Copy-Item -LiteralPath $Fixture -Destination (Join-Path $inputDir $longEntry)
& $tar -cf $longArchive --format pax -C $inputDir $longEntry
Assert ($LASTEXITCODE -eq 0) 'Cannot create maximum-length entry archive'
# Keep the profile root short to isolate record length from the Win32 path limit.
$env:USERPROFILE = $root
$longInstall = Invoke-Lifecycle @('install', 'long-tool', '1.0', $longArchive, (Get-Sha256 $longArchive), $longEntry)
Assert ($longInstall.Code -eq 0) "Maximum-length entry did not install: $($longInstall.Result.error)"
$longRemove = Invoke-Lifecycle @('uninstall', 'long-tool')
Assert ($longRemove.Code -eq 0) "Maximum-length entry uninstall failed: $($longRemove.Result.error)"

foreach ($extension in 'cmd', 'bat', 'ps1') {
    $entry = "script.$extension"
    $body = if ($extension -eq 'ps1') {
        @'
if ($args[0] -eq '--probe') {
    if ($args.Count -ne 6 -or $args[1] -cne '' -or $args[2] -cne $env:BRW_TEST_WORD -or
        $args[3] -cne 'a"b' -or $args[4] -cne 'ends \' -or $args[5] -cne 'last') { exit 8 }
    if (!(Test-Path -LiteralPath 'cwd.marker') -or [Console]::ReadLine() -cne 'input') { exit 9 }
    [Console]::Error.WriteLine('diagnostic')
    [Console]::WriteLine('output')
    exit 41
}
[Console]::WriteLine($args[0])
exit 43
'@
    } else {
        @'
@echo off
if "%~1"=="--probe" goto probe
echo %~1
exit /b 43
:probe
if not [%2]==[""] exit /b 8
if not "%~3"=="%BRW_TEST_WORD%" exit /b 8
if not "%~4"=="ends \" exit /b 8
if not "%~5"=="last" exit /b 8
if not exist cwd.marker exit /b 9
set /p line=
if not "%line%"=="input" exit /b 9
echo diagnostic 1>&2
echo output
exit /b 41
'@
    }
    Set-Content -LiteralPath (Join-Path $inputDir $entry) -Value $body -Encoding ASCII
    $bundle = Join-Path $root "$extension.tar"
    & $tar -cf $bundle --format ustar -C $inputDir $entry
    $installed = Invoke-Lifecycle @('install', "test-$extension", '1', $bundle, (Get-Sha256 $bundle), $entry)
    Assert ($installed.Code -eq 0) "$extension install failed: $($installed.Result.error)"
    $alias = Join-Path $root ".local/bin/test-$extension.exe"
    $output = & $alias 'two words'
    Assert ($LASTEXITCODE -eq 43 -and $output -ceq 'two words') "$extension invocation failed"
    $env:BRW_TEST_WORD = [string][char]0x706b + ' star'
    $argsText = if ($extension -eq 'ps1') {
        '--probe "" "' + $env:BRW_TEST_WORD + '" "a\"b" "ends \\" last'
    } else {
        '--probe "" "' + $env:BRW_TEST_WORD + '" "ends \\" last'
    }
    if ($extension -eq 'ps1') {
        $previousPolicy = $env:PSExecutionPolicyPreference
        try {
            $env:PSExecutionPolicyPreference = 'Restricted'
            $denied = Invoke-Probe $alias 'two'
            Assert ($denied.code -ne 43 -and $denied.code -ne 0 -and $denied.err) 'Entry bypassed Restricted execution policy'
        } finally {
            $env:PSExecutionPolicyPreference = $previousPolicy
        }
    }
    $scriptProbe = Invoke-Probe $alias $argsText
    Assert ($scriptProbe.code -eq 41 -and $scriptProbe.out.TrimEnd() -ceq 'output' -and
        $scriptProbe.err.TrimEnd() -ceq 'diagnostic') "$extension argv, cwd, or stream forwarding failed (exit $($scriptProbe.code))"
}

# Launch through the Windows shell verb used by Explorer. A console shim could
# flash its own window even when the GUI child itself has no console.
Copy-Item -LiteralPath $Gui -Destination (Join-Path $inputDir 'window.exe')
$guiArchive = Join-Path $root 'window.tar'
& $tar -cf $guiArchive --format ustar -C $inputDir window.exe
$guiUpdate = @('update', 'test-cmd', '2', $guiArchive, (Get-Sha256 $guiArchive), 'window.exe')
$alias = Join-Path $root '.local/bin/test-cmd.exe'
$aliasLock = [IO.File]::Open($alias, 'Open', 'Read', 'Read')
try {
    $blocked = Invoke-Lifecycle $guiUpdate
    Assert ($blocked.Result.error -eq 'publish-entry') 'Locked launcher replacement was not rejected'
    $output = & $alias 'still active'
    Assert ($LASTEXITCODE -eq 43 -and $output -eq 'still active') 'Failed GUI update switched the target'
    Assert (!(Test-Path (Join-Path $root '.local/share/burrow/packages/test-cmd/2'))) 'Failed GUI update left a version'
} finally {
    $aliasLock.Dispose()
}
$updated = Invoke-Lifecycle $guiUpdate
Assert ($updated.Code -eq 0) 'Update from batch to GUI failed'
$pe = [IO.File]::ReadAllBytes($alias)
$nt = [BitConverter]::ToInt32($pe, 60)
Assert ([BitConverter]::ToUInt16($pe, $nt + 24 + 68) -eq 2) 'GUI entry would open a console'
$marker = Join-Path $root 'shell marker.txt'
$info = New-Object Diagnostics.ProcessStartInfo
$info.FileName = $alias
$info.UseShellExecute = $true
$info.Verb = 'open'
$info.Arguments = '"' + $marker + '"'
$process = [Diagnostics.Process]::Start($info)
Assert ($process.WaitForExit(10000) -and $process.ExitCode -eq 0) 'Shell launch failed'
Assert ((Get-Content -LiteralPath $marker) -ceq 'gui') 'GUI target had a console'
$rolledBack = Invoke-Lifecycle @('rollback', 'test-cmd', '1')
Assert ($rolledBack.Code -eq 0) 'Rollback to batch failed'
$pe = [IO.File]::ReadAllBytes($alias)
$nt = [BitConverter]::ToInt32($pe, 60)
Assert ([BitConverter]::ToUInt16($pe, $nt + 24 + 68) -eq 3) 'Rollback left a GUI launcher'
$env:Path = (Split-Path -Parent $alias) + ';' + $env:Path
$output = & $env:ComSpec /d /c 'test-cmd.exe "two words"'
Assert ($LASTEXITCODE -eq 43 -and $output -ceq 'two words') 'cmd host launch after rollback failed'

$unsafe = Invoke-Probe $alias '"hello%PATH%"'
Assert ($unsafe.Code -eq 126 -and $unsafe.err -match 'cannot safely forward' -and !$unsafe.out) 'Unsafe batch argument lacked a diagnostic'

# Simulate an abrupt exit after publishing the new version, before changing its entry.
$env:BRW_TEST_INTERRUPT = 'version'
try {
    $null = & $InterruptBrw update test-cmd 3 $archive $sha payload.exe --json
    Assert ($LASTEXITCODE -eq 86) 'Publication did not stop at the version boundary'
} finally { Remove-Item Env:BRW_TEST_INTERRUPT }
foreach ($pass in 1..2) {
    $result = & $Brw recover --json | ConvertFrom-Json
    Assert ($LASTEXITCODE -eq 0) 'Version recovery failed'
    Assert (!(Test-Path -LiteralPath (Join-Path $root '.local/share/burrow/packages/test-cmd/3'))) 'Interrupted version still installed'
    $output = & $alias 'two words'
    Assert ($LASTEXITCODE -eq 43 -and $output -ceq 'two words') 'Recovery lost the previous command'
}
