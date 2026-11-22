#requires -Version 7.0
<#
.SYNOPSIS
Per-package acceptance of a real, shared Scoop installation against the Burrow CLI.

.DESCRIPTION
The hard acceptance queue is the machine's actual installed set, not a fixed sample list:
every package found in the selected root is inventoried and each one gets its own record of
run/version/entry/environment/data/install-status facts. Facts are read from the real
filesystem, the real user environment and the real program itself; Burrow's view is compared
with Scoop's own view of the same shared state.

With -Lifecycle the same queue is alternately managed by both clients:
  * Scoop republishes entries for every package (scoop reset) and Burrow re-reads + runs it;
  * selected packages switch to a retained local version through Burrow and back, with Scoop
    re-reading and re-running the entry after each switch;
  * Optionally a real remote version switch is performed by Burrow (UpdateClinkViaBurrow) and by
    Scoop (UpdateGitViaScoop) so a no-op cannot pass as an update;
  * Optionally one package is uninstalled and re-installed by Burrow and read by Scoop.

Every command, exit code, output and observation is written under a fresh run directory so a
previous run's evidence is never overwritten. Nothing here is product code: the script drives
the real CLI and observes the real result.

.EXAMPLE
pwsh -NoProfile -File tests/real-scoop-cohort.ps1 -BurrowExe .local/build/burrow.exe `
  -EvidenceDir .local/pi/tasks/40/evidence
#>
param(
    [Parameter(Mandatory)][string]$BurrowExe,
    [Parameter(Mandatory)][string]$EvidenceDir,
    [string]$ScoopRoot = 'C:\Users\Neo\scoop',
    [string[]]$OnlyPackages = @(),
    [switch]$Lifecycle,
    [string[]]$BurrowResetPackages = @(),
    [string[]]$SkipLifecyclePackages = @('pi-coding-agent'),
    [switch]$UpdateClinkViaBurrow,
    [switch]$UpdateGitViaScoop,
    [string]$ReinstallPackage = '',
    [string]$ScoopRemovePackage = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'invoke-burrow.ps1')
$ProgressPreference = 'SilentlyContinue'

$BurrowExe = [IO.Path]::GetFullPath($BurrowExe)
$ScoopRoot = [IO.Path]::GetFullPath($ScoopRoot)
$EvidenceDir = [IO.Path]::GetFullPath($EvidenceDir)
# pwsh -File forwards comma-separated values as one literal string, so split them here.
$OnlyPackages = @($OnlyPackages | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$BurrowResetPackages = @($BurrowResetPackages | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$SkipLifecyclePackages = @($SkipLifecyclePackages | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$scoopPs1 = (Get-Command scoop.ps1 -ErrorAction Stop).Source
$runName = 'cohort-' + (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$runDir = Join-Path $EvidenceDir $runName
$suffix = 1
while (Test-Path -LiteralPath $runDir) { $runDir = Join-Path $EvidenceDir ($runName + '-' + $suffix); $suffix++ }
New-Item -ItemType Directory -Path $runDir -Force | Out-Null

$script:Commands = [System.Collections.Generic.List[object]]::new()
$script:Checks = [System.Collections.Generic.List[object]]::new()
$script:FreshPath = (@(
        [Environment]::GetEnvironmentVariable('Path', 'Machine'),
        [Environment]::GetEnvironmentVariable('Path', 'User')
    ) | Where-Object { $_ }) -join ';'

function Add-Check {
    param([string]$Scope, [string]$Name, [bool]$Ok, [string]$Detail)
    $record = [pscustomobject]@{ scope = $Scope; name = $Name; ok = $Ok; detail = $Detail }
    $script:Checks.Add($record)
    $label = if ($Ok) { 'ok  ' } else { 'FAIL' }
    Write-Host ("[{0}] {1}: {2}{3}" -f $label, $Scope, $Name, $(if ($Detail) { " ($Detail)" } else { '' }))
}

function Invoke-Tool {
    param(
        [string]$Name,
        [string]$Exe,
        [string[]]$Arguments,
        [string]$LogDir = $runDir,
        [switch]$FailOnExit
    )
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
    $log = Join-Path $LogDir ($Name + '.log')
    $output = Invoke-BurrowNative $Exe  @Arguments | Out-String
    $code = $LASTEXITCODE
    $header = '$ ' + $Exe + ' ' + ($Arguments -join ' ')
    $body = $header + [Environment]::NewLine + $output + [Environment]::NewLine + 'exit=' + $code
    Set-Content -LiteralPath $log -Value $body -Encoding utf8
    $script:Commands.Add([pscustomobject]@{
            name      = $Name
            exe       = $Exe
            arguments = $Arguments
            exit      = $code
            log       = $log
            utc       = [DateTime]::UtcNow.ToString('o')
        })
    if ($FailOnExit -and $code -ne 0) {
        throw "$Name failed with exit $code; see $log"
    }
    return [pscustomobject]@{ exit = $code; output = $output; log = $log }
}

function Burrow {
    param([string]$Name, [string[]]$Arguments, [string]$LogDir = $runDir, [switch]$FailOnExit)
    return Invoke-Tool $Name $BurrowExe (@('--root', $ScoopRoot) + $Arguments) $LogDir -FailOnExit:$FailOnExit
}

function Scoop {
    param([string]$Name, [string[]]$Arguments, [string]$LogDir = $runDir, [switch]$FailOnExit)
    $quoted = @($Arguments | ForEach-Object { "'" + ($_ -replace "'", "''") + "'" })
    $command = '& ' + "'" + ($scoopPs1 -replace "'", "''") + "' " + ($quoted -join ' ')
    return Invoke-Tool $Name "$PSHOME\pwsh.exe" @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $command) $LogDir -FailOnExit:$FailOnExit
}

function Get-RealPath {
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (-not $item) { return $null }
    if ($item.LinkType -in @('Junction', 'SymbolicLink') -and $item.Target) {
        return [IO.Path]::GetFullPath([string]$item.Target)
    }
    return [IO.Path]::GetFullPath($item.FullName)
}

function Test-SamePath {
    param([string]$Left, [string]$Right)
    if (-not $Left -or -not $Right) { return $false }
    $a = Get-RealPath $Left
    if (-not $a) { $a = [IO.Path]::GetFullPath(($Left -replace '/', '\')) }
    $b = Get-RealPath $Right
    if (-not $b) { $b = [IO.Path]::GetFullPath(($Right -replace '/', '\')) }
    return [string]::Equals($a.TrimEnd('\'), $b.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)
}

function Test-VersionText {
    param([string]$Version, [string]$Text)
    $parts = @($Version -split '[^0-9]+' | Where-Object { $_ -ne '' })
    if ($parts.Count -eq 0 -or -not $Text) { return $false }
    $pattern = '\b' + ($parts -join '[.\- ]?(?:windows[.\- ]?)?') + '\b'
    return [regex]::IsMatch($Text, $pattern)
}

function Invoke-InFreshPath {
    param([scriptblock]$Block)
    $saved = $env:Path
    try {
        $env:Path = $script:FreshPath
        return & $Block
    }
    finally {
        $env:Path = $saved
    }
}

function Get-ScoopInstalled {
    param([string]$LogDir = $runDir)
    $result = Scoop 'scoop-list' @('list') $LogDir
    $map = @{}
    foreach ($line in ($result.output -split "`r?`n")) {
        if ($line -match '^(?<name>[A-Za-z0-9_.\-]+)\s+(?<version>\S+)\s+(?<source>\S+)') {
            $name = $matches.name
            $version = $matches.version
            if ($name -in @('Name', '----', 'WARN', 'INFO', 'ERROR') -or $version -notmatch '^[0-9]') { continue }
            $map[$name] = [pscustomobject]@{ version = $version; source = $matches.source }
        }
    }
    return [pscustomobject]@{ exit = $result.exit; map = $map; text = $result.output; log = $result.log }
}

function Get-BurrowEntry {
    param([string]$Package, [string]$LogDir = $runDir)
    $result = Burrow 'burrow-list' @('list', '--json') $LogDir
    if ($result.exit -ne 0) { throw "burrow list failed: $($result.log)" }
    $entries = @($result.output | ConvertFrom-Json)
    return [pscustomobject]@{
        all   = $entries
        entry = @($entries | Where-Object { $_.name -eq $Package }) | Select-Object -First 1
        log   = $result.log
    }
}

function Get-UserPathEntries {
    return @(([Environment]::GetEnvironmentVariable('Path', 'User')) -split ';' | Where-Object { $_ })
}

# Assess the declared exposure of one installed package against the real user environment and the
# real Start Menu links. Used both for the read-only facts record and for each lifecycle step, so a
# rewritten entry (new version, reset by either client) is judged by current facts, not stale ones.
function Get-ExposureAssessment {
    param($Ir)
    $environment = @($Ir.exposure.environment | ForEach-Object {
            $declared = $_
            $userValue = [Environment]::GetEnvironmentVariable($declared.name, 'User')
            $matchesExpected = $false
            if ($userValue) {
                if ($declared.value -match '^[A-Za-z]:\\' -or $declared.value -match '^\\\\') {
                    $matchesExpected = Test-SamePath $userValue $declared.value
                }
                else {
                    $matchesExpected = ($userValue -eq $declared.value)
                }
            }
            [pscustomobject]@{
                name             = $declared.name
                expected         = $declared.value
                user_value       = $userValue
                matches_expected = $matchesExpected
            }
        })
    $userPathEntries = Get-UserPathEntries
    $pathEntries = @($Ir.exposure.path_entries | ForEach-Object {
            $entry = $_
            [pscustomobject]@{
                path                 = $entry
                exists               = (Test-Path -LiteralPath $entry)
                present_in_user_path = [bool](@($userPathEntries | Where-Object { Test-SamePath $_ $entry }).Count)
            }
        })
    $shortcuts = @($Ir.exposure.shortcuts | ForEach-Object {
            [pscustomobject]@{ name = $_.name; link = $_.link; exists = (Test-Path -LiteralPath $_.link) }
        })
    return [pscustomobject]@{
        environment    = $environment
        path_entries   = $pathEntries
        shortcuts      = $shortcuts
        environment_ok = @($environment | Where-Object { -not $_.matches_expected }).Count -eq 0
        path_ok        = @($pathEntries | Where-Object { -not $_.exists -or -not $_.present_in_user_path }).Count -eq 0
        shortcuts_ok   = @($shortcuts | Where-Object { -not $_.exists }).Count -eq 0
    }
}

function Get-PersistenceAssessment {
    param($Persistence)
    $records = @()
    foreach ($p in @($Persistence)) {
        $storeExists = Test-Path -LiteralPath $p.store
        $linkExists = Test-Path -LiteralPath $p.link
        $linkMatches = $false
        if ($linkExists) {
            $linkItem = Get-Item -LiteralPath $p.link -Force
            $linkMatches = $linkItem.LinkType -in @('Junction', 'SymbolicLink') -and (Test-SamePath $p.link $p.store)
        }
        $files = @()
        if ($storeExists) {
            $files = @(Get-ChildItem -LiteralPath $p.store -Recurse -File -Force -ErrorAction SilentlyContinue)
        }
        $sample = @($files | Sort-Object FullName | Select-Object -First 3 | ForEach-Object {
                [pscustomobject]@{
                    relative = $_.FullName.Substring($p.store.Length).TrimStart('\')
                    size     = $_.Length
                    sha256   = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower()
                }
            })
        $records += [pscustomobject]@{
            source       = $p.source
            store        = $p.store
            link         = $p.link
            store_exists = $storeExists
            link_exists  = $linkExists
            link_matches = $linkMatches
            file_count   = $files.Count
            sample       = $sample
        }
    }
    return $records
}

function Get-DataPolicyFacts {
    param($Package, $Exposure)
    $dirs = @()
    foreach ($e in @($Exposure.environment)) {
        if ($e.value -match '(?i)\\persist\\') {
            $exists = Test-Path -LiteralPath $e.value
            $files = if ($exists) { @(Get-ChildItem -LiteralPath $e.value -Recurse -File -Force -ErrorAction SilentlyContinue) } else { @() }
            $dirs += [pscustomobject]@{ variable = $e.name; directory = $e.value; exists = $exists; file_count = $files.Count }
        }
    }
    return [pscustomobject]@{
        persist_root        = Join-Path $ScoopRoot (Join-Path 'persist' $Package)
        persist_root_exists = Test-Path -LiteralPath (Join-Path $ScoopRoot (Join-Path 'persist' $Package))
        directories         = $dirs
    }
}

function Invoke-PackageProgram {
    param($Descriptor, [string]$LogDir, [string]$Label, [string]$ResolvedPath)
    $entry = if ($ResolvedPath) { $ResolvedPath } else { $Descriptor.runPath }
    if (-not (Test-Path -LiteralPath $entry)) {
        return [pscustomobject]@{ exit = $null; output = "missing entry: $entry"; log = $null; entry = $entry }
    }
    $result = Invoke-InFreshPath { Invoke-Tool $Label $entry $Descriptor.args $LogDir }
    $result | Add-Member -NotePropertyName entry -NotePropertyValue $entry
    return $result
}

function Get-ShimVariants {
    param([string]$Command)
    $suffixes = @('.exe', '.cmd', '.ps1', '.bat', '.shim', '')
    $found = @()
    foreach ($suffix in $suffixes) {
        $candidate = Join-Path $ScoopRoot ('shims\' + $Command + $suffix)
        if (Test-Path -LiteralPath $candidate) { $found += $candidate }
    }
    return $found
}

function Get-ItemFacts {
    param($Descriptor)
    $pkg = $Descriptor.name
    $logDir = Join-Path $runDir (Join-Path 'items' $pkg)
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null

    $inventory = Get-BurrowEntry $pkg $logDir
    $burrowEntry = $inventory.entry
    if (-not $burrowEntry) { throw "burrow list does not report $pkg as installed" }
    $scoopInstalled = Get-ScoopInstalled $logDir
    $scoopEntry = $scoopInstalled.map[$pkg]

    $preview = Burrow 'uninstall-preview' @('uninstall', $pkg, '--dry-run', '--json') $logDir
    $ir = $preview.output | ConvertFrom-Json
    $diagnostics = @($ir.diagnostics | Where-Object { $_.category -ne 'verification' })

    $appDir = Join-Path $ScoopRoot (Join-Path 'apps' $pkg)
    $current = Join-Path $appDir 'current'
    $currentItem = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
    $currentIsLink = $currentItem -and $currentItem.LinkType -in @('Junction', 'SymbolicLink')
    $currentTarget = if ($currentIsLink) { Get-RealPath $current } else { $null }

    $manifestPath = Join-Path $current 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { $manifestPath = Join-Path $current 'scoop-manifest.json' }
    $manifest = if (Test-Path -LiteralPath $manifestPath) { Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json } else { $null }
    $installPath = Join-Path $current 'install.json'
    $installRecord = if (Test-Path -LiteralPath $installPath) { Get-Content -LiteralPath $installPath -Raw | ConvertFrom-Json } else { $null }
    $installJsonPresent = [bool]($installRecord)

    $commandEntries = @()
    foreach ($cmd in @($ir.package.commands)) {
        $found = @(Get-ShimVariants $cmd.name)
        $commandEntries += [pscustomobject]@{
            command = $cmd.name
            target  = $cmd.target
            launcher = $cmd.launcher
            shims   = $found
        }
    }
    $allCommandsPublished = @($commandEntries | Where-Object { $_.shims.Count -eq 0 }).Count -eq 0
    $exposure = Get-ExposureAssessment $ir
    $shortcutEntries = $exposure.shortcuts
    $missingShortcuts = @($shortcutEntries | Where-Object { -not $_.exists })
    $environmentEntries = $exposure.environment
    $environmentOk = $exposure.environment_ok
    $pathEntryChecks = $exposure.path_entries
    $pathEntriesOk = $exposure.path_ok

    $freshCommand = Invoke-InFreshPath { Get-Command $Descriptor.freshCommand -ErrorAction SilentlyContinue }
    $resolvedEntry = if ($freshCommand) { $freshCommand.Source } else { $Descriptor.runPath }
    $run = Invoke-PackageProgram $Descriptor $logDir 'run' $resolvedEntry
    $runVersionOk = Test-VersionText $burrowEntry.version $run.output

    $persistence = @(Get-PersistenceAssessment $ir.persistence)
    $persistenceOk = @($persistence | Where-Object { -not $_.store_exists -or -not $_.link_matches }).Count -eq 0
    $dataPolicy = Get-DataPolicyFacts $pkg $ir.exposure
    $policyOk = $dataPolicy.persist_root_exists -or @($dataPolicy.directories | Where-Object { -not $_.exists }).Count -eq 0
    $oldVersions = @($burrowEntry.old_versions | ForEach-Object { $_.version })

    $facts = [pscustomobject]@{
        package                 = $pkg
        bucket                  = $burrowEntry.bucket
        burrow_version            = $burrowEntry.version
        burrow_management         = $burrowEntry.management
        burrow_last_operation     = $burrowEntry.last_operation
        burrow_diagnostics        = @($burrowEntry.diagnostics).Count
        burrow_directory          = $burrowEntry.directory
        scoop_version           = if ($scoopEntry) { $scoopEntry.version } else { $null }
        manifest_version        = if ($manifest) { $manifest.version } else { $null }
        manifest_path           = $manifestPath
        install_record_present  = $installJsonPresent
        install_architecture    = if ($installRecord) { $installRecord.architecture } else { $null }
        current_is_link         = [bool]$currentIsLink
        current_target          = $currentTarget
        old_versions            = $oldVersions
        commands                = $commandEntries
        commands_published      = $allCommandsPublished
        shortcuts               = $shortcutEntries
        environment             = $environmentEntries
        path_entries            = $pathEntryChecks
        fresh_path_command      = if ($freshCommand) { $freshCommand.Source } else { $null }
        run                     = [pscustomobject]@{
            entry       = $run.entry
            resolved_from_path = $resolvedEntry
            arguments   = $Descriptor.args
            exit        = $run.exit
            output      = $run.output.Trim()
            version_ok  = $runVersionOk
            log         = $run.log
        }
        persistence             = $persistence
        data_policy_directories = $dataPolicy
        ir_execution_available  = $ir.execution_available
        ir_operations           = @($ir.ir_operations | ForEach-Object { $_.operation })
        diagnostics             = $diagnostics
    }

    $scope = "facts/$pkg"
    Add-Check $scope 'burrow-reports-installed' ([bool]$burrowEntry) "version=$($burrowEntry.version) management=$($burrowEntry.management)"
    Add-Check $scope 'current-junction' ([bool]$currentIsLink) "target=$currentTarget"
    Add-Check $scope 'current-target-matches-version' ([bool]($currentTarget -and (Test-SamePath $currentTarget (Join-Path $appDir $burrowEntry.version)))) "$currentTarget"
    Add-Check $scope 'manifest-version-matches' ([bool]($manifest -and $manifest.version -eq $burrowEntry.version)) "manifest=$($manifest.version) burrow=$($burrowEntry.version)"
    Add-Check $scope 'scoop-version-matches' ([bool]($scoopEntry -and $scoopEntry.version -eq $burrowEntry.version)) "scoop=$($scoopEntry.version) burrow=$($burrowEntry.version)"
    Add-Check $scope 'commands-published' $allCommandsPublished (@($commandEntries | ForEach-Object { "$($_.command)->$(@($_.shims).Count)" }) -join ', ')
    Add-Check $scope 'shortcuts-present' ($missingShortcuts.Count -eq 0) (@($shortcutEntries | ForEach-Object { "$($_.name)=$($_.exists)" }) -join ', ')
    Add-Check $scope 'environment-ownership' $environmentOk (@($environmentEntries | ForEach-Object { "$($_.name)=$($_.user_value)" }) -join '; ')
    Add-Check $scope 'path-entries' $pathEntriesOk (@($pathEntryChecks | ForEach-Object { "$($_.path) exists=$($_.exists) in-path=$($_.present_in_user_path)" }) -join '; ')
    Add-Check $scope 'fresh-terminal-resolution' ([bool]$freshCommand) $(if ($freshCommand) { $freshCommand.Source } else { "not resolvable from user+machine PATH" })
    Add-Check $scope 'program-runs' ($run.exit -eq 0 -and $runVersionOk) "exit=$($run.exit) output=$($run.output.Trim())"
    Add-Check $scope 'persistence-intact' $persistenceOk (@($persistence | ForEach-Object { "$($_.source) link=$($_.link_matches) files=$($_.file_count)" }) -join '; ')
    Add-Check $scope 'data-policy-directories' $policyOk (@($dataPolicy.directories | ForEach-Object { "$($_.variable) exists=$($_.exists) files=$($_.file_count)" }) -join '; ')
    Add-Check $scope 'no-blocking-diagnostics' ($diagnostics.Count -eq 0) (@($diagnostics | ForEach-Object { $_.code }) -join ', ')
    # The preview must carry the IR sequence: an empty list here would make every operation
    # assertion below vacuous, so the harness fails instead of recording empty evidence.
    Add-Check $scope 'ir-operations-planned' ($facts.ir_operations.Count -gt 0) (@($facts.ir_operations) -join ', ')
    $facts | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $logDir 'facts.json') -Encoding utf8
    return $facts
}

function Test-PackageAfterChange {
    param($Facts, $Descriptor, [string]$Label, [string]$ExpectedVersion)
    $pkg = $Facts.package
    $logDir = Join-Path $runDir (Join-Path (Join-Path 'lifecycle' $pkg) $Label)
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    $scope = "lifecycle/$pkg/$Label"
    $inventory = Get-BurrowEntry $pkg $logDir
    $burrowEntry = $inventory.entry
    $expected = if ($ExpectedVersion) { $ExpectedVersion } else { $Facts.burrow_version }
    $scoopInstalled = Get-ScoopInstalled $logDir
    $scoopEntry = $scoopInstalled.map[$pkg]
    $currentTarget = Get-RealPath (Join-Path $ScoopRoot (Join-Path (Join-Path 'apps' $pkg) 'current'))
    $appDir = Join-Path $ScoopRoot (Join-Path 'apps' $pkg)
    $freshCommand = Invoke-InFreshPath { Get-Command $Descriptor.freshCommand -ErrorAction SilentlyContinue }
    $resolvedEntry = if ($freshCommand) { $freshCommand.Source } else { $Descriptor.runPath }
    $run = Invoke-PackageProgram $Descriptor $logDir 'run' $resolvedEntry
    $preview = Burrow 'uninstall-preview' @('uninstall', $pkg, '--dry-run', '--json') $logDir
    $ir = $preview.output | ConvertFrom-Json
    $exposure = Get-ExposureAssessment $ir
    $shortcutsOk = $exposure.shortcuts_ok
    $environmentOk = $exposure.environment_ok
    $pathOk = $exposure.path_ok
    $persistence = @(Get-PersistenceAssessment $ir.persistence)
    $persistenceOk = $true
    foreach ($before in @($Facts.persistence)) {
        $after = $persistence | Where-Object { $_.source -eq $before.source } | Select-Object -First 1
        if (-not $after -or -not $after.store_exists -or -not $after.link_matches) { $persistenceOk = $false }
        if ($before.file_count -gt 0) {
            $beforeSample = ($before.sample | ForEach-Object { $_.relative + ':' + $_.sha256 }) -join ','
            $afterSample = ($after.sample | ForEach-Object { $_.relative + ':' + $_.sha256 }) -join ','
            if ($beforeSample -ne $afterSample) { $persistenceOk = $false }
        }
    }
    Add-Check $scope 'burrow-identifies-current-facts' ([bool]($burrowEntry -and $burrowEntry.version -eq $expected)) "burrow=$($burrowEntry.version) expected=$expected"
    Add-Check $scope 'scoop-identifies-same-version' ([bool]($scoopEntry -and $scoopEntry.version -eq $expected)) "scoop=$($scoopEntry.version) expected=$expected"
    Add-Check $scope 'current-target' ([bool]($currentTarget -and (Test-SamePath $currentTarget (Join-Path $appDir $expected)))) "$currentTarget"
    Add-Check $scope 'program-runs-expected-version' ($run.exit -eq 0 -and (Test-VersionText $expected $run.output)) "exit=$($run.exit) output=$($run.output.Trim())"
    Add-Check $scope 'fresh-terminal-resolution' ([bool]$freshCommand) $(if ($freshCommand) { $freshCommand.Source } else { 'not resolvable from user+machine PATH' })
    Add-Check $scope 'entries-survive' ($shortcutsOk -and $environmentOk -and $pathOk) "shortcuts=$shortcutsOk env=$environmentOk path=$pathOk"
    Add-Check $scope 'data-preserved' $persistenceOk (@($persistence | ForEach-Object { "$($_.source) files=$($_.file_count)" }) -join '; ')
    return [pscustomobject]@{
        label       = $Label
        burrow_version = $burrowEntry.version
        scoop_version = $scoopEntry.version
        current_target = $currentTarget
        run_exit    = $run.exit
        run_output  = $run.output.Trim()
        shortcuts_ok = $shortcutsOk
        environment_ok = $environmentOk
        path_ok     = $pathOk
        persistence_ok = $persistenceOk
        persistence = $persistence
        log_dir     = $logDir
    }
}

function Get-RetainedVersion {
    param($Facts)
    $candidates = @($Facts.old_versions | Where-Object { $_ -and $_ -ne $Facts.burrow_version })
    if ($candidates.Count -eq 0) { return $null }
    return @($candidates | Sort-Object { [version](($_ -replace '[^0-9.]', '')) })[-1]
}

# ---------------------------------------------------------------------------
# Environment baseline
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
Write-Host "evidence run: $runDir"
& git -C (Join-Path $ScoopRoot 'buckets\main') rev-parse HEAD 2>&1 | Out-String | Set-Content -LiteralPath (Join-Path $runDir 'bucket-head.txt') -Encoding utf8
$scoopVersion = Scoop 'scoop-version' @('--version')
$baseline = [ordered]@{
    utc             = [DateTime]::UtcNow.ToString('o')
    platform        = (Get-CimInstance Win32_OperatingSystem).Caption + ' ' + [Environment]::OSVersion.Version
    scoop_root      = $ScoopRoot
    scoop_version   = $scoopVersion.output.Trim()
    bucket_head     = (Get-Content -LiteralPath (Join-Path $runDir 'bucket-head.txt') -Raw).Trim()
    burrow_exe        = $BurrowExe
    burrow_sha256     = (Get-FileHash -LiteralPath $BurrowExe -Algorithm SHA256).Hash.ToLower()
    burrow_version    = (& $BurrowExe --version | Out-String).Trim()
    powershell      = $PSVersionTable.PSVersion.ToString()
    lifecycle       = [bool]$Lifecycle
    reset_packages  = @($BurrowResetPackages)
    update_clink    = [bool]$UpdateClinkViaBurrow
    update_git      = [bool]$UpdateGitViaScoop
    reinstall       = $ReinstallPackage
    program_resolver = 'user+machine PATH from HKCU/HKLM, no inherited session PATH'
}
$baseline | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDir 'baseline.json') -Encoding utf8

$inventoryBefore = Burrow 'burrow-list-before' @('list', '--json')
$inventoryBefore.output | Set-Content -LiteralPath (Join-Path $runDir 'inventory-before.json') -Encoding utf8
$inventoryJson = @($inventoryBefore.output | ConvertFrom-Json)
$scoopBefore = Get-ScoopInstalled
$scoopBefore.text | Set-Content -LiteralPath (Join-Path $runDir 'scoop-list-before.txt') -Encoding utf8
Write-Host ("installed packages (burrow): {0}; (scoop): {1}" -f ($inventoryJson.name -join ', '), (($scoopBefore.map.Keys | Sort-Object) -join ', '))

$descriptors = @(
    @{ name = '7zip'; freshCommand = '7z'; runPath = (Join-Path $ScoopRoot 'shims\7z.exe'); args = @('i') },
    @{ name = 'clink'; freshCommand = 'clink'; runPath = (Join-Path $ScoopRoot 'shims\clink.cmd'); args = @('--version') },
    @{ name = 'cmake'; freshCommand = 'cmake'; runPath = (Join-Path $ScoopRoot 'shims\cmake.exe'); args = @('--version') },
    @{ name = 'fastfetch'; freshCommand = 'fastfetch'; runPath = (Join-Path $ScoopRoot 'shims\fastfetch.exe'); args = @('--version') },
    @{ name = 'fd'; freshCommand = 'fd'; runPath = (Join-Path $ScoopRoot 'shims\fd.exe'); args = @('--version') },
    @{ name = 'fzf'; freshCommand = 'fzf'; runPath = (Join-Path $ScoopRoot 'shims\fzf.exe'); args = @('--version') },
    @{ name = 'gh'; freshCommand = 'gh'; runPath = (Join-Path $ScoopRoot 'shims\gh.exe'); args = @('--version') },
    @{ name = 'git'; freshCommand = 'git'; runPath = (Join-Path $ScoopRoot 'shims\git.exe'); args = @('--version') },
    @{ name = 'innounp'; freshCommand = 'innounp'; runPath = (Join-Path $ScoopRoot 'shims\innounp.exe'); args = @() },
    @{ name = 'ninja'; freshCommand = 'ninja'; runPath = (Join-Path $ScoopRoot 'shims\ninja.exe'); args = @('--version') },
    @{ name = 'pi-coding-agent'; freshCommand = 'pi'; runPath = (Join-Path $ScoopRoot 'shims\pi.exe'); args = @('--version') },
    @{ name = 'ripgrep'; freshCommand = 'rg'; runPath = (Join-Path $ScoopRoot 'shims\rg.exe'); args = @('--version') },
    @{ name = 'starship'; freshCommand = 'starship'; runPath = (Join-Path $ScoopRoot 'apps\starship\current\starship.exe'); args = @('--version') },
    @{ name = 'uv'; freshCommand = 'uv'; runPath = (Join-Path $ScoopRoot 'shims\uv.exe'); args = @('--version') },
    @{ name = 'zoxide'; freshCommand = 'zoxide'; runPath = (Join-Path $ScoopRoot 'shims\zoxide.exe'); args = @('--version') }
)
$inventoryNames = @($inventoryJson.name | Sort-Object)
$descriptorNames = @($descriptors.name | Sort-Object)
Add-Check 'baseline' 'queue-matches-installed-set' ((($inventoryNames -join ',') -eq ($descriptorNames -join ','))) "installed=$($inventoryNames -join ',') descriptors=$($descriptorNames -join ',')"
Add-Check 'baseline' 'scoop-and-burrow-see-same-set' ((@($scoopBefore.map.Keys | Sort-Object) -join ',') -eq ($inventoryNames -join ',')) "scoop=$(@($scoopBefore.map.Keys | Sort-Object) -join ',')"

$selected = if ($OnlyPackages.Count -gt 0) { @($descriptors | Where-Object { $OnlyPackages -contains $_.name }) } else { $descriptors }
$factsList = @()
foreach ($descriptor in $selected) {
    Write-Host "`n=== facts: $($descriptor.name) ==="
    $factsList += Get-ItemFacts $descriptor
}
$factsList | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runDir 'facts-all.json') -Encoding utf8

# ---------------------------------------------------------------------------
# Alternating lifecycle
# ---------------------------------------------------------------------------
$lifecycleRecords = @()
if ($Lifecycle) {
    foreach ($facts in $factsList) {
        $descriptor = $descriptors | Where-Object { $_.name -eq $facts.package } | Select-Object -First 1
        if ($SkipLifecyclePackages -contains $facts.package) {
            $lifecycleRecords += [pscustomobject]@{
                package = $facts.package
                steps   = @([pscustomobject]@{
                        step   = 'scoop-reset'
                        status = 'not-run'
                        reason = 'excluded: the running executor depends on this package entry'
                    })
            }
            continue
        }
        Write-Host "`n=== lifecycle: $($facts.package) (Scoop reset) ==="
        $reset = Scoop 'scoop-reset' @('reset', $facts.package)
        Add-Check "lifecycle/$($facts.package)/scoop-reset" 'scoop-reset-exit' ($reset.exit -eq 0) "exit=$($reset.exit)"
        $afterScoop = Test-PackageAfterChange $facts $descriptor 'scoop-reset' $facts.burrow_version
        $lifecycleRecords += [pscustomobject]@{ package = $facts.package; steps = @([pscustomobject]@{ step = 'scoop-reset'; status = if ($reset.exit -eq 0) { 'met' } else { 'not-met' }; scoop_exit = $reset.exit; verification = $afterScoop }) }
    }

    foreach ($packageName in $BurrowResetPackages) {
        $facts = $factsList | Where-Object { $_.package -eq $packageName } | Select-Object -First 1
        if (-not $facts) { continue }
        $descriptor = $descriptors | Where-Object { $_.name -eq $packageName } | Select-Object -First 1
        $retained = Get-RetainedVersion $facts
        if (-not $retained) {
            Add-Check "lifecycle/$packageName/burrow-reset" 'retained-local-version' $false 'no retained local version to switch to'
            $lifecycleRecords += [pscustomobject]@{ package = $packageName; steps = @([pscustomobject]@{ step = 'burrow-reset-roundtrip'; status = 'not-run'; reason = 'no retained local version' }) }
            continue
        }
        Write-Host "`n=== lifecycle: $packageName (Burrow reset $retained then back to $($facts.burrow_version)) ==="
        $steps = @()
        $toOld = Burrow 'burrow-reset-old' @('reset', ($packageName + '@' + $retained), '--json') (Join-Path $runDir (Join-Path 'lifecycle' $packageName))
        Add-Check "lifecycle/$packageName/burrow-reset" 'switch-to-retained-version' ($toOld.exit -eq 0) "target=$retained exit=$($toOld.exit)"
        if ($toOld.exit -eq 0) {
            $oldVerify = Test-PackageAfterChange $facts $descriptor 'burrow-reset-old' $retained
            $scoopResetOld = Scoop 'scoop-reset' @('reset', $packageName) (Join-Path $runDir (Join-Path (Join-Path 'lifecycle' $packageName) 'burrow-reset-old'))
            Add-Check "lifecycle/$packageName/burrow-reset" 'scoop-reset-on-retained-version' ($scoopResetOld.exit -eq 0) "exit=$($scoopResetOld.exit)"
            $scoopOldVerify = Test-PackageAfterChange $facts $descriptor 'burrow-reset-old-scoop' $retained
            $steps += [pscustomobject]@{ step = 'switch-to-' + $retained; status = 'met'; burrow_exit = $toOld.exit; burrow_verify = $oldVerify; scoop_verify = $scoopOldVerify }
            $back = Burrow 'burrow-reset-current' @('reset', ($packageName + '@' + $facts.burrow_version), '--json') (Join-Path $runDir (Join-Path 'lifecycle' $packageName))
            Add-Check "lifecycle/$packageName/burrow-reset" 'switch-back-to-current' ($back.exit -eq 0) "target=$($facts.burrow_version) exit=$($back.exit)"
            if ($back.exit -eq 0) {
                $currentVerify = Test-PackageAfterChange $facts $descriptor 'burrow-reset-current' $facts.burrow_version
                $scoopResetCurrent = Scoop 'scoop-reset' @('reset', $packageName) (Join-Path $runDir (Join-Path (Join-Path 'lifecycle' $packageName) 'burrow-reset-current'))
                Add-Check "lifecycle/$packageName/burrow-reset" 'scoop-reset-on-current-version' ($scoopResetCurrent.exit -eq 0) "exit=$($scoopResetCurrent.exit)"
                $finalVerify = Test-PackageAfterChange $facts $descriptor 'burrow-reset-current-scoop' $facts.burrow_version
                $steps += [pscustomobject]@{ step = 'switch-back-to-current'; status = 'met'; burrow_exit = $back.exit; burrow_verify = $currentVerify; scoop_verify = $finalVerify }
            }
            else {
                $steps += [pscustomobject]@{ step = 'switch-back-to-current'; status = 'not-met'; burrow_exit = $back.exit; reason = 'Burrow refused or failed to switch back; environment left on retained version' }
            }
        }
        else {
            $diagnostic = try { (($toOld.output | ConvertFrom-Json).diagnostics | ForEach-Object { $_.message }) -join ' | ' } catch { $toOld.output }
            $steps += [pscustomobject]@{ step = 'switch-to-' + $retained; status = 'blocked'; burrow_exit = $toOld.exit; reason = $diagnostic }
        }
        $lifecycleRecords += [pscustomobject]@{ package = $packageName; steps = $steps }
    }

    if ($UpdateClinkViaBurrow) {
        $facts = $factsList | Where-Object { $_.package -eq 'clink' } | Select-Object -First 1
        $descriptor = $descriptors | Where-Object { $_.name -eq 'clink' }
        $logDir = Join-Path $runDir 'lifecycle\clink\burrow-update'
        New-Item -ItemType Directory -Path $logDir -Force | Out-Null
        Write-Host "`n=== lifecycle: clink real update through Burrow ==="
        $preview = Burrow 'burrow-update-preview' @('update', 'clink', '--dry-run', '--json') $logDir
        $previewJson = try { $preview.output | ConvertFrom-Json } catch { $null }
        $targetVersion = if ($previewJson) { $previewJson.package.version } else { $null }
        Add-Check 'lifecycle/clink/burrow-update' 'preview-executable' ([bool]($previewJson -and $previewJson.execution_available)) "target=$targetVersion exit=$($preview.exit)"
        $update = Burrow 'burrow-update' @('update', 'clink', '--json') $logDir
        Add-Check 'lifecycle/clink/burrow-update' 'update-applied' ($update.exit -eq 0 -and $targetVersion -and $targetVersion -ne $facts.burrow_version) "exit=$($update.exit) from=$($facts.burrow_version) to=$targetVersion"
        if ($update.exit -eq 0) {
            $journalPath = try { ($update.output | ConvertFrom-Json).journal } catch { $null }
            $verify = Test-PackageAfterChange $facts $descriptor 'burrow-update' $targetVersion
            $scoopReset = Scoop 'scoop-reset' @('reset', 'clink') $logDir
            Add-Check 'lifecycle/clink/burrow-update' 'scoop-reset-after-burrow-update' ($scoopReset.exit -eq 0) "exit=$($scoopReset.exit)"
            $scoopVerify = Test-PackageAfterChange $facts $descriptor 'burrow-update-scoop' $targetVersion
            $retainedOld = Test-Path -LiteralPath (Join-Path $ScoopRoot ('apps\clink\' + $facts.burrow_version))
            Add-Check 'lifecycle/clink/burrow-update' 'previous-version-retained' $retainedOld (Join-Path $ScoopRoot ('apps\clink\' + $facts.burrow_version))
            $lifecycleRecords += [pscustomobject]@{
                package = 'clink'
                steps   = @([pscustomobject]@{
                        step          = 'burrow-real-update'
                        status        = 'met'
                        from          = $facts.burrow_version
                        to            = $targetVersion
                        journal       = $journalPath
                        previous_version_retained = $retainedOld
                        burrow_verify   = $verify
                        scoop_verify  = $scoopVerify
                    })
            }
        }
        else {
            $lifecycleRecords += [pscustomobject]@{ package = 'clink'; steps = @([pscustomobject]@{ step = 'burrow-real-update'; status = 'not-met'; burrow_exit = $update.exit; reason = $update.output.Trim() }) }
        }
    }

    if ($UpdateGitViaScoop) {
        $facts = $factsList | Where-Object { $_.package -eq 'git' } | Select-Object -First 1
        $descriptor = $descriptors | Where-Object { $_.name -eq 'git' }
        $logDir = Join-Path $runDir 'lifecycle\git\scoop-update'
        New-Item -ItemType Directory -Path $logDir -Force | Out-Null
        Write-Host "`n=== lifecycle: git real update through Scoop ==="
        $status = Scoop 'scoop-status' @('status') $logDir
        $latest = $null
        foreach ($line in ($status.output -split "`r?`n")) {
            if ($line -match ('^' + [regex]::Escape('git') + '\s+(?<installed>\S+)\s+(?<latest>\S+)')) {
                $latest = $matches.latest
                break
            }
        }
        Add-Check 'lifecycle/git/scoop-update' 'scoop-sees-an-update' ([bool]($latest -and $latest -ne $facts.burrow_version)) "installed=$($facts.burrow_version) latest=$latest"
        $update = Scoop 'scoop-update' @('update', 'git') $logDir
        Add-Check 'lifecycle/git/scoop-update' 'scoop-update-applied' ($update.exit -eq 0 -and $latest -and $latest -ne $facts.burrow_version) "exit=$($update.exit) target=$latest"
        if ($update.exit -eq 0) {
            $burrowReset = Burrow 'burrow-reset-git' @('reset', ('git@' + $latest), '--json') $logDir
            # Scoop has already published $latest, so Burrow's reset is expected to report that nothing
            # would change. Record that outcome explicitly; any other refusal is a failure.
            $resetAlreadyCurrent = $burrowReset.exit -ne 0 -and $burrowReset.output -match 'already current'
            Add-Check 'lifecycle/git/scoop-update' 'burrow-reset-outcome' ($burrowReset.exit -eq 0 -or $resetAlreadyCurrent) "exit=$($burrowReset.exit) already_current=$resetAlreadyCurrent"
            $verify = Test-PackageAfterChange $facts $descriptor 'scoop-update' $latest
            Add-Check 'lifecycle/git/scoop-update' 'burrow-reads-updated-install' ($verify.burrow_version -eq $latest) "burrow=$($verify.burrow_version) expected=$latest"
            $lifecycleRecords += [pscustomobject]@{
                package = 'git'
                steps   = @([pscustomobject]@{
                        step        = 'scoop-real-update'
                        status      = 'met'
                        from        = $facts.burrow_version
                        to          = $latest
                        scoop_exit  = $update.exit
                        burrow_reset  = [pscustomobject]@{ exit = $burrowReset.exit; already_current = $resetAlreadyCurrent }
                        burrow_verify = $verify
                    })
            }
        }
        else {
            $lifecycleRecords += [pscustomobject]@{ package = 'git'; steps = @([pscustomobject]@{ step = 'scoop-real-update'; status = 'not-met'; scoop_exit = $update.exit; reason = $update.output.Trim() }) }
        }
    }

    if ($ReinstallPackage) {
        $facts = $factsList | Where-Object { $_.package -eq $ReinstallPackage } | Select-Object -First 1
        $descriptor = $descriptors | Where-Object { $_.name -eq $ReinstallPackage } | Select-Object -First 1
        if ($facts -and $descriptor) {
            $logDir = Join-Path $runDir 'lifecycle\reinstall'
            New-Item -ItemType Directory -Path $logDir -Force | Out-Null
            Write-Host "`n=== lifecycle: $ReinstallPackage uninstall + Burrow install ==="
            $remove = Burrow 'burrow-uninstall' @('uninstall', $ReinstallPackage, '--json') $logDir
            $currentAfterRemove = Join-Path $ScoopRoot ('apps\' + $ReinstallPackage + '\current')
            $scoopAfterRemove = Get-ScoopInstalled $logDir
            $scoopRowVersion = if ($scoopAfterRemove.map[$ReinstallPackage]) { $scoopAfterRemove.map[$ReinstallPackage].version } else { $null }
            $shimAfterRemove = @(Get-ShimVariants $descriptor.freshCommand)
            $removalRecognized = $remove.exit -eq 0 -and -not (Test-Path -LiteralPath $currentAfterRemove) -and $shimAfterRemove.Count -eq 0
            Add-Check 'lifecycle/reinstall' 'burrow-uninstall-removes-current-and-entries' $removalRecognized "exit=$($remove.exit) current=$(Test-Path -LiteralPath $currentAfterRemove) shims=$($shimAfterRemove.Count)"
            # Scoop's own list selects a retained version directory when no current version exists,
            # so the row (if any) must no longer carry the uninstalled version.
            Add-Check 'lifecycle/reinstall' 'scoop-no-longer-reports-uninstalled-version' ($scoopRowVersion -ne $facts.burrow_version) "scoop-row=$scoopRowVersion uninstalled=$($facts.burrow_version)"
            $install = Burrow 'burrow-install' @('install', $ReinstallPackage, '--json') $logDir
            Add-Check 'lifecycle/reinstall' 'burrow-install-succeeds' ($install.exit -eq 0) "exit=$($install.exit)"
            if ($install.exit -eq 0) {
                $verify = Test-PackageAfterChange $facts $descriptor 'burrow-reinstall' $facts.burrow_version
                $scoopReset = Scoop 'scoop-reset' @('reset', $ReinstallPackage) $logDir
                Add-Check 'lifecycle/reinstall' 'scoop-reset-after-burrow-install' ($scoopReset.exit -eq 0) "exit=$($scoopReset.exit)"
                $scoopVerify = Test-PackageAfterChange $facts $descriptor 'burrow-reinstall-scoop' $facts.burrow_version
                $standardManifest = Join-Path $ScoopRoot ('apps\' + $ReinstallPackage + '\current\scoop-manifest.json')
                if (-not (Test-Path -LiteralPath $standardManifest)) { $standardManifest = Join-Path $ScoopRoot ('apps\' + $ReinstallPackage + '\current\manifest.json') }
                $standardInstallRecord = Join-Path $ScoopRoot ('apps\' + $ReinstallPackage + '\current\scoop-install.json')
                Add-Check 'lifecycle/reinstall' 'standard-install-record' (Test-Path -LiteralPath $standardManifest) $standardManifest
                Add-Check 'lifecycle/reinstall' 'standard-install-metadata' ((Test-Path -LiteralPath $standardInstallRecord) -or (Test-Path -LiteralPath (Join-Path $ScoopRoot ('apps\' + $ReinstallPackage + '\current\install.json')))) $standardInstallRecord
                $lifecycleRecords += [pscustomobject]@{
                    package = $ReinstallPackage
                    steps   = @([pscustomobject]@{
                            step           = 'burrow-uninstall-then-install'
                            status         = 'met'
                            uninstall_exit = $remove.exit
                            install_exit   = $install.exit
                            burrow_verify    = $verify
                            scoop_verify   = $scoopVerify
                        })
                }
            }
            else {
                $lifecycleRecords += [pscustomobject]@{ package = $ReinstallPackage; steps = @([pscustomobject]@{ step = 'burrow-reinstall'; status = 'not-met'; reason = $install.output.Trim() }) }
            }
        }
    }

    if ($ScoopRemovePackage) {
        $facts = $factsList | Where-Object { $_.package -eq $ScoopRemovePackage } | Select-Object -First 1
        $descriptor = $descriptors | Where-Object { $_.name -eq $ScoopRemovePackage } | Select-Object -First 1
        if ($facts -and $descriptor) {
            $logDir = Join-Path $runDir 'lifecycle\scoop-remove'
            New-Item -ItemType Directory -Path $logDir -Force | Out-Null
            Write-Host "`n=== lifecycle: $ScoopRemovePackage removed by Scoop, installed again by Burrow ==="
            $currentEntry = (Get-BurrowEntry $ScoopRemovePackage $logDir).entry
            $expected = $currentEntry.version
            $currentPath = Join-Path $ScoopRoot ('apps\' + $ScoopRemovePackage + '\current')
            $burrowEntry = @('.cmd','.shim') | Where-Object { Test-Path -LiteralPath (Join-Path $ScoopRoot ('shims\' + $descriptor.freshCommand + $_)) } | Select-Object -First 1
            Add-Check 'lifecycle/scoop-remove' 'burrow-published-shared-entry' ($null -ne $burrowEntry) "$($descriptor.freshCommand)$burrowEntry version=$expected"
            $remove = Scoop 'scoop-uninstall' @('uninstall', $ScoopRemovePackage) $logDir
            $remainingShims = @(Get-ShimVariants $descriptor.freshCommand)
            Add-Check 'lifecycle/scoop-remove' 'scoop-uninstall-removes-shared-entry' ($remove.exit -eq 0 -and -not (Test-Path -LiteralPath $currentPath) -and $remainingShims.Count -eq 0) "exit=$($remove.exit) current=$(Test-Path -LiteralPath $currentPath) shims=$($remainingShims.Count)"
            $afterRemove = (Get-BurrowEntry $ScoopRemovePackage $logDir).entry
            Add-Check 'lifecycle/scoop-remove' 'burrow-reidentifies-removal' ($null -eq $afterRemove) 'Burrow still lists the removed package'
            $install = Burrow 'burrow-install' @('install', $ScoopRemovePackage, '--json') $logDir
            Add-Check 'lifecycle/scoop-remove' 'burrow-reinstall-after-scoop-removal' ($install.exit -eq 0) "exit=$($install.exit)"
            if ($install.exit -eq 0) {
                $verify = Test-PackageAfterChange $facts $descriptor 'scoop-remove-reinstall' $expected
                $scoopReset = Scoop 'scoop-reset' @('reset', $ScoopRemovePackage) $logDir
                Add-Check 'lifecycle/scoop-remove' 'scoop-reset-after-reinstall' ($scoopReset.exit -eq 0) "exit=$($scoopReset.exit)"
                $scoopVerify = Test-PackageAfterChange $facts $descriptor 'scoop-remove-reinstall-scoop' $expected
                $lifecycleRecords += [pscustomobject]@{
                    package = $ScoopRemovePackage
                    steps   = @([pscustomobject]@{
                            step       = 'scoop-uninstall-then-burrow-install'
                            status     = 'met'
                            scoop_exit = $remove.exit
                            burrow_exit  = $install.exit
                            burrow_verify = $verify
                            scoop_verify = $scoopVerify
                        })
                }
            }
            else {
                $lifecycleRecords += [pscustomobject]@{ package = $ScoopRemovePackage; steps = @([pscustomobject]@{ step = 'scoop-uninstall-then-burrow-install'; status = 'not-met'; scoop_exit = $remove.exit; reason = $install.output.Trim() }) }
            }
        }
    }
}

$lifecycleRecords | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $runDir 'lifecycle.json') -Encoding utf8

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
$failed = @($script:Checks | Where-Object { -not $_.ok })
$perItem = foreach ($facts in $factsList) {
    $itemChecks = @($script:Checks | Where-Object { $_.scope -eq ('facts/' + $facts.package) })
    $lifecycleSteps = @($lifecycleRecords | Where-Object { $_.package -eq $facts.package } | ForEach-Object { $_.steps })
    $itemFailed = @($itemChecks | Where-Object { -not $_.ok })
    [pscustomobject]@{
        package          = $facts.package
        version          = $facts.burrow_version
        facts_checks     = $itemChecks.Count
        facts_failed     = $itemFailed.Count
        lifecycle_steps  = @($lifecycleSteps | ForEach-Object { $_.step + '=' + $_.status }) -join ','
        status           = if ($itemFailed.Count -eq 0) { 'met' } else { 'not-met' }
    }
}
$summary = [ordered]@{
    run               = $runDir
    lifecycle         = [bool]$Lifecycle
    package_count     = $factsList.Count
    checks            = $script:Checks.Count
    failures          = $failed.Count
    failed_checks     = @($failed | ForEach-Object { [pscustomobject]@{ scope = $_.scope; name = $_.name; detail = $_.detail } })
    items             = @($perItem)
    commands          = $script:Commands.Count
    status            = if ($failed.Count -eq 0) { 'pass' } else { 'fail' }
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runDir 'summary.json') -Encoding utf8
$script:Commands | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDir 'commands.json') -Encoding utf8

Write-Host ("`nchecks={0} failures={1} evidence={2}" -f $script:Checks.Count, $failed.Count, $runDir)
if ($failed.Count -gt 0) {
    Write-Host 'failed checks:'
    $failed | ForEach-Object { Write-Host ("  {0}: {1} ({2})" -f $_.scope, $_.name, $_.detail) }
    exit 1
}
exit 0
