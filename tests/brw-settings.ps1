# Red when: a registry preview writes, recovery overwrites a newer user edit, or an unauthorized value can change.
param([string] $Brw, [string] $Scratch, [switch] $UserRegistry)
$ErrorActionPreference = 'Stop'
$root = Join-Path $Scratch ([guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
Remove-Item Env:BRW_TEST_REGISTRY_ID,Env:BRW_TEST_HIVE -ErrorAction SilentlyContinue
$testKey = $null
if ($UserRegistry) {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run the dedicated HKCU transaction test without elevation'
    }
    $id = [guid]::NewGuid().ToString('N')
    $leaf = "Burrow-Tests-$id"
    $candidate = "Software\$leaf"
    $hive = [Microsoft.Win32.Registry]::CurrentUser
    if ($hive.OpenSubKey($candidate)) { throw 'Test key already exists' }
    $container = $hive.OpenSubKey('Software', [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
                                  [Security.AccessControl.RegistryRights]::CreateSubKey)
    try { $fixture = $container.CreateSubKey($leaf); $fixture.Close() }
    finally { $container.Close() }
    $testKey = $candidate
    $env:BRW_TEST_REGISTRY_ID = $id
} else {
    $env:BRW_TEST_HIVE = Join-Path $root 'settings.hiv'
}
try {
$env:XDG_STATE_HOME = Join-Path $root 'state'
$key = 'HKCU\Software\Burrow\Settings'
function Run([int] $code, [string[]] $arguments) {
    $output = & $Brw @arguments --json
    if ($LASTEXITCODE -ne $code) { throw "Unexpected exit $LASTEXITCODE for $arguments ($output)" }
    if ($output) { $output | ConvertFrom-Json }
}
function Check($condition, [string] $why) { if (!$condition) { throw $why } }
$set = @('registry','set',$key,'Profile','REG_SZ','one','--expect-missing')
Check ((Run 0 ($set + '--dry-run')).status -eq 'planned') 'Preview failed'
Check ($null -eq (Run 0 @('registry','get',$key,'Profile','REG_SZ')).value) 'Preview wrote the value'
Check (!(Test-Path $env:XDG_STATE_HOME)) 'Preview created state'
$env:BRW_TEST_INTERRUPT = 'settings'
try { Run 86 $set } finally { Remove-Item Env:BRW_TEST_INTERRUPT }
Check ((Run 0 @('registry','get',$key,'Profile','REG_SZ')).value -ceq 'one') 'Interrupted write missing'
Check ((Run 0 @('recover')).status -eq 'recovered') 'Recovery failed'
Check ($null -eq (Run 0 @('registry','get',$key,'Profile','REG_SZ')).value) 'Recovery lost missing state'
Check ((Run 0 @('recover')).status -eq 'no-pending') 'Second recovery not empty'

# Exercise a resolved state directory at the old GetTempFileName limit.
$shortState = $env:XDG_STATE_HOME
$env:XDG_STATE_HOME = Join-Path $root ('s' * [Math]::Max(1, 238 - $root.Length))
try { $null = Run 0 $set } finally { $env:XDG_STATE_HOME = $shortState }
function Reject([string] $expected, [string[]] $arguments) {
    $failure = Run 1 $arguments
    Check ($failure.error -ceq $expected) "Expected $expected for $arguments, got $($failure.error)"
}
Reject unauthorized-key @('registry','set','HKLM\Software\Burrow\Settings','Profile','REG_SZ','bad','--expect','one')
Reject unauthorized-key @('registry','remove',"$key\child",'Profile','REG_SZ','--expect','one')
Reject unauthorized-value @('registry','set',$key,'Other','REG_SZ','bad','--expect-missing')
Reject value-type @('registry','set',$key,'Profile','REG_DWORD','3','--expect-missing')
Reject old-value-conflict @('registry','set',$key,'Profile','REG_SZ','bad','--expect','wrong')
Reject invalid-precondition @('registry','remove',$key,'Profile','REG_SZ')
Check ((Run 0 @('registry','get',$key,'Profile','REG_SZ')).value -ceq 'one') 'Rejected request changed state'
$enable = @('registry','set',$key,'Enabled','REG_DWORD')
Check ((Run 1 ($enable + @('4294967296','--expect-missing'))).error -eq 'invalid-dword') 'Overflow accepted'
$null = Run 0 ($enable + @('4294967295','--expect-missing'))
if ($UserRegistry) {
    $actual = $hive.OpenSubKey("$testKey\Software\Burrow\Settings")
    try {
        Check ($actual.GetValueKind('Enabled') -eq 'DWord' -and $actual.GetValue('Enabled') -eq -1) 'Raw DWORD differs'
        Check ($actual.GetValueKind('Profile') -eq 'String' -and $actual.GetValue('Profile') -ceq 'one') 'Raw string differs'
    } finally { $actual.Close() }
}
Check ((Run 0 @('registry','get',$key,'Enabled','REG_DWORD')).value -eq 4294967295) 'DWORD did not round-trip'
$null = Run 0 @('registry','remove',$key,'Enabled','REG_DWORD','--expect','4294967295')
Check ($null -eq (Run 0 @('registry','get',$key,'Enabled','REG_DWORD')).value) 'DWORD deletion failed'

$env:BRW_TEST_INTERRUPT = 'settings'
try { Run 86 @('registry','remove',$key,'Profile','REG_SZ','--expect','one') }
finally { Remove-Item Env:BRW_TEST_INTERRUPT }
Check ((Run 0 @('recover','--dry-run')).status -eq 'recoverable') 'Missing pending deletion'
Check ($null -eq (Run 0 @('registry','get',$key,'Profile','REG_SZ')).value) 'Recovery preview restored the value'
# Another writer uses the same hive and an independent state directory.
$originalState = $env:XDG_STATE_HOME
$env:XDG_STATE_HOME = Join-Path $root 'other-state'
try { $null = Run 0 @('registry','set',$key,'Profile','REG_SZ','external','--expect-missing') }
finally { $env:XDG_STATE_HOME = $originalState }
Check ((Run 0 @('recover','--dry-run')).status -eq 'would-preserve-user-change') 'Missing user-change diagnostic'
Check ((Run 0 @('recover')).status -eq 'recovered-with-user-changes') 'Missing preservation result'
Check ((Run 0 @('recover')).status -eq 'no-pending') 'User change kept the transaction pending'
Check ((Run 0 @('registry','get',$key,'Profile','REG_SZ')).value -ceq 'external') 'Recovery overwrote user edit'

$null = Run 0 @('env','user','set','BRW_SETTINGS_TEST','persistent','--expect-missing')
$env:BRW_TEST_INTERRUPT = 'settings'
try { Run 86 @('env','user','remove','BRW_SETTINGS_TEST','--expect','persistent') }
finally { Remove-Item Env:BRW_TEST_INTERRUPT }
$null = Run 0 @('recover')
$null = Run 0 @('env','user','remove','BRW_SETTINGS_TEST','--expect','persistent')
$null = Run 0 @('env','user','set','Path','C:\keep;C:\old')
$add = Run 0 @('env','user','path-add','C:\new dir')
Check ($add.value -ceq 'C:\keep;C:\old;C:\new dir') 'User PATH append failed'
$repeat = Run 0 @('env','user','path-add','c:\NEW DIR\.')
Check ($repeat.value -ceq $add.value) 'Duplicate PATH entry'
$remove = Run 0 @('env','user','path-remove','C:\OLD')
Check ($remove.value -ceq 'C:\keep;C:\new dir') 'PATH removal changed unrelated entries'
if ($UserRegistry) {
    $actual = $hive.OpenSubKey("$testKey\Environment")
    try { Check ($actual.GetValue('Path') -ceq $remove.value) 'Fixture user PATH differs' }
    finally { $actual.Close() }
}

$env:BRW_SETTINGS_TEST = 'parent'
$child = @('--',$env:ComSpec,'/d','/c','set BRW_SETTINGS_TEST')
$output = & $Brw env session set BRW_SETTINGS_TEST child @child
Check ($LASTEXITCODE -eq 0 -and $output -ceq 'BRW_SETTINGS_TEST=child') 'Child did not inherit session value'
Check ($env:BRW_SETTINGS_TEST -ceq 'parent') 'Parent inherited session change'
$marker = Join-Path $root 'child-ran'
$env:BRW_CHILD_MARKER = $marker
$markScript = Join-Path $root 'mark.ps1'
[IO.File]::WriteAllText($markScript, '[IO.File]::WriteAllText($env:BRW_CHILD_MARKER, "ran")')
$markChild = @('--', (Get-Process -Id $PID).Path, '-NoProfile','-ExecutionPolicy','Bypass','-File',$markScript)
$null = & $Brw env session remove BRW_SETTINGS_TEST @markChild
Check ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $marker)) 'Child witness failed'
[IO.File]::Delete($marker)
$preview = & $Brw env session remove BRW_SETTINGS_TEST --dry-run --json @markChild | ConvertFrom-Json
Check (!(Test-Path -LiteralPath $marker)) 'Session preview launched the child'
Check ($LASTEXITCODE -eq 0 -and $preview.status -eq 'planned' -and $null -eq $preview.childExitCode) 'Session preview ran child'
$null = & $Brw env session remove BRW_SETTINGS_TEST -- $env:ComSpec /d /c 'if defined BRW_SETTINGS_TEST (exit /b 8) else (exit /b 42)'
Check ($LASTEXITCODE -eq 42) 'Removed variable remained in child or exit code was lost'
$beforePath = $env:Path
$env:Path = 'C:\keep;C:\old'
try {
    $pathChild = @('--',$env:ComSpec,'/d','/c','echo %PATH%')
    $output = & $Brw env session path-remove C:\OLD @pathChild
    Check ($LASTEXITCODE -eq 0 -and $output -ceq 'C:\keep') 'Session PATH removal failed'
    $output = & $Brw env session path-add C:\new @pathChild
    Check ($LASTEXITCODE -eq 0 -and $output -ceq 'C:\keep;C:\old;C:\new') 'Session PATH append failed'
} finally { $env:Path = $beforePath }
} finally {
    if ($testKey) { $hive.DeleteSubKeyTree($testKey) }
}
