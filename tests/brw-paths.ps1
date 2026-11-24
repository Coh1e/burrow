# Red when: brw ignores an override, silently falls back for an unusable absolute path, or moves an installation.
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
New-Item -ItemType Directory -Path $root | Out-Null
$env:USERPROFILE = $root
$variables = [ordered]@{
    config = 'XDG_CONFIG_HOME'; cache = 'XDG_CACHE_HOME'; data = 'XDG_DATA_HOME'
    state = 'XDG_STATE_HOME'; bin = 'BRW_BIN_DIR'
}
$defaults = @{
    config = '.config/burrow'; cache = '.cache/burrow'; data = '.local/share/burrow'
    state = '.local/state/burrow'; bin = '.local/bin'
}
$errors = @{config='config-directory';cache='cache-directory';data='data-directory';state='state-directory';bin='entry-directory'}
try {
    foreach ($variable in $variables.Values) { [Environment]::SetEnvironmentVariable($variable, $null) }
    $baseline = Invoke-Brw 0 @('init', '--dry-run')
    Assert ($baseline.status -eq 'planned' -and $baseline.pathChanged) 'Fresh profile init preview missing'
    Assert (@(Get-ChildItem -LiteralPath $root -Force).Count -eq 0) 'Preview changed profile'
    foreach ($key in $variables.Keys) {
        $default = Join-Path $root $defaults[$key]
        Assert ($baseline.paths.$key -eq $default) "Wrong default for $key"
        foreach ($value in @('', 'relative\override', 'C:drive-relative')) {
            [Environment]::SetEnvironmentVariable($variables[$key], $value)
            $result = Invoke-Brw 0 @('init', '--dry-run')
            Assert ($result.paths.$key -eq $default) "Relative or empty value changed $key"
        }
        $override = Join-Path $root "overrides/$key"
        $expected = if ($key -eq 'bin') { $override } else { Join-Path $override 'burrow' }
        [Environment]::SetEnvironmentVariable($variables[$key], $override)
        $result = Invoke-Brw 0 @('init', '--dry-run')
        Assert ($result.paths.$key -eq $expected) "Wrong absolute override for $key"

        $file = Join-Path $root 'file'
        [IO.File]::WriteAllText($file, 'not a directory')
        [Environment]::SetEnvironmentVariable($variables[$key], (Join-Path $file 'missing'))
        $result = Invoke-Brw 1 @('init', '--dry-run')
        Assert ($result.error -eq $errors[$key]) "Unusable path fell back for $key"
        [Environment]::SetEnvironmentVariable($variables[$key], $null)
    }
    $archive = Join-Path $root 'fixture.tar'
    Copy-Item -LiteralPath $Fixture -Destination (Join-Path $root 'payload.exe')
    & (Join-Path $env:SystemRoot 'System32/tar.exe') -cf $archive --format ustar -C $root payload.exe
    Assert ($LASTEXITCODE -eq 0) 'Could not create fixture archive'
    $hash = [BitConverter]::ToString(([Security.Cryptography.SHA256]::Create()).ComputeHash(
        [IO.File]::ReadAllBytes($archive))).Replace('-', '')
    Invoke-Brw 0 @('install', 'original', '1', $archive, $hash, 'payload.exe') | Out-Null
    $catalog = Join-Path $root 'catalog.tsv'
    @('brw-catalog-v1', (@('catalog-tool','1','archive','fixture.tar',$hash,'payload.exe') -join [char]9)) |
        Set-Content -LiteralPath $catalog -Encoding ASCII
    Invoke-Brw 0 @('source', 'add', 'fixture', $catalog) | Out-Null
    Invoke-Brw 0 @('install', 'catalog-tool') | Out-Null
    Assert (Test-Path (Join-Path $root '.config/burrow/sources/fixture')) 'Default config directory missing'
    Assert (Test-Path (Join-Path $root '.cache/burrow')) 'Default download cache missing'
    $old = Join-Path $root '.local/bin/original.exe'
    $oldPayload = Join-Path $root '.local/share/burrow/packages/original/1/payload.exe'
    Assert ((Test-Path $old) -and (Test-Path $oldPayload)) 'Default installation paths missing'
    $state = Join-Path $root '.local/state/burrow'
    Assert (@(Get-ChildItem -LiteralPath $state -Filter 'install-*').Count -eq 2) 'Default install records missing'
    $env:XDG_DATA_HOME = Join-Path $root 'other data/base'
    $env:XDG_STATE_HOME = Join-Path $root 'other state/base'
    $env:BRW_BIN_DIR = Join-Path $root 'other bin/base'
    $env:XDG_CONFIG_HOME = Join-Path $root 'other config/base'
    $env:XDG_CACHE_HOME = Join-Path $root 'other cache/base'
    Remove-Item Env:USERPROFILE
    Assert ((Invoke-Brw 0 @('init', '--dry-run')).paths.bin -eq $env:BRW_BIN_DIR) 'Explicit roots required a profile'
    Assert (!(Invoke-Brw 0 @('list')).packages.Count) 'Changing overrides relocated or found old installation'
    Invoke-Brw 0 @('install', 'second', '1', $archive, $hash, 'payload.exe') | Out-Null
    Assert (Test-Path (Join-Path $env:XDG_DATA_HOME 'burrow/packages/second/1/payload.exe')) 'Data override ignored'
    $newState = Join-Path $env:XDG_STATE_HOME 'burrow'
    Assert (@(Get-ChildItem -LiteralPath $newState -Filter 'install-*').Count -eq 1) 'State override ignored'
    Assert ((Test-Path $old) -and (Test-Path $oldPayload)) 'Original installation moved'
    & (Join-Path $env:BRW_BIN_DIR 'second.exe') fixture
    Assert ($LASTEXITCODE -eq 37) 'Overridden entry did not run'
    Invoke-Brw 0 @('uninstall', 'second') | Out-Null
    Assert (@(Get-ChildItem -LiteralPath $newState -Filter 'install-*').Count -eq 0) 'Uninstall left its record'
    Assert (@(Get-ChildItem -LiteralPath $state -Filter 'install-*').Count -eq 2) 'Other state records moved'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
