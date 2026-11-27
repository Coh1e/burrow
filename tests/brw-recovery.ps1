# Red when: recovery overwrites an edited object, confuses same-name entries, or cannot resume an interrupted undo.
param([string] $Brw, [string] $InterruptBrw, [string] $Fixture, [string] $Scratch, [string] $Layout)
$ErrorActionPreference = 'Stop'

function Assert($condition, [string] $message) {
    if (!$condition) { throw $message }
}
function Run([int] $expected, [string[]] $arguments) {
    $output = & $Brw @arguments --json
    Assert ($LASTEXITCODE -eq $expected) "Unexpected exit $LASTEXITCODE for $arguments ($output)"
    Write-Host "brw $($arguments[0]) exit=$expected $output"
    if ($output) { $output | ConvertFrom-Json }
}
function Interrupt([string] $phase, [string[]] $arguments) {
    $env:BRW_TEST_INTERRUPT = $phase
    try {
        $null = & $InterruptBrw @arguments --json
        Assert ($LASTEXITCODE -eq 86) "Failed to interrupt $arguments at $phase"
        $journal = Join-Path $state 'publication-v1'
        if (Test-Path -LiteralPath $journal -PathType Leaf) {
            $file = Get-Item -LiteralPath $journal
            $hash = (Get-FileHash -LiteralPath $journal -Algorithm SHA256).Hash
            Write-Host "interrupt=$phase exit=86 journal=publication-v1 bytes=$($file.Length) sha256=$hash"
        }
    } finally { Remove-Item Env:BRW_TEST_INTERRUPT }
}

$id = [guid]::NewGuid().ToString('N').Substring(0,12)
$root = Join-Path $Scratch $id
$env:USERPROFILE = $root
$env:XDG_DATA_HOME = Join-Path $root 'data'
$stateBase = $root
if ($Layout -eq 'cross_volume') {
    $stateBase = [IO.Path]::GetTempPath()
    $dataVolume = & "$env:SystemRoot/System32/mountvol.exe" ([IO.Path]::GetPathRoot($root)) /L
    $stateVolume = & "$env:SystemRoot/System32/mountvol.exe" ([IO.Path]::GetPathRoot($stateBase)) /L
    if (($dataVolume -join '').Trim() -notmatch 'Volume\{' -or
        ($stateVolume -join '').Trim() -notmatch 'Volume\{' -or
        ($dataVolume -join '').Trim() -eq ($stateVolume -join '').Trim()) {
        Write-Host 'Cross-volume recovery requires TEMP and the build directory on distinct volumes.'
        exit 77
    }
}
$env:XDG_STATE_HOME = Join-Path $stateBase "brw-$id"
$env:BRW_BIN_DIR = Join-Path $env:XDG_STATE_HOME 'bin'
New-Item -ItemType Directory -Path $root | Out-Null
$archive = Join-Path $root 'payload.tar'
& "$env:SystemRoot/System32/tar.exe" -cf $archive --format ustar -C (Split-Path $Fixture) (Split-Path -Leaf $Fixture)
Assert ($LASTEXITCODE -eq 0) 'Cannot create archive'
$entry = Split-Path -Leaf $Fixture
$sha = [Security.Cryptography.SHA256]::Create()
try { $digest = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($archive))).Replace('-', '') }
finally { $sha.Dispose() }
$install = @('install','sample','1',$archive,$digest,$entry)
$update = @('update','sample','2',$archive,$digest,$entry)
$alias = Join-Path $env:BRW_BIN_DIR 'sample.exe'
$state = Join-Path $env:XDG_STATE_HOME 'burrow'
$version = Join-Path $env:XDG_DATA_HOME 'burrow/packages/sample/2'
Write-Host "Recovery data volume $([IO.Path]::GetPathRoot($root)), state volume $([IO.Path]::GetPathRoot($state))"

Interrupt version $install
[IO.File]::WriteAllText($alias, 'another command')
Assert ((Run 1 @('recover')).error -eq 'entry-conflict') 'Same-name command lacked an ownership conflict'
Assert ([IO.File]::ReadAllText($alias) -ceq 'another command') 'Recovery changed another command'
[IO.File]::Delete($alias)
$temporary = (Get-ChildItem $env:BRW_BIN_DIR -Filter '.stage-*-target').FullName
$locked = [IO.File]::Open($temporary, 'Open', 'Read', 'Read')
try { Assert ((Run 1 @('recover')).error -eq 'publication-cleanup') 'Locked cleanup did not report failure' }
finally { $locked.Dispose() }
Assert ((Run 0 @('recover')).status -eq 'recovered') 'First install could not finish cleanup'
Assert (!(Test-Path $temporary)) 'Retry left the staged target'
Assert (@((Run 0 @('list')).packages).Count -eq 0) 'Undone first install was still listed'

Interrupt version $install
Interrupt cleanup @('recover')
$packages = Join-Path $env:XDG_DATA_HOME 'burrow/packages'
$staged = [IO.Directory]::GetDirectories($packages, '.stage-*')
Assert ($staged.Count -eq 1) 'Expected one detached version'
$note = Join-Path $staged[0] 'user-note'
[IO.File]::WriteAllText($note, 'keep me')
foreach ($argsForRecovery in @(@('recover','--dry-run'),@('recover'))) {
    Assert ((Run 1 $argsForRecovery).error -eq 'publication-conflict') 'Cleanup missed an added child'
    Assert (Test-Path (Join-Path $staged[0] $entry)) 'Conflicting cleanup removed another file'
}
Assert ([IO.File]::ReadAllText($note) -ceq 'keep me') 'Cleanup changed the added file'
[IO.File]::Delete($note)
Assert ((Run 0 @('recover')).status -eq 'recovered') 'Cleanup could not resume after conflict resolution'

Interrupt entry $install
$bytes = [IO.File]::ReadAllBytes($alias)
[IO.File]::AppendAllText($alias, 'user edit')
Assert ((Run 1 @('recover','--dry-run')).error -eq 'entry-conflict') 'Preview missed changed shim'
Assert ((Run 1 @('recover')).error -eq 'entry-conflict') 'Recovery missed changed shim'
Assert ((Get-Item $alias).Length -gt $bytes.Length) 'Recovery removed user edit'
[IO.File]::WriteAllBytes($alias, $bytes)
[IO.File]::Move($alias, "$alias.original")
[IO.File]::WriteAllBytes($alias, $bytes)
Assert ((Run 1 @('recover')).error -eq 'entry-conflict') 'A byte-identical replacement was treated as owned'
Assert (Test-Path $alias) 'Recovery removed the replacement command'
[IO.File]::Delete($alias)
[IO.File]::Move("$alias.original", $alias)
if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($alias)).DriveFormat -eq 'NTFS') {
    Set-Content -LiteralPath $alias -Stream 'user-note' -Value 'keep me'
    Assert ((Run 1 @('recover')).error -eq 'entry-conflict') 'Recovery missed an added file stream'
    Assert ((Get-Content -LiteralPath $alias -Stream 'user-note') -ceq 'keep me') 'Recovery lost the added stream'
    Remove-Item -LiteralPath $alias -Stream 'user-note'
}
Interrupt recover @('recover')
Assert ((Run 0 @('recover')).status -eq 'recovered') 'Interrupted recovery could not resume'
Assert ((Run 0 @('recover')).status -eq 'no-pending') 'Second recovery found a pending transaction'

$null = Run 0 $install
Interrupt target $update
$payload = Join-Path $version $entry
$original = [IO.File]::ReadAllBytes($payload)
[IO.File]::AppendAllText($payload, 'keep me')
Assert ((Run 1 @('recover')).error -eq 'publication-conflict') 'Edited package file lost ownership protection'
Assert ((Get-Item $payload).Length -gt $original.Length) 'Recovery removed the edited package file'
[IO.File]::WriteAllBytes($payload, $original)
Assert ((Run 1 @('uninstall','sample')).error -eq 'recovery-required') 'Another mutation ignored pending recovery'
Assert ((Run 0 @('recover','--dry-run')).status -eq 'recoverable') 'Preview could not identify pending update'
Assert ((Test-Path $payload) -and @((Get-ChildItem $state -Filter 'install-*')).Count -eq 2) 'Preview changed install facts'
Assert ((Run 0 @('recover')).status -eq 'recovered') 'Update recovery failed'
Assert ((Run 0 @('list')).packages[0].version -eq '1') 'Previous version was not active'
Assert (!(Test-Path $version) -and @(Get-ChildItem $state -Filter 'install-*').Count -eq 1) 'Update receipt or payload survived recovery'
$reply = & $alias 'recovered entry'
Assert ($LASTEXITCODE -eq 37 -and $reply -ceq 'recovered entry') 'Recovered command did not run'
$null = Run 0 $update
Interrupt target @('rollback','sample','1')
Assert ((Run 0 @('recover')).status -eq 'recovered') 'Interrupted rollback could not recover'
Assert ((Run 0 @('list')).packages[0].version -eq '2') 'Recovery did not undo the interrupted rollback'
$null = Run 0 @('uninstall','sample')
[IO.Directory]::Delete($env:BRW_BIN_DIR)
[IO.Directory]::Delete($state)
[IO.Directory]::Delete($env:XDG_STATE_HOME)
