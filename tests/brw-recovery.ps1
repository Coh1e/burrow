# Red when: recovery overwrites an edited object, confuses same-name entries, or cannot resume an interrupted undo.
param([string] $Brw, [string] $Fixture, [string] $Scratch)
$ErrorActionPreference = 'Stop'

function Assert($condition, [string] $message) {
    if (!$condition) { throw $message }
}
function Run([int] $expected, [string[]] $arguments) {
    $output = & $Brw @arguments --json
    Assert ($LASTEXITCODE -eq $expected) "Unexpected exit $LASTEXITCODE for $arguments ($output)"
    if ($output) { $output | ConvertFrom-Json }
}
function Interrupt([string] $phase, [string[]] $arguments) {
    $env:BRW_TEST_INTERRUPT = $phase
    try { Run 86 $arguments } finally { Remove-Item Env:BRW_TEST_INTERRUPT }
}

$id = [guid]::NewGuid().ToString('N').Substring(0,12)
$root = Join-Path $Scratch $id
$env:USERPROFILE = $root
$env:XDG_DATA_HOME = Join-Path $root 'data'
$env:XDG_STATE_HOME = Join-Path ([IO.Path]::GetTempPath()) "brw-$id"
$env:BRW_BIN_DIR = Join-Path $root 'bin'
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
Assert ((Run 0 @('recover')).status -eq 'recovered') 'First install could not be undone'
Assert (@((Run 0 @('list')).packages).Count -eq 0) 'Undone first install was still listed'

Interrupt entry $install
$bytes = [IO.File]::ReadAllBytes($alias)
[IO.File]::AppendAllText($alias, 'user edit')
Assert ((Run 1 @('recover','--dry-run')).error -eq 'entry-conflict') 'Preview missed changed shim'
Assert ((Run 1 @('recover')).error -eq 'entry-conflict') 'Recovery missed changed shim'
Assert ((Get-Item $alias).Length -gt $bytes.Length) 'Recovery removed user edit'
[IO.File]::WriteAllBytes($alias, $bytes)
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
[IO.Directory]::Delete($state)
[IO.Directory]::Delete($env:XDG_STATE_HOME)
