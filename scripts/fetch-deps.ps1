$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$sources = Join-Path $projectRoot '.local/deps/sources'
New-Item -ItemType Directory -Path $sources -Force | Out-Null

function Get-PinnedFile([string]$Name, [string]$Url, [string]$Hash, [string]$Directory) {
    $target = Join-Path $Directory $Name
    if (-not (Test-Path -LiteralPath $target)) {
        $temporary = "$target.download"
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        & curl.exe -fL --retry 2 $Url -o $temporary
        if ($LASTEXITCODE -ne 0) { throw "Dependency download failed: $Name" }
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLower() -ne $Hash) { throw "Dependency hash mismatch: $Name" }
        Move-Item -LiteralPath $temporary -Destination $target
    }
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLower() -ne $Hash) { throw "Existing dependency hash mismatch: $Name" }
    return $target
}

foreach ($pin in (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'blueprint-sources.txt'))) {
    $name, $digest, $url = $pin.Split(' ')
    $archive = Get-PinnedFile $name $url $digest $sources
    $directory = $name.Substring(0, $name.IndexOf('.tar.'))
    $members = @()
    if ($directory -like 'libgit2-*') {
        $marker = 'CMakeLists.txt'
        $members = @("$directory/CMakeLists.txt", "$directory/cmake/*", "$directory/include/*",
                     "$directory/src/*", "$directory/deps/*", "$directory/COPYING")
    } elseif ($directory -like 'quickjs-*') {
        $marker = 'quickjs.c'
    } elseif ($directory -like 'libarchive-*' -or $directory -like 'zlib-*') {
        $marker = 'CMakeLists.txt'
    } else { throw "Unknown Blueprint dependency: $name" }
    if (-not (Test-Path -LiteralPath (Join-Path $sources "$directory/$marker"))) {
        & tar.exe -xf $archive -C $sources @members
        if ($LASTEXITCODE -ne 0) { throw "Source extraction failed: $name" }
    }
    Write-Output "Verified $directory sources"
}
