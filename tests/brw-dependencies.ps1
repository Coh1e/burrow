# Red when: planning writes, dependencies run out of order, or a required package can be removed.
param([string] $Brw, [string] $Fixture, [string] $Scratch)
$ErrorActionPreference = 'Stop'

function Invoke-Brw([int] $Expected, [string[]] $Arguments) {
    $output = & $Brw @Arguments --json
    if ($LASTEXITCODE -ne $Expected) { throw "Unexpected exit for $Arguments ($LASTEXITCODE): $output" }
    Write-Host ($output -join [Environment]::NewLine)
    $output | ConvertFrom-Json
}
function Assert($condition, [string] $message) {
    if (!$condition) { throw $message }
}

$root = Join-Path $Scratch ([guid]::NewGuid().ToString('N').Substring(0, 12))
$env:USERPROFILE = $root
'XDG_CACHE_HOME','XDG_DATA_HOME','XDG_STATE_HOME','BRW_BIN_DIR' |
    ForEach-Object { Remove-Item -LiteralPath "Env:$_" -ErrorAction SilentlyContinue }
$env:XDG_CONFIG_HOME = Join-Path $root 'config'
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $payload = Join-Path $root 'payload.exe'
    Copy-Item -LiteralPath $Fixture -Destination $payload
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = [BitConverter]::ToString($algorithm.ComputeHash([IO.File]::ReadAllBytes($payload))).Replace('-', '')
    } finally {
        $algorithm.Dispose()
    }
    $catalog = Join-Path $root 'catalog.tsv'
    $rows = @('brw-catalog-v1')
    foreach ($item in @(
        'base', 'mid,base', 'app,mid,base', 'extra', 'broken,extra',
        'cycle-a,cycle-b', 'cycle-b,cycle-a', 'missing,absent', 'later', 'chain,broken,later'
    )) {
        $name, $depends = $item -split ',', 2
        $digest = if ($name -eq 'broken') { '0' * 64 } else { $hash }
        $rows += @($name, '1', 'exe', 'payload.exe', $digest, 'payload.exe', $depends) -join [char]9
    }
    $rows | Set-Content -LiteralPath $catalog -Encoding ASCII
    Invoke-Brw 0 @('source', 'add', 'fixture', $catalog) | Out-Null
    $cycle = Invoke-Brw 1 @('install', 'cycle-a')
    Assert ($cycle.error -eq 'dependency-cycle' -and $cycle.step -eq 0) 'Missing cycle diagnostic'
    $missing = Invoke-Brw 1 @('install', 'missing')
    Assert ($missing.error -eq 'dependency-not-found' -and $missing.package -eq 'absent') 'Missing unknown dependency diagnostic'
    $other = Join-Path $root 'other.tsv'
    $rows[0..1] | Set-Content -LiteralPath $other -Encoding ASCII
    Invoke-Brw 0 @('source', 'add', 'other', $other) | Out-Null
    $ambiguous = Invoke-Brw 1 @('install', 'app')
    Assert ($ambiguous.error -eq 'dependency-ambiguous' -and $ambiguous.package -eq 'base') 'Ambiguous dependency was accepted'
    Invoke-Brw 0 @('source', 'remove', 'other') | Out-Null
    $preview = Invoke-Brw 0 @('install', 'app', '--dry-run')
    Assert (($preview.packages -join ',') -eq 'base,mid,app') 'Dependency order or deduplication failed'
    Assert (!(Test-Path (Join-Path $root '.local'))) 'Planning modified the installation root'

    Invoke-Brw 0 @('install', 'base') | Out-Null
    $preview = Invoke-Brw 0 @('install', 'app', '--dry-run')
    Assert (($preview.packages -join ',') -eq 'mid,app') 'Installed dependency was not reused'
    Invoke-Brw 0 @('install', 'app') | Out-Null
    $failed = Invoke-Brw 1 @('install', 'broken')
    Assert ($failed.error -eq 'digest-mismatch' -and $failed.package -eq 'broken') 'Main package failure was not attributed'
    $failed = Invoke-Brw 1 @('install', 'chain')
    Assert ($failed.error -eq 'digest-mismatch' -and $failed.package -eq 'broken') 'Dependency failure was not attributed'
    $state = Invoke-Brw 0 @('list')
    Assert ((($state.packages.name | Sort-Object) -join ',') -eq 'app,base,extra,mid') 'Committed dependency was lost'
    Invoke-Brw 0 @('source', 'remove', 'fixture') | Out-Null
    $tar = Join-Path $env:SystemRoot 'System32/tar.exe'
    $archive = Join-Path $root 'update.tar'
    & $tar -cf $archive --format ustar -C $root payload.exe
    Assert ($LASTEXITCODE -eq 0) 'Could not create update fixture'
    $hash = [BitConverter]::ToString(([Security.Cryptography.SHA256]::Create()).ComputeHash(
        [IO.File]::ReadAllBytes($archive))).Replace('-', '')
    Invoke-Brw 0 @('update', 'app', '2', $archive, $hash, 'payload.exe') | Out-Null
    foreach ($arguments in @(@('uninstall', 'MID', '--dry-run'), @('uninstall', 'MID'))) {
        $blocked = Invoke-Brw 1 $arguments
        Assert ($blocked.error -eq 'package-required' -and $blocked.dependent -eq 'app') 'Reverse dependency diagnostic missing'
    }
    Invoke-Brw 0 @('rollback', 'app', '1') | Out-Null
    Assert ((Invoke-Brw 1 @('uninstall', 'mid')).dependent -eq 'app') 'Rollback lost dependency records'
    $entry = Join-Path $root '.local/bin/mid.exe'
    & $entry protected
    Assert ($LASTEXITCODE -eq 37) 'Protected entry was changed'
    foreach ($name in 'app', 'mid', 'base', 'extra') {
        Invoke-Brw 0 @('uninstall', $name) | Out-Null
    }
    Assert (!(Invoke-Brw 0 @('list')).packages.Count) 'Uninstall left packages behind'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
