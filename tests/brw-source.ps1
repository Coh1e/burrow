# Red when: a catalog cannot be searched, safe archives fail to install, or rejected downloads become visible.
param([string] $Brw, [string] $Fixture, [string] $Scratch, [string] $CancelRunner)
$ErrorActionPreference = 'Stop'

function Invoke-Brw([int] $Expected, [string[]] $Arguments) {
    $output = & $Brw @Arguments --json
    if ($LASTEXITCODE -ne $Expected) {
        throw "brw $Arguments returned $LASTEXITCODE, expected ${Expected}: $output"
    }
    Write-Host ($output -join "`n")
    $output | ConvertFrom-Json
}

function Get-Sha256([string] $path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try {
        [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '')
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

$root = Join-Path $Scratch ([guid]::NewGuid().ToString('N'))
$env:USERPROFILE = $root
$env:XDG_CONFIG_HOME = Join-Path $root 'config'
$env:TMP = Join-Path $root 'temporary'
$env:TEMP = $env:TMP
New-Item -ItemType Directory -Path $env:TMP -Force | Out-Null
Copy-Item -LiteralPath $Fixture -Destination (Join-Path $root 'payload.exe')
$tar = Join-Path $env:SystemRoot 'System32/tar.exe'
& $tar -czf (Join-Path $root 'package.tar.gz') --format ustar -C $root payload.exe
if ($LASTEXITCODE) { throw 'Fixture archive creation failed' }
$exeHash = Get-Sha256 (Join-Path $root 'payload.exe')
$zipPath = Join-Path $root 'package.zip'
$tree = Join-Path $root 'archive-tree'
New-Item -ItemType Directory -Path (Join-Path $tree 'bin'), (Join-Path $tree 'share/empty') -Force | Out-Null
Copy-Item -LiteralPath $Fixture -Destination (Join-Path $tree 'bin/payload.exe')
Set-Content -LiteralPath (Join-Path $tree 'share/support.txt') -Value 'fixture data' -Encoding ASCII
& $tar -a -cf $zipPath -C $tree .
if ($LASTEXITCODE) { throw 'Fixture ZIP creation failed' }
$zipHash = Get-Sha256 $zipPath
$tarHash = Get-Sha256 (Join-Path $root 'package.tar.gz')
Add-Type -AssemblyName System.IO.Compression
$unsafeZip = Join-Path $root 'unsafe.zip'
$archiveStream = [IO.File]::Create($unsafeZip)
try {
    $zip = New-Object IO.Compression.ZipArchive ($archiveStream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $zip.CreateEntry('share/safe.txt') | Out-Null
        $zip.CreateEntry('share/../../outside.exe') | Out-Null
    } finally {
        $zip.Dispose()
    }
} finally {
    $archiveStream.Dispose()
}
$unsafeHash = Get-Sha256 $unsafeZip
$listener = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$marker = Join-Path $root 'request-cancel'
$catalog = Join-Path $root 'catalog.tsv'
@(
    "brw-catalog-v1"
    "tool`t1.0`texe`tpayload.exe`t$exeHash`tpayload.exe"
    "packed`t2.0`tarchive`tpackage.tar.gz`t$tarHash`tpayload.exe"
    "broken`t1.0`texe`tpayload.exe`t$('0' * 64)`tpayload.exe"
    "unsafe`t1.0`tarchive`tunsafe.zip`t$unsafeHash`tpayload.exe"
    "remote`t3.0`tarchive`thttp://127.0.0.1:$port/package.zip?fixture=1`t$zipHash`tbin/payload.exe"
    "cancelled`t1.0`texe`thttp://127.0.0.1:$port/slow.exe`t$exeHash`tpayload.exe"
) | Set-Content -LiteralPath $catalog -Encoding ASCII

function Invoke-HttpInstall([switch] $Cancel) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.FileName = $Brw
    $start.Arguments = 'install remote --json'
    $target = '/package.zip?fixture=1'
    if ($Cancel) {
        $start.FileName = $CancelRunner
        $start.Arguments = '"{0}" "{1}"' -f $Brw, $marker
        $target = '/slow.exe'
    }
    $child = [Diagnostics.Process]::Start($start)
    $connection = $null
    try {
        $clock = [Diagnostics.Stopwatch]::StartNew()
        while (!$listener.Pending() -and !$child.HasExited -and $clock.Elapsed.TotalSeconds -lt 10) {
            Start-Sleep -Milliseconds 20
        }
        if (!$listener.Pending()) { throw "No HTTP request for $target" }
        $connection = $listener.AcceptTcpClient()
        $stream = $connection.GetStream()
        $stream.ReadTimeout = 5000
        $reader = New-Object IO.StreamReader ($stream, [Text.Encoding]::ASCII)
        if ($reader.ReadLine() -notlike "GET $target HTTP/*") { throw "Unexpected request for $target" }
        do { $header = $reader.ReadLine() } while ($header)
        $bytes = [IO.File]::ReadAllBytes($zipPath)
        $length = $bytes.Length
        if ($Cancel) {
            $bytes = New-Object byte[] 65536
            $length = 1048576
        }
        $headers = [Text.Encoding]::ASCII.GetBytes(
            "HTTP/1.1 200 OK`r`nContent-Length: $length`r`nConnection: close`r`n`r`n")
        $stream.Write($headers, 0, $headers.Length)
        $stream.Write($bytes, 0, $bytes.Length)
        if ($Cancel) {
            $clock.Restart()
            do {
                $staged = @(Get-ChildItem -LiteralPath $env:TMP -Recurse -Force |
                    Where-Object { !$_.PSIsContainer } | ForEach-Object {
                        $file = [IO.File]::Open($_.FullName, 'Open', 'Read', 'ReadWrite')
                        try { if ($file.Length -gt 0) { $file.Length } } finally { $file.Dispose() }
                    })
                if ($staged.Count) { break }
                Start-Sleep -Milliseconds 20
            } while (!$child.HasExited -and $clock.Elapsed.TotalSeconds -lt 4)
            if (!$staged.Count) { throw 'Partial HTTP body was never staged' }
            if (Test-Path -LiteralPath (Join-Path $root '.local/bin/cancelled.exe')) {
                throw 'Entry visible before the download finished'
            }
            Write-Host "Observed staged HTTP bytes before cancellation: $($staged[0])"
            New-Item -ItemType File -Path $marker | Out-Null
        }
        if (!$child.WaitForExit(16000)) { throw "Timed out installing $target" }
        $output = $child.StandardOutput.ReadToEnd()
        Write-Host $output
        $result = $output | ConvertFrom-Json
        if ($Cancel) {
            if ($child.ExitCode -ne 1 -or $result.error -ne 'cancelled' -or $result.step -ne 1) {
                throw "Cancellation diagnostic missing (exit $($child.ExitCode))"
            }
        } elseif ($child.ExitCode -ne 0 -or $result.status -ne 'installed') {
            throw 'HTTP ZIP install failed'
        }
    } finally {
        if ($connection) { $connection.Close() }
        if (!$child.HasExited) { $child.Kill(); $child.WaitForExit() }
        $child.Dispose()
    }
}

try {
    $malformed = Join-Path $root 'malformed.tsv'
    [IO.File]::WriteAllBytes($malformed, [byte[]](98, 114, 119, 255))
    if ((Invoke-Brw 1 @('source', 'add', 'bad-catalog', $malformed)).error -ne 'catalog-format' -or
        (Invoke-Brw 0 @('source', 'list')).sources.Count) {
        throw 'Malformed catalog was registered'
    }
    Invoke-Brw 0 @('source', 'add', 'fixture', $catalog) | Out-Null
    $sources = Invoke-Brw 0 @('source', 'list')
    if ($sources.sources[0].name -ne 'fixture') { throw 'Source registration missing' }
    if ((Invoke-Brw 0 @('search')).packages.Count -ne 6) { throw 'Available packages missing' }
    $search = Invoke-Brw 0 @('search', 'TOO')
    if ($search.packages.Count -ne 1 -or $search.packages[0].name -ne 'tool') {
        throw 'Search did not return the matching package'
    }
    $detail = Invoke-Brw 0 @('show', 'fixture/packed')
    if ($detail.package.sha256 -ne $tarHash) { throw 'Package detail missing manifest fields' }
    $preview = Invoke-Brw 0 @('install', 'remote', '--dry-run')
    if (($preview.operations -join ',') -ne 'download,verify,extract,publish' -or
        $preview.pending -notcontains 'sha256-verification') {
        throw 'Unexpected source install preview'
    }
    $textPreview = & $Brw install remote --dry-run
    if ($LASTEXITCODE -ne 0 -or ($textPreview -join ' ') -notmatch 'Pending checks.*SHA-256') {
        throw 'Text preview did not identify pending verification'
    }
    if ($listener.Pending() -or @(Get-ChildItem -LiteralPath $env:TMP -Force).Count) {
        throw 'Preview contacted the server or wrote a download'
    }
    Invoke-Brw 0 @('source', 'add', 'other', $catalog) | Out-Null
    if ((Invoke-Brw 1 @('show', 'packed')).error -ne 'package-ambiguous') {
        throw 'Ambiguous package selection was accepted'
    }
    Invoke-Brw 0 @('show', 'fixture/packed') | Out-Null
    Invoke-Brw 0 @('source', 'remove', 'other') | Out-Null
    $bad = Invoke-Brw 1 @('install', 'broken')
    if ($bad.error -ne 'digest-mismatch' -or $bad.step -ne 2) { throw 'Missing digest diagnostic' }
    if ((Invoke-Brw 0 @('list')).packages.Count -or
        (Test-Path -LiteralPath (Join-Path $root '.local/share/burrow/packages/broken'))) {
        throw 'Failed package was published'
    }
    if (@(Get-ChildItem -LiteralPath $env:TMP -Force).Count) { throw 'Failed download staging leaked' }
    $unsafe = Invoke-Brw 1 @('install', 'unsafe')
    $packageRoot = Join-Path $root '.local/share/burrow/packages'
    if ($unsafe.error -ne 'archive-path' -or $unsafe.step -ne 3 -or
        (Test-Path -LiteralPath (Join-Path $packageRoot 'unsafe/1.0')) -or
        (Test-Path -LiteralPath (Join-Path $packageRoot 'unsafe/outside.exe')) -or
        @(Get-ChildItem -Path (Join-Path $packageRoot 'unsafe/.stage-*') -ErrorAction SilentlyContinue).Count) {
        throw 'Archive traversal escaped staging or left staged files'
    }
    Invoke-HttpInstall -Cancel
    if ((Invoke-Brw 0 @('list')).packages.Count -or
        (Test-Path -LiteralPath (Join-Path $root '.local/share/burrow/packages/cancelled'))) {
        throw 'Cancelled package became visible'
    }
    if (@(Get-ChildItem -LiteralPath $env:TMP -Force).Count) { throw 'Download staging leaked' }
    Invoke-HttpInstall
    Invoke-Brw 0 @('install', 'tool') | Out-Null
    Invoke-Brw 0 @('install', 'packed') | Out-Null
    $list = Invoke-Brw 0 @('list')
    if ((@($list.packages | ForEach-Object { $_.name + '=' + $_.version }) | Sort-Object) -join ',' -ne 'packed=2.0,remote=3.0,tool=1.0') {
        throw 'Installed versions missing'
    }
    foreach ($name in 'tool', 'packed', 'remote') {
        & (Join-Path $root ".local/bin/$name.exe") source-fixture
        if ($LASTEXITCODE -ne 37) { throw "Entry failed for $name" }
    }
    $installed = Join-Path $root '.local/share/burrow/packages/remote/3.0'
    if ((Get-Content -LiteralPath (Join-Path $installed 'share/support.txt')) -ne 'fixture data' -or
        !(Test-Path -LiteralPath (Join-Path $installed 'share/empty'))) {
        throw 'Nested archive paths were lost'
    }
    $foreign = Join-Path $installed 'share/foreign.txt'
    Set-Content -LiteralPath $foreign -Value 'user file' -Encoding ASCII
    if ((Invoke-Brw 1 @('uninstall', 'remote')).error -ne 'package-conflict') {
        throw 'Uninstall accepted a foreign nested file'
    }
    Remove-Item -LiteralPath $foreign
    Invoke-Brw 0 @('source', 'remove', 'fixture') | Out-Null
    if ((Invoke-Brw 0 @('source', 'list')).sources.Count) { throw 'Source was not removed' }
    if ((Invoke-Brw 0 @('list')).packages.Count -ne 3) { throw 'Removing a source changed installations' }
    foreach ($name in 'remote', 'tool', 'packed') {
        Invoke-Brw 0 @('uninstall', $name) | Out-Null
    }
    if ((Invoke-Brw 0 @('list')).packages.Count -or (Test-Path -LiteralPath $installed)) {
        throw 'Uninstall left installed packages'
    }
} finally {
    $listener.Stop()
    Remove-Item -LiteralPath $root -Recurse -Force
}
