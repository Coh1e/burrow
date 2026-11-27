# Red when: planning writes, dependencies run out of order, or a required package can be removed.
param([string] $Brw, [string] $Fixture, [string] $Scratch, [string] $RecordingBrw)
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

function Get-FixtureSnapshot {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $entries = foreach ($item in (Get-ChildItem -LiteralPath $root -Recurse -Force | Sort-Object FullName)) {
            $relative = $item.FullName.Substring($root.Length + 1)
            if ($item.PSIsContainer) { "dir $relative" }
            else {
                $digest = $sha.ComputeHash([IO.File]::ReadAllBytes($item.FullName))
                "file $relative $([Convert]::ToBase64String($digest))"
            }
        }
        return ($entries -join [Environment]::NewLine)
    } finally {
        $sha.Dispose()
    }
}

function Get-InstallPreview([string] $Name) {
    $before = Get-FixtureSnapshot
    $plan = Invoke-Brw 0 @('install', $Name, '--dry-run')
    Assert ((Get-FixtureSnapshot) -ceq $before) "JSON preview changed the fixture for $Name"
    $lines = & $Brw install $Name --dry-run
    Assert ($LASTEXITCODE -eq 0) "Text preview failed for $Name"
    Assert ((Get-FixtureSnapshot) -ceq $before) "Text preview changed the fixture for $Name"
    $expected = @(
        foreach ($package in $plan.packages) { "Install $package" }
        $plan.operations
        'Pending checks include payload availability and SHA-256 verification.'
    )
    Assert (($lines -join ';') -ceq ($expected -join ';')) "Text and JSON differ for $Name"
    Assert (($plan.pending -join ',') -ceq 'payload-availability,sha256-verification') 'Missing pending checks'
    return $plan
}

function Test-InstallTrace([string] $Name, $Plan, [int] $ExpectedExit = 0) {
    $packages = if ($Plan.packages) { $Plan.packages } else { @($Name) }
    $expected = foreach ($package in $packages) {
        foreach ($operation in $Plan.operations) { "$package $operation" }
    }
    $env:BRW_TEST_INSTALL_TRACE = Join-Path $root ([guid]::NewGuid().ToString('N') + '.trace')
    try {
        $output = & $RecordingBrw install $Name --json
        Assert ($LASTEXITCODE -eq $ExpectedExit) "Unexpected install exit for $Name ($LASTEXITCODE)"
        $report = $output | ConvertFrom-Json
        if ($ExpectedExit) {
            Assert ($report.error -eq 'digest-mismatch' -and $report.package -eq 'broken' -and
                $report.step -eq 2) 'Wrong verification failure or package-local step'
            $failure = [array]::IndexOf($expected, 'broken verify')
            Assert ($failure -ge 0) 'The failing package and operation were absent from the preview'
            $expected = $expected[0..$failure]
        }
        Assert (Test-Path -LiteralPath $env:BRW_TEST_INSTALL_TRACE) 'No backend steps were observed'
        $actual = Get-Content -LiteralPath $env:BRW_TEST_INSTALL_TRACE
        Assert (($actual -join ';') -ceq ($expected -join ';')) "Execution differed from preview for ${Name}: $actual"
        Write-Host "Backend steps for $Name (exit $ExpectedExit): $($actual -join '; ')"
    } finally {
        Remove-Item Env:BRW_TEST_INSTALL_TRACE
    }
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
    $listener = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        $port = $listener.LocalEndpoint.Port
        $rows -replace '^(base\s+1\s+exe\s+)payload\.exe', ('${1}http://127.0.0.1:' + $port + '/base') |
            Set-Content -LiteralPath $catalog -Encoding ASCII
        Assert ((Invoke-Brw 0 @('show', 'base')).package.location -eq "http://127.0.0.1:$port/base") 'HTTP dependency fixture missing'
        $preview = Get-InstallPreview 'app'
        Assert (!$listener.Pending()) 'Preview contacted a dependency payload server'
    } finally {
        $listener.Stop()
    }
    $rows | Set-Content -LiteralPath $catalog -Encoding ASCII
    Assert (($preview.packages -join ',') -eq 'base,mid,app') 'Dependency order or deduplication failed'
    Assert (!(Test-Path (Join-Path $root '.local'))) 'Planning modified the installation root'

    $single = Get-InstallPreview 'base'
    Test-InstallTrace 'base' $single
    $preview = Get-InstallPreview 'app'
    Assert (($preview.packages -join ',') -eq 'mid,app') 'Installed dependency was not reused'
    Test-InstallTrace 'app' $preview
    $before = Get-FixtureSnapshot
    foreach ($arguments in @(@('install', 'app', '--dry-run'), @('install', 'app'))) {
        $existing = Invoke-Brw 1 $arguments
        Assert ($existing.status -ceq 'failed' -and $existing.error -ceq 'destination-exists' -and
            $existing.step -eq 0 -and $existing.package -ceq 'app') 'Installed main package precondition differed'
        Assert ((Get-FixtureSnapshot) -ceq $before) 'Rejecting the installed main package modified the fixture'
    }
    $chain = Get-InstallPreview 'chain'
    Assert (($chain.packages -join ',') -eq 'extra,broken,later,chain') 'Dependency plan lost its failure prefix or suffix'
    Test-InstallTrace 'chain' $chain 1
    $failed = Invoke-Brw 1 @('install', 'broken')
    Assert ($failed.error -eq 'digest-mismatch' -and $failed.package -eq 'broken') 'Main package failure was not attributed'
    $state = Invoke-Brw 0 @('list')
    Assert ((($state.packages.name | Sort-Object) -join ',') -eq 'app,base,extra,mid') 'Committed dependency was lost'
    $beforeChange = Get-InstallPreview 'chain'
    Assert (($beforeChange.packages -join ',') -ceq 'broken,later,chain') 'Unexpected original dependency set'
    $rows -replace 'broken,later$', 'later' | Set-Content -LiteralPath $catalog -Encoding ASCII
    Invoke-Brw 0 @('install', 'chain') | Out-Null
    $state = Invoke-Brw 0 @('list')
    Assert ((($state.packages.name | Sort-Object) -join ',') -ceq 'app,base,chain,extra,later,mid') 'Execution reused stale dependencies'
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
    foreach ($name in 'app', 'mid', 'base', 'extra', 'chain', 'later') {
        Invoke-Brw 0 @('uninstall', $name) | Out-Null
    }
    Assert (!(Invoke-Brw 0 @('list')).packages.Count) 'Uninstall left packages behind'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
