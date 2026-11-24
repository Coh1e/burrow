# Red when: preview writes files, rejected input publishes, or the shim drops arguments or exit status.
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

$root = Join-Path $Scratch ([guid]::NewGuid().ToString())
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
