param(
    [Parameter(Mandatory)][string]$BurrowExe,
    [Parameter(Mandatory)][string]$EvidenceDir,
    [string]$ScoopRoot = 'C:\Users\Neo\scoop',
    [string]$Package = 'jq',
    [string]$OldVersion = '1.7.1'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'invoke-burrow.ps1')
$EvidenceDir = [IO.Path]::GetFullPath($EvidenceDir)
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
$BurrowExe = [IO.Path]::GetFullPath($BurrowExe)
$ScoopRoot = [IO.Path]::GetFullPath($ScoopRoot)
$scoop = (Get-Command scoop.ps1 -ErrorAction Stop).Source
$commands = [System.Collections.Generic.List[object]]::new()
function Run([string]$name,[string]$exe,[string[]]$arguments) {
    $log = Join-Path $EvidenceDir ($name+'.log')
    $output = Invoke-BurrowNative $exe  @arguments | Out-String
    $exit = $LASTEXITCODE
    Set-Content -LiteralPath $log -Value $output -Encoding utf8
    $commands.Add([pscustomobject]@{name=$name;exe=$exe;arguments=$arguments;exit=$exit;log=$log})
    return [pscustomobject]@{exit=$exit;output=$output;log=$log}
}
function Burrow([string]$name,[string[]]$arguments) {
    return Run $name $BurrowExe (@('--root',$ScoopRoot)+$arguments)
}
function Scoop([string]$name,[string[]]$arguments) {
    return Run $name $scoop $arguments
}
function Snapshot([string]$name) {
    $lines = foreach ($path in @("$ScoopRoot/apps",$ScoopRoot+'/shims',"$ScoopRoot/.burrow")) {
        if (Test-Path -LiteralPath $path) {
            Get-ChildItem -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
                $relative = $_.FullName.Substring($ScoopRoot.Length).TrimStart('\')
                $hash = if ($_.PSIsContainer) { 'directory' } else { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash }
                "$relative|$($_.Length)|$($_.LastWriteTimeUtc.ToString('o'))|$hash"
            }
        }
    }
    $lines | Sort-Object | Set-Content -LiteralPath (Join-Path $EvidenceDir ($name+'.snapshot.txt')) -Encoding utf8
}
function Assert-True([bool]$condition,[string]$message) { if (-not $condition) { throw $message } }
function InstalledVersion([string]$name,[string]$stage) {
    $list = (Burrow ($name+'-list-'+$stage) @('list','--json')).output | ConvertFrom-Json
    # A shared Scoop installation has no Burrow record and reports status=installed; after a Burrow
    # write the published management record carries status=ready. Both describe an installed
    # package and neither is a retirement of the other.
    $entry = @($list | Where-Object { $_.name -eq $name -and $_.status -in @('installed','ready') })
    if ($entry.Count -eq 0) { return $null }
    return $entry[0]
}
Snapshot 'before'
$before = InstalledVersion $Package 'before'
Assert-True ($null -eq $before) "The selected real package is already installed; refusing to change an existing user installation: $Package"
$inventoryBefore = (Burrow 'burrow-list-before' @('list','--json')).output | ConvertFrom-Json
$scoopVersion = (Scoop 'scoop-version' @('--version')).output
$bucketHead = (& git -C "$ScoopRoot/buckets/main" rev-parse HEAD 2>&1 | Out-String).Trim()
# 1. Scoop creates the installation from a real local bucket history manifest.
$install = Scoop 'scoop-install-old' @('install',"$Package@$OldVersion")
Assert-True ($install.exit -eq 0) "Scoop install $Package@$OldVersion failed: $($install.output)"
$scoopRun = Run 'scoop-run-old' "$ScoopRoot/shims/$Package.exe" @('--version')
Assert-True ($scoopRun.exit -eq 0 -and $scoopRun.output.Trim() -eq "jq-$OldVersion") "Scoop-installed program did not run as $OldVersion`: $($scoopRun.output)"
$observed = InstalledVersion $Package 'after-scoop-install'
Assert-True ($null -ne $observed -and $observed.version -eq $OldVersion) "Burrow did not identify the Scoop installation: $($observed | ConvertTo-Json -Compress)"
$privateRecordBefore = Test-Path -LiteralPath "$ScoopRoot/.burrow/packages/$Package.json"
Assert-True (-not $privateRecordBefore) "A Burrow private record already existed before the shared update: $ScoopRoot/.burrow/packages/$Package.json"
$scoopInstallManagement = $observed.management
$preview = Burrow 'burrow-update-preview' @('update',$Package,'--dry-run','--json')
Assert-True ($preview.exit -eq 0) "Burrow update preview failed: $($preview.output)"
$previewJson = $preview.output | ConvertFrom-Json
Assert-True ($previewJson.execution_available -eq $true) "Burrow update preview was not executable: $($preview.output)"
$planned = @($previewJson.ir_operations | ForEach-Object operation)
# 2. Burrow performs the update against the same shared installation and records the plan it consumed.
$update = Burrow 'burrow-update' @('update',$Package,'--json')
Assert-True ($update.exit -eq 0) "Burrow update failed: $($update.output)"
$updateJson = $update.output | ConvertFrom-Json
$journal = Get-Content -LiteralPath $updateJson.journal -Raw | ConvertFrom-Json
Assert-True ((($journal.observed_steps) -join ',') -eq ($planned -join ',')) "Burrow execution did not follow its displayed plan: planned=$planned observed=$($journal.observed_steps)"
$burrowRun = Run 'burrow-run' "$ScoopRoot/shims/$Package.exe" @('--version')
Assert-True ($burrowRun.exit -eq 0 -and $burrowRun.output.Trim() -eq 'jq-1.8.2') "Burrow-updated program did not run: $($burrowRun.output)"
$afterUpdate = InstalledVersion $Package 'after-burrow-update'
Assert-True ($null -ne $afterUpdate -and $afterUpdate.version -eq '1.8.2') "Burrow did not report the updated version: $($afterUpdate | ConvertTo-Json -Compress)"
$scoopList = Scoop 'scoop-list-after-burrow-update' @('list',$Package)
Assert-True ($scoopList.exit -eq 0 -and $scoopList.output -match '1\.8\.2') "Scoop did not recognize the Burrow-updated installation: $($scoopList.output)"
$scoopUpdate = Scoop 'scoop-update-after-burrow' @('update',$Package)
Assert-True ($scoopUpdate.exit -eq 0) "Scoop could not continue managing the Burrow-updated package: $($scoopUpdate.output)"
$standardManifest = if (Test-Path "$ScoopRoot/apps/$Package/current/scoop-manifest.json") { "$ScoopRoot/apps/$Package/current/scoop-manifest.json" } else { "$ScoopRoot/apps/$Package/current/manifest.json" }
$standardVersion = (Get-Content -LiteralPath $standardManifest -Raw | ConvertFrom-Json).version
Assert-True ($standardVersion -eq '1.8.2') "Standard Scoop install record did not carry the updated version: $standardVersion"
$retainedOld = Test-Path -LiteralPath "$ScoopRoot/apps/$Package/$OldVersion"
Assert-True $retainedOld 'The previous version was not retained after the Burrow update'
# Remove only the old version through real Scoop before testing a clean Burrow uninstall.
$cleanup = Scoop 'scoop-cleanup' @('cleanup',$Package)
Assert-True ($cleanup.exit -eq 0) "Scoop cleanup failed: $($cleanup.output)"
$remove = Burrow 'burrow-uninstall' @('uninstall',$Package,'--json')
Assert-True ($remove.exit -eq 0) "Burrow uninstall failed: $($remove.output)"
Assert-True (-not (Test-Path "$ScoopRoot/apps/$Package/current") -and -not (Test-Path "$ScoopRoot/shims/$Package.exe")) 'Burrow uninstall left the shared package visible'
$gone = InstalledVersion $Package 'after-burrow-uninstall'
Assert-True ($null -eq $gone) 'Burrow still listed the uninstalled package'
$scoopListGone = Scoop 'scoop-list-after-burrow-uninstall' @('list',$Package)
Assert-True ($scoopListGone.output -notmatch [regex]::Escape($Package)) "Scoop still listed the Burrow-uninstalled package: $($scoopListGone.output)"
# 3. Reverse flow: Burrow creates the standard installation; Scoop resets its entry and runs it.
$installBurrow = Burrow 'burrow-install' @('install',$Package,'--json')
Assert-True ($installBurrow.exit -eq 0) "Burrow install failed: $($installBurrow.output)"
$burrowInstallRun = Run 'burrow-install-run' "$ScoopRoot/shims/$Package.exe" @('--version')
Assert-True ($burrowInstallRun.exit -eq 0 -and $burrowInstallRun.output.Trim() -eq 'jq-1.8.2') "Burrow-installed program did not run: $($burrowInstallRun.output)"
$scoopListBurrow = Scoop 'scoop-list-burrow-install' @('list',$Package)
Assert-True ($scoopListBurrow.exit -eq 0 -and $scoopListBurrow.output -match '1\.8\.2') "Scoop did not recognize the Burrow installation: $($scoopListBurrow.output)"
$reset = Scoop 'scoop-reset' @('reset',$Package)
Assert-True ($reset.exit -eq 0) "Scoop reset failed: $($reset.output)"
$resetRun = Run 'scoop-run-after-reset' "$ScoopRoot/shims/$Package.exe" @('--version')
Assert-True ($resetRun.exit -eq 0 -and $resetRun.output.Trim() -eq 'jq-1.8.2') "Scoop-reset entry did not run: $($resetRun.output)"
$afterReset = InstalledVersion $Package 'after-scoop-reset'
Assert-True ($null -ne $afterReset -and $afterReset.version -eq '1.8.2') "Burrow did not re-identify the Scoop-reset installation: $($afterReset | ConvertTo-Json -Compress)"
$removeAfterReset = Burrow 'burrow-uninstall-after-reset' @('uninstall',$Package,'--json')
Assert-True ($removeAfterReset.exit -eq 0) "Burrow uninstall after Scoop reset failed: $($removeAfterReset.output)"
Assert-True (-not (Test-Path "$ScoopRoot/apps/$Package/current") -and -not (Test-Path "$ScoopRoot/shims/$Package.exe") -and -not (Test-Path "$ScoopRoot/shims/$Package.shim")) 'Burrow uninstall after Scoop reset left published state'
$finalGone = InstalledVersion $Package 'after-reverse-uninstall'
Assert-True ($null -eq $finalGone) 'Burrow still listed the package after the reverse flow'
Snapshot 'after'
$inventoryAfter = (Burrow 'burrow-list-after' @('list','--json')).output | ConvertFrom-Json
$evidence = [ordered]@{
    platform = (Get-CimInstance Win32_OperatingSystem).Caption
    scoop_root = $ScoopRoot
    package = $Package
    old_version = $OldVersion
    scoop_version = $scoopVersion.Trim()
    bucket_head = $bucketHead
    burrow_exe = $BurrowExe
    burrow_sha256 = (Get-FileHash -LiteralPath $BurrowExe -Algorithm SHA256).Hash.ToLower()
    inventory_before = @($inventoryBefore | ForEach-Object { [pscustomobject]@{name=$_.name;version=$_.version;bucket=$_.bucket;management=$_.management} })
    inventory_after = @($inventoryAfter | ForEach-Object { [pscustomobject]@{name=$_.name;version=$_.version;bucket=$_.bucket;management=$_.management} })
    scoop_install_management = $scoopInstallManagement
    private_record_before_update = $privateRecordBefore
    planned_ir_operations = $planned
    observed_ir_operations = @($journal.observed_steps)
    previous_version_retained = $retainedOld
    standard_manifest = $standardManifest
    standard_version = $standardVersion
    commands = @($commands)
    timestamp = [DateTime]::UtcNow.ToString('o')
}
$evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $EvidenceDir 'real-scoop-alternating-evidence.json') -Encoding utf8
Write-Output "PASS real Scoop/Burrow single-file alternating management: $(Join-Path $EvidenceDir 'real-scoop-alternating-evidence.json')"
