param(
    [string]$Root = (Join-Path $PSScriptRoot '..'),
    [switch]$Staged
)
# Fast, deterministic source hygiene: a UTF-8 byte-order mark or mixed line endings inside one file
# shows up as an invisible rewrite (a three-line change becomes a whole-file diff) and is easy to
# produce by accident when a script writes a file back with the wrong encoding.
#
# Deliberate whole-file line-ending conversion is not detected here: it stays a review concern. The
# aim is to catch the invisible, accidental class that has actually happened.
#
# -Staged reads the bytes git has staged, not the working tree, so a file whose staged content differs
# from disk is checked as it will be committed.
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root)
Push-Location $Root
try {
    $files = if ($Staged) {
        @(git diff --cached --name-only --diff-filter=ACM)
    } else {
        @(git ls-files)
    }
    $addedStaged = if ($Staged) { @(git diff --cached --name-only --diff-filter=A) } else { @() }
} finally {
    Pop-Location
}
if ($LASTEXITCODE -ne 0) { throw 'git could not list files' }

$violations = New-Object System.Collections.ArrayList
foreach ($relative in $files) {
    if (-not $relative) { continue }
    # Staged mode answers one question only: does a file this commit adds declare what reddens it. The
    # encoding and line-ending rules read the working tree, where the bytes are exact; reconstructing
    # them from a pipeline would be a worse approximation of the file than the file.
    if ($Staged) {
        if ($relative -like 'tests/*.ps1' -and ($addedStaged -contains $relative)) {
            $path = Join-Path $Root $relative
            $text = if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllText($path) } else { '' }
            if ($text -notmatch '(?m)^# Red when:') {
                [void]$violations.Add("$relative : a new test needs a '# Red when: <behaviour change>' line")
            }
        }
        continue
    }
    $path = Join-Path $Root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $info = Get-Item -LiteralPath $path
    if ($info.Length -eq 0) { continue }
    $stream = [IO.File]::OpenRead($path)
    try {
        $head = New-Object byte[] ([Math]::Min(8192,$info.Length))
        $read = $stream.Read($head,0,$head.Length)
    } finally { $stream.Dispose() }

    # Binary files are out of scope; a NUL byte in the first block is the usual tell.
    $binary = $false
    for ($i = 0; $i -lt $read; $i++) { if ($head[$i] -eq 0) { $binary = $true; break } }
    if ($binary) { continue }
    $hasBom = ($read -ge 3 -and $head[0] -eq 0xEF -and $head[1] -eq 0xBB -and $head[2] -eq 0xBF)
    if ($hasBom -and $relative -notlike '*.ps1') {
        # A PowerShell script may carry one on purpose (see the rule below); everywhere else a BOM is
        # still the invisible-rewrite accident this check exists to catch.
        [void]$violations.Add("$relative : starts with a UTF-8 byte-order mark")
        continue
    }

    $text = [IO.File]::ReadAllText($path)

    # Windows PowerShell 5.1 parses a BOM-less script as ANSI, so every non-ASCII literal in one turns
    # into mojibake or a parse error. ADR-0008 makes 5.1 the script baseline: such a script needs a BOM.
    if ($relative -like '*.ps1' -and -not $hasBom -and $text -match '[^\x00-\x7F]') {
        [void]$violations.Add("$relative : non-ASCII script without a UTF-8 BOM; Windows PowerShell 5.1 reads it as ANSI")
    }
    $crlf = ([regex]::Matches($text,"`r`n")).Count
    $loneLf = ([regex]::Matches($text,"(?<!`r)`n")).Count
    if ($crlf -gt 0 -and $loneLf -gt 0) {
        [void]$violations.Add("$relative : mixes CRLF ($crlf) and LF ($loneLf) line endings")
    }

    # PowerShell 5.1 can throw when a child writes to redirected stderr.
    if ($relative -like 'tests/*.ps1') {
        $lineNumber = 0
        foreach ($line in ($text -split "`n")) {
            $lineNumber++
            if ($line.TrimStart().StartsWith('#')) { continue }
            if ($line -match '2>&1' -and $line -match '&\s') {
                [void]$violations.Add("$relative : line $lineNumber captures stderr with 2>&1; capture the child process streams directly")
            }
            if ($line -match '2>\s*(?!\$null|&)') {
                [void]$violations.Add("$relative : line $lineNumber redirects stderr to a file; capture the child process streams directly")
            }
        }
    }
}

if ($violations.Count -gt 0) {
    [Console]::Error.WriteLine('Source hygiene check failed:')
    foreach ($violation in $violations) { [Console]::Error.WriteLine("  $violation") }
    [Console]::Error.WriteLine('Rewrite the file without a byte-order mark, or with one consistent line ending.')
    exit 1
}
Write-Output "source hygiene: $($files.Count) files checked"
exit 0
