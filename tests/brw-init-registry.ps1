# Red when: init preview writes, init fails to persist PATH, or repeating init duplicates the entry.
param([string] $Runner, [string] $Scratch)
$ErrorActionPreference = 'Stop'
$root = Join-Path $Scratch ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    & $Runner (Join-Path $root 'registry.hiv') $root
    if ($LASTEXITCODE) { throw "Registry integration failed at stage $LASTEXITCODE" }
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
