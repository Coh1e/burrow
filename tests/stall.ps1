# Red when: a write command that produces no output for several seconds stops reporting that it is still
# working, or that report leaks into a --json run or a quiet run.
param([Parameter(Mandatory)][string]$BurrowExe, [Parameter(Mandatory)][string]$TestRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'invoke-burrow.ps1')
$TestRoot = [IO.Path]::GetFullPath($TestRoot)
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../.local/tests')).TrimEnd('\') + '\'
if (-not ($TestRoot.TrimEnd('\') + '\').StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test data must stay within the project test directory'
}
$archive = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../.local/deps/test-archives/ripgrep-15.2.0-x86_64-pc-windows-msvc.zip'))
if (-not (Test-Path -LiteralPath $archive)) { throw "Missing test archive: $archive" }
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLower()
$url = 'file:///' + ($archive -replace '\\', '/')
$root = Join-Path $TestRoot 'stall'
if (Test-Path $root) { Remove-Item -LiteralPath $root -Recurse -Force }
New-Item -ItemType Directory -Path "$root/buckets/probe/bucket" -Force | Out-Null
@"
{
  "version": "1.0",
  "url": "$url",
  "hash": "$hash",
  "extract_dir": "ripgrep-15.2.0-x86_64-pc-windows-msvc",
  "bin": "rg.exe"
}
"@ | Set-Content "$root/buckets/probe/bucket/slow.json" -Encoding utf8 -NoNewline
$previousStall = $env:BURROW_TEST_STALL_MS
try {
    $env:BURROW_TEST_STALL_MS = '6500'

    # Text mode reports the silence instead of looking frozen.
    $out = Invoke-BurrowNative $BurrowExe  --root $root install probe/slow
    if ($LASTEXITCODE -cne 0) { throw "the install failed: $($out -join ' | ')" }
    if (($out -join "`n") -cnotmatch 'burrow: still working, \d+s without progress') {
        throw "a silent write command printed no stall report: $($out -join ' | ')"
    }
    Invoke-BurrowNative $BurrowExe  --root $root uninstall slow | Out-Null

    # A --json run stays machine-readable: the report must not appear.
    $out = Invoke-BurrowNative $BurrowExe  --root $root install probe/slow --json
    if ($LASTEXITCODE -cne 0) { throw 'the json install failed' }
    if (($out -join "`n") -cmatch 'still working') { throw 'a --json run printed the stall report' }
    Invoke-BurrowNative $BurrowExe  --root $root uninstall slow -q | Out-Null

    # A quiet run suppresses progress, including this report.
    $out = Invoke-BurrowNative $BurrowExe  --root $root install probe/slow -q
    if ($LASTEXITCODE -cne 0) { throw 'the quiet install failed' }
    if (($out -join "`n") -cmatch 'still working') { throw 'a quiet run printed the stall report' }
} finally {
    if ($null -eq $previousStall) { Remove-Item Env:\BURROW_TEST_STALL_MS -ErrorAction SilentlyContinue }
    else { $env:BURROW_TEST_STALL_MS = $previousStall }
}
Write-Output 'PASS: a silent write command reports that it is still working'
