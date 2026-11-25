# Red when: clone publishes an incomplete checkout or pull overwrites local changes or diverged history.
param([Parameter(Mandatory)] [string] $Brw, [Parameter(Mandatory)] [string] $Scratch)
$ErrorActionPreference = 'Stop'
$root = Join-Path $Scratch ("git fixture " + [guid]::NewGuid())
$source = Join-Path $root upstream
$tree = Join-Path $root checkout
function RunGit($dir, [string[]] $arguments) {
    $output = & git -C $dir @arguments
    if ($LASTEXITCODE) { throw "git $arguments failed" }
    $output
}
function RunBrw([string[]] $arguments, $expectedError) {
    $output = & $Brw git @arguments --json
    $code = $LASTEXITCODE
    $report = $output | ConvertFrom-Json
    if (!$report -or ($code -eq 0) -ne (!$expectedError) -or $report.error -ne $expectedError) {
        throw "brw git $arguments returned $code $output"
    }
    $report
}
New-Item -Type Directory -Path $source -Force | Out-Null
RunGit $source @('init','-q')
function Commit($dir, $value) {
    Set-Content -Encoding ascii (Join-Path $dir payload) $value
    RunGit $dir @('add','payload')
    RunGit $dir @('-c','user.name=Fixture','-c','user.email=test@example.invalid','commit','-qm',$value)
    RunGit $dir @('rev-parse','HEAD')
}
$first = Commit $source first
$missing = Join-Path $root missing
$report = RunBrw @('clone',$missing,$tree,'--dry-run') $null
if ($report.status -ne 'planned' -or (Test-Path $tree)) { throw 'clone preview wrote the destination' }
RunBrw @('clone',$missing,$tree) 'clone-failed' | Out-Null
if ((Test-Path $tree) -or @(Get-ChildItem $root -Filter 'checkout.brw-*').Count) { throw 'failed clone leaked files' }
$watch = New-Object IO.FileSystemWatcher $root
$watch.NotifyFilter = [IO.NotifyFilters]::DirectoryName
$eventId = "brw-clone-" + [guid]::NewGuid()
Register-ObjectEvent $watch Renamed -SourceIdentifier $eventId | Out-Null
try {
    $watch.EnableRaisingEvents = $true
    $report = RunBrw @('clone',$source,$tree) $null
    $published = Wait-Event -SourceIdentifier $eventId -Timeout 5
    if (!$published -or $published.SourceEventArgs.FullPath -ne $tree -or
        $published.SourceEventArgs.OldFullPath -notlike "$tree.brw-*") { throw 'clone did not publish from staging' }
} finally {
    Unregister-Event -SourceIdentifier $eventId
    $watch.Dispose()
}
if ($report.commit -ne $first -or (RunGit $tree @('rev-parse','HEAD')) -ne $first) {
    throw 'Clone did not publish the checked-out commit'
}
$second = Commit $source second
$report = RunBrw @('pull',$tree,'--dry-run') $null
if ($report.commit -ne $first -or $report.pending -notcontains 'remote-revision') { throw 'pull preview' }
if ((RunGit $tree @('rev-parse','HEAD')) -ne $first -or (Get-Content (Join-Path $tree payload)) -ne 'first') {
    throw 'pull preview changed checkout'
}
$indexLock = Join-Path $tree '.git/index.lock'
New-Item -Type File $indexLock | Out-Null
RunBrw @('pull',$tree) 'index-locked' | Out-Null
if ((RunGit $tree @('rev-parse','HEAD')) -ne $first -or (Get-Content (Join-Path $tree payload)) -ne 'first') {
    throw 'index lock left a partial checkout'
}
Remove-Item -LiteralPath $indexLock
$report = RunBrw @('pull',$tree) $null
if ($report.commit -ne $second -or (RunGit $tree @('rev-parse','HEAD')) -ne $second -or
    (Get-Content (Join-Path $tree payload)) -ne 'second') { throw 'fast-forward checkout' }
if ((RunBrw @('pull',$tree) $null).commit -ne $second) { throw 'unchanged HEAD' }
RunBrw @('clone',$source,$tree) 'destination-exists' | Out-Null
Commit $source third | Out-Null
$file = Join-Path $tree payload
Set-Content -Encoding ascii $file mine
foreach ($kind in 'unstaged','staged') {
    if ($kind -eq 'staged') { RunGit $tree @('add','payload') }
    $before = RunGit $tree @('status','--porcelain')
    RunBrw @('pull',$tree) 'dirty-worktree' | Out-Null
    if ((RunGit $tree @('rev-parse','HEAD')) -ne $second -or (Get-Content $file) -ne 'mine' -or
        (RunGit $tree @('status','--porcelain')) -ne $before) { throw "$kind edits changed" }
}
RunGit $tree @('restore','--source=HEAD','--staged','--worktree','payload')
$extra = Join-Path $tree extra
Set-Content -Encoding ascii $extra untracked
RunBrw @('pull',$tree) 'dirty-worktree' | Out-Null
if ((Get-Content $extra) -ne 'untracked') { throw 'untracked file lost' }
Remove-Item -LiteralPath $extra
$local = Commit $tree local
RunBrw @('pull',$tree) 'non-fast-forward' | Out-Null
if ((RunGit $tree @('rev-parse','HEAD')) -ne $local -or (Get-Content $file) -ne 'local' -or
    (RunGit $tree @('status','--porcelain'))) { throw 'diverged checkout changed' }
$owned = (Resolve-Path -LiteralPath $root).Path
if ((Split-Path -Parent $owned) -ne (Resolve-Path -LiteralPath $Scratch).Path) { throw 'invalid fixture root' }
Remove-Item -LiteralPath $owned -Force -Recurse
