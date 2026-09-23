# One-command setup of the frontier server (Cloudflare Workers + D1) and the worker binary
# on Windows. A rerun skips the finished steps (state in
# server\.setup-state.json). It runs from any directory:
#     scripts\windows\setup-server.cmd
# or  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\windows\setup-server.ps1
# Options: -OwnerName NAME  -SkipBuild  -MsysRoot C:\msys64  -BlockBits 50  -Campaign frontier-1
#          -MaxBits 60      largest block a worker may ask for (2^MaxBits numbers)
#          -LeaseHours 12   how long a machine may hold a block before it is handed out again
#          -Reset           delete the database and repeat the database steps (the secrets,
#                           the worker token and the Cloudflare login are kept)
#          -AllowDirty      let the campaign list a binary commit that ends in -dirty or -nogit
#                           (campaign.mjs --allow-dirty), and fall back to <sha>-nogit from
#                           .git when collatz.exe cannot report its commit
# server\wrangler.jsonc holds the live settings and is git-ignored. When it is missing, the
# setup copies the template server\wrangler.example.jsonc to it; the template is never
# edited. The setup only replaces single values in wrangler.jsonc (Set-JsoncValue): the
# database id and owner name placeholders. It copies the file to server\wrangler.jsonc.bak
# before the first change of a run.

# PSScriptAnalyzer checks each file on its own: the steps in setup\*.ps1 read these
# parameters and the variables set below.
[Diagnostics.CodeAnalysis.SuppressMessageAttribute("PSReviewUnusedParameter", "")]
[Diagnostics.CodeAnalysis.SuppressMessageAttribute("PSUseDeclaredVarsMoreThanAssignments", "")]
param(
    [string]$OwnerName = "",
    [switch]$SkipBuild,
    [string]$MsysRoot = "C:\msys64",
    [int]$BlockBits = 50,
    [string]$Campaign = "frontier-1",
    [int]$MaxBits = 60,
    [int]$LeaseHours = 12,
    [switch]$Reset,
    [switch]$AllowDirty
)
# Continue, not Stop: Windows PowerShell 5.1 turns any stderr line of a program (wrangler
# prints progress there) into a terminating error under Stop. Exit codes are checked instead.
$ErrorActionPreference = "Continue"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Server = Join-Path $Repo "server"
$StateFile = Join-Path $Server ".setup-state.json"
$Wrangler = Join-Path $Server "wrangler.jsonc"
$WranglerExample = Join-Path $Server "wrangler.example.jsonc"
$TokenFile = Join-Path $env:USERPROFILE "collatz-token.txt"
$Utf8 = New-Object System.Text.UTF8Encoding $false
# Throws on bytes that are not UTF-8, where ReadAllText would turn them into U+FFFD.
$Utf8Strict = New-Object System.Text.UTF8Encoding $false, $true
$script:JsoncBackedUp = $false

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Done($msg) { Write-Host "    $msg" -ForegroundColor DarkGray }
function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# Runs a program and prints its output; a failing program ends the setup. Returns the output lines.
function Run([string]$exe, [string[]]$argv) {
    $out = & $exe @argv 2>&1 | ForEach-Object { "$_" }
    $out | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) { Fail "'$exe $($argv -join ' ')' failed (exit $LASTEXITCODE)" }
    return $out
}

function Load-State {
    if (Test-Path $StateFile) { return Get-Content $StateFile -Raw | ConvertFrom-Json }
    return New-Object PSObject
}
function Save-State($s) { [IO.File]::WriteAllText($StateFile, ($s | ConvertTo-Json), $Utf8) }
function Set-StateValue($s, $name, $value) {
    if ($s.PSObject.Properties[$name]) { $s.$name = $value }
    else { $s | Add-Member -NotePropertyName $name -NotePropertyValue $value }
    Save-State $s
}
function Get-StateValue($s, $name) {
    if ($s.PSObject.Properties[$name]) { return $s.$name }
    return $null
}

# wrangler.jsonc holds the live settings, so it is edited one value at a time and every other
# byte stays as it is. server/test/setup-jsonc.test.mjs runs these functions on copies of
# wrangler.example.jsonc and of live files made from it when pwsh is installed.

# The file's text and whether it starts with a byte order mark, which Write-Jsonc keeps.
# A file that is not UTF-8 is refused rather than written back with U+FFFD in it.
function Read-Jsonc([string]$file) {
    try { $bytes = [IO.File]::ReadAllBytes($file) }
    catch { Fail "cannot read ${file}: $($_.Exception.Message)" }
    $bom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $skip = 0
    if ($bom) { $skip = 3 }
    try { $text = $Utf8Strict.GetString($bytes, $skip, $bytes.Length - $skip) }
    catch { Fail "$file is not UTF-8 text; fix it by hand" }
    return @{ Bom = $bom; Text = $text }
}
function Write-Jsonc([string]$file, [bool]$bom, [string]$text) {
    $bytes = $Utf8.GetBytes($text)
    if ($bom) { $bytes = [byte[]](@(0xEF, 0xBB, 0xBF) + $bytes) }
    [IO.File]::WriteAllBytes($file, $bytes)
}

# The text with each comment turned into spaces (line breaks kept), so that offsets and line
# numbers are those of the file and a key named in a comment is not found.
function Get-JsoncCode([string]$text) {
    return [regex]::Replace($text, '"(?:[^"\\\r\n]|\\.)*"|//[^\r\n]*|/\*[\s\S]*?\*/', {
        param($m)
        if ($m.Value[0] -eq [char]'"') { return $m.Value }
        return [regex]::Replace($m.Value, '[^\r\n]', ' ')
    })
}

# Offsets, in text from Get-JsoncCode, of the bracket that opens the value of the top-level
# key $name and of the one that closes it; $null if there is no such object or array.
function Find-JsoncBlock([string]$code, [string]$name) {
    $depth = 0
    $key = $null
    $colon = $false
    $open = -1
    for ($i = 0; $i -lt $code.Length; $i++) {
        $c = $code[$i]
        if ($c -eq [char]'"') {
            $j = $i + 1
            while ($j -lt $code.Length -and $code[$j] -ne [char]'"') {
                if ($code[$j] -eq [char]'\') { $j++ }
                $j++
            }
            if ($depth -eq 1) { $key = $code.Substring($i + 1, $j - $i - 1); $colon = $false }
            $i = $j
        } elseif ($c -eq [char]':') {
            if ($depth -eq 1) { $colon = $true }
        } elseif ($c -eq [char]'{' -or $c -eq [char]'[') {
            if ($depth -eq 1 -and $open -lt 0 -and $colon -and $key -ceq $name) { $open = $i }
            $depth++
        } elseif ($c -eq [char]'}' -or $c -eq [char]']') {
            $depth--
            if ($depth -eq 1 -and $open -ge 0) { return @($open, $i) }
        } elseif ($c -eq [char]',' -and $depth -eq 1) {
            $key = $null
            $colon = $false
        }
    }
    return $null
}

# "KEY": "VALUE" at the start of a line, after the indentation. Group 1 runs up to the
# value's opening quote, group 2 is the value as written (JSON escapes not decoded).
function Get-JsoncKeyRegex([string]$key) {
    $pattern = '^([ \t]*"' + [regex]::Escape($key) + '"[ \t]*:[ \t]*")((?:[^"\\\r\n]|\\.)*)"'
    return New-Object System.Text.RegularExpressions.Regex ($pattern, [System.Text.RegularExpressions.RegexOptions]::Multiline)
}

# $s escaped for the inside of a JSON string literal.
function Format-JsonString([string]$s) {
    $s = $s.Replace('\', '\\').Replace('"', '\"')
    return [regex]::Replace($s, '[\x00-\x1f]', { param($m) '\u{0:x4}' -f [int][char]$m.Value })
}

# The value of "KEY" in the top-level block $block (first one), as written between the
# quotes, or $null when the block or the key is missing.
function Get-JsoncValue([string]$file, [string]$key, [string]$block = "vars") {
    $code = Get-JsoncCode (Read-Jsonc $file).Text
    $span = Find-JsoncBlock $code $block
    if ($null -eq $span) { return $null }
    $m = (Get-JsoncKeyRegex $key).Match($code.Substring($span[0], $span[1] - $span[0] + 1))
    if ($m.Success) { return $m.Groups[2].Value }
    return $null
}

# Sets the string value of "KEY" in place: only the characters between its quotes change,
# so comments, indentation, tabs, line endings and a byte order mark stay. The key is looked
# for at the start of a line in the top-level block -Block ("vars" unless given) and must be
# there: a missing key is an error and is never appended. Without -From the first "KEY" of
# the block changes; with -From each "KEY" whose value is -From does and no other (every D1
# binding that carries the database id). The first change of a run copies the file to
# wrangler.jsonc.bak, and each change prints one line. -WhatIf prints the change and writes
# nothing. Returns whether the file changed.
function Set-JsoncValue {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true)][string]$File,
        [Parameter(Mandatory = $true)][string]$Key,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value,
        [string]$Block = "vars",
        [string]$From
    )
    if ($Key -cnotmatch '^[A-Za-z_][A-Za-z0-9_]*$') { Fail "Set-JsoncValue: bad key '$Key'" }
    $name = Split-Path $File -Leaf
    $doc = Read-Jsonc $File
    $code = Get-JsoncCode $doc.Text
    $span = Find-JsoncBlock $code $Block
    if ($null -eq $span) { Fail "$name has no top-level `"$Block`"; fix it by hand" }
    $found = @((Get-JsoncKeyRegex $Key).Matches($code.Substring($span[0], $span[1] - $span[0] + 1)))
    if ($found.Count -eq 0) { Fail "$name has no `"$Key`": `"...`" line in `"$Block`"; add it by hand" }
    $new = Format-JsonString $Value
    if ($PSBoundParameters.ContainsKey("From")) {
        $old = Format-JsonString $From
        $found = @($found | Where-Object { $_.Groups[2].Value -ceq $old })
    } else {
        $found = @($found[0])
    }
    $found = @($found | Where-Object { $_.Groups[2].Value -cne $new })
    if ($found.Count -eq 0) { return $false }
    $text = $doc.Text
    $lines = @()
    # Last match first, so the offsets of the earlier ones still hold.
    for ($k = $found.Count - 1; $k -ge 0; $k--) {
        $g = $found[$k].Groups[2]
        $at = $span[0] + $g.Index
        $text = $text.Substring(0, $at) + $new + $text.Substring($at + $g.Length)
        $lines = @([regex]::Matches($doc.Text.Substring(0, $at), "`n").Count + 1) + $lines
    }
    $change = "$name line $($lines -join ', '): `"$Key`": `"$($found[0].Groups[2].Value)`" -> `"$new`""
    if (-not $PSCmdlet.ShouldProcess($File, "set `"$Key`" to `"$new`"")) {
        Write-Host "    would change $change"
        return $false
    }
    # Under $ErrorActionPreference Continue a failed copy or write would not stop the setup,
    # which would then report and deploy a change that is not in the file.
    try {
        if (-not $script:JsoncBackedUp) {
            [IO.File]::Copy($File, "$File.bak", $true)
            $script:JsoncBackedUp = $true
        }
        Write-Jsonc $File $doc.Bom $text
    } catch { Fail "cannot write ${File}: $($_.Exception.Message)" }
    Write-Host "    changed $change"
    return $true
}

# The steps are functions in setup\*.ps1. Each is dot-called, so it runs in the scope of this
# script and the variables it sets, such as $dbId, $url and $commit, stay set for the steps
# after it.
. (Join-Path $PSScriptRoot "setup\prepare.ps1")
. (Join-Path $PSScriptRoot "setup\deploy.ps1")
. (Join-Path $PSScriptRoot "setup\campaign.ps1")

$state = Load-State
Write-Host "Collatz frontier setup, repo: $Repo"
if ($Reset) {
    . Invoke-DatabaseReset
}
. Assert-NodeVersion

Step "npm install (server)"
Push-Location $Server
try {
    . Install-ServerPackage
    . Connect-Cloudflare
    . Initialize-Database
    . Edit-WranglerConfig
    . Initialize-DatabaseTable
    . Add-ServerSecret
    . Publish-Server
} finally { Pop-Location }

$exe = Join-Path $Repo "build\windows-release\collatz.exe"
$bash = Join-Path $MsysRoot "usr\bin\bash.exe"
if (-not $SkipBuild) {
    . Invoke-CollatzBuild
}
. Resolve-BinaryCommit

Push-Location $Server
try {
    . Initialize-Campaign
} finally { Pop-Location }
. Show-Summary
