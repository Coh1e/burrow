# Red when: the verified ZIP fails to start, its Blueprint fails, or brw loads an unapproved DLL.
# Run in 64-bit Windows PowerShell on the Windows 11 x64 host receiving the ZIP.
# Pass -PristineImage only for a freshly provisioned host with no developer tools.
param(
    [Parameter(Mandatory)][string]$Archive,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ZipSha256,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$BrwSha256,
    [Parameter(Mandatory)][string]$Work,
    [Parameter(Mandatory)][string]$ImageDescription,
    [switch]$PristineImage
)
$ErrorActionPreference = 'Stop'
$windows = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
if (-not [Environment]::Is64BitProcess -or $env:PROCESSOR_ARCHITECTURE -ne 'AMD64' -or
    $windows.InstallationType -ne 'Client' -or [int]$windows.CurrentBuildNumber -lt 22000) {
    throw 'Run with x64 Windows PowerShell on a Windows 11 client.'
}
$root = [IO.Path]::GetFullPath($Work)
if (Test-Path -LiteralPath $root) { throw 'The evidence/work directory must be new.' }
$archivePath = (Resolve-Path -LiteralPath $Archive).Path
if ((Get-FileHash -LiteralPath $archivePath).Hash -ne $ZipSha256) { throw 'ZIP digest mismatch.' }
$visibleTools = @(Get-Command -CommandType Application -Name gcc.exe,g++.exe,cl.exe,cmake.exe,ninja.exe,msbuild.exe,git.exe -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty Name -Unique)
$installedTools = @(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue |
    Where-Object { $_.DisplayName -match 'Visual Studio|Windows Software Development Kit|MinGW|w64devkit|MSYS2|CMake|LLVM|^Git version' } |
    Select-Object -ExpandProperty DisplayName -Unique)
if ($PristineImage -and ($visibleTools.Count -or $installedTools.Count)) {
    throw 'The declared clean image has developer tools. Inspect the host before using it for acceptance.'
}
$delivery = Join-Path $root 'delivery'
$project = Join-Path $root 'project'
$kit = Join-Path $root 'kit'
$null = New-Item -ItemType Directory -Path $root,$project,$kit
Expand-Archive -LiteralPath $archivePath -DestinationPath $delivery
$brw = Join-Path $delivery 'brw.exe'
if ((Get-FileHash -LiteralPath $brw).Hash -ne $BrwSha256) { throw 'Extracted brw digest mismatch.' }
$allowed = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'windows-system-dlls.txt')
$encoding = New-Object System.Text.UTF8Encoding($false)
$savedEnvironment = @{}
try {
    foreach ($name in 'XDG_CONFIG_HOME','XDG_DATA_HOME','XDG_STATE_HOME','XDG_CACHE_HOME','BRW_BIN_DIR') {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
        [Environment]::SetEnvironmentVariable($name, (Join-Path $root $name))
    }
    $list = & $brw list --json
    if ($LASTEXITCODE -ne 0) { throw 'brw list failed.' }
    $empty = $list | ConvertFrom-Json
    if ($empty.status -ne 'ok' -or $empty.packages.Count -ne 0) { throw 'Unexpected empty package list.' }

    # The child marks the precise sampling window. The brw process waits in its
    # normal Blueprint run operation while the observer lists its loaded modules.
    $module = @'
export default () => [{op:"run",program:"powershell.exe",args:["-NoProfile","-NonInteractive","-Command","[IO.File]::WriteAllText('ready','1'); Start-Sleep -Seconds 5"]}];
'@
    $entry = Join-Path $kit 'main.mjs'
    [IO.File]::WriteAllText($entry, $module, $encoding)
    $digest = (Get-FileHash -LiteralPath $entry).Hash.ToLowerInvariant()
    $manifest = Join-Path $kit 'blueprint.manifest'
    [IO.File]::WriteAllText($manifest, "brw-blueprint-v1`nentry`tmain.mjs`nplatform`tany`narch`tany`nrequires`trun`nmodule`tmain.mjs`t$digest`n", $encoding)
    $trusted = (Get-FileHash -LiteralPath $manifest).Hash.ToLowerInvariant()
    $applyOut = Join-Path $root 'apply.json'
    $process = Start-Process -FilePath $brw -ArgumentList @('blueprint','apply',('"' + $kit + '"'),
        '--sha256',$trusted,'--workspace',('"' + $project + '"'),'--json') -PassThru -WindowStyle Hidden -WorkingDirectory $root -RedirectStandardOutput $applyOut -RedirectStandardError (Join-Path $root 'apply.stderr')
    try {
        # Keep the process handle open so Windows PowerShell 5.1 can read the exit status.
        $null = $process.Handle
        $ready = Join-Path $project 'ready'
        $clock = [Diagnostics.Stopwatch]::StartNew()
        while (-not (Test-Path -LiteralPath $ready) -and -not $process.HasExited -and $clock.Elapsed.TotalSeconds -lt 30) {
            Start-Sleep -Milliseconds 50
        }
        if (-not (Test-Path -LiteralPath $ready) -or $process.HasExited) { throw 'The sampling window was not observed.' }
        $system = Join-Path $env:SystemRoot 'System32'
        $modules = @(foreach ($item in $process.Modules) {
            if ($item.FileName -eq $brw) { continue }
            if ((Split-Path -Parent $item.FileName) -ne $system -or $allowed -notcontains $item.ModuleName) {
                throw "Unreviewed module in the Blueprint process: $($item.ModuleName)"
            }
            $item.ModuleName.ToLowerInvariant()
        })
        if ($modules -notcontains 'kernel32.dll' -or $modules -notcontains 'ntdll.dll') { throw 'Incomplete module snapshot.' }
        if (-not $process.WaitForExit(15000) -or $process.ExitCode -ne 0) { throw "brw blueprint apply exit=$($process.ExitCode)." }
    } finally {
        $process.Dispose()
    }
    $applied = Get-Content -Raw -LiteralPath $applyOut | ConvertFrom-Json
    if ($applied.status -ne 'applied' -or $applied.completed -ne 1) { throw 'The observation Blueprint did not complete its one step.' }
    $record = [ordered]@{image=$ImageDescription; operatorConfirmedPristine=[bool]$PristineImage;
        cleanHostVerified=[bool]$PristineImage; windowsBuild=$windows.CurrentBuildNumber; ubr=$windows.UBR;
        architecture=$env:PROCESSOR_ARCHITECTURE; visibleTools=$visibleTools; installedTools=$installedTools;
        zipSha256=$ZipSha256; brwSha256=$BrwSha256; list=$empty; apply=$applied;
        moduleSnapshotDuring='brw blueprint apply waiting for its PowerShell child'; modules=@($modules | Sort-Object -Unique)}
    [IO.File]::WriteAllText((Join-Path $root 'clean-windows.json'), (($record | ConvertTo-Json -Depth 8) + "`n"), $encoding)
} finally {
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
}
