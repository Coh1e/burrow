# Red when: preview writes, rejection publishes, version switching fails, or uninstall removes persistent data.
param(
    [Parameter(Mandatory=$true)][string] $Brw,
    [Parameter(Mandatory=$true)][string] $Fixture,
    [Parameter(Mandatory=$true)][string] $Scratch
)

$ErrorActionPreference = 'Stop'
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
