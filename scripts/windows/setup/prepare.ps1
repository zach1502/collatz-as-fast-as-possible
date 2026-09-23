# The steps of setup-server.ps1 before the secrets: -Reset, Node.js, the server's npm
# packages, the Cloudflare login, the D1 database, wrangler.jsonc and the database tables.
# setup-server.ps1 dot-sources this file and dot-calls each step, so a step reads and sets
# the variables of setup-server.ps1. Install-ServerPackage and the steps after it run in
# server\ (setup-server.ps1 pushes the location).

# `wrangler d1 list --json`, parsed; tolerant of a banner line before the JSON.
function Get-D1List {
    $raw = (& npx wrangler d1 list --json 2>$null | Out-String)
    $i = $raw.IndexOf("[")
    if ($i -lt 0) { return @() }
    return $raw.Substring($i) | ConvertFrom-Json
}

function Invoke-DatabaseReset {
    Step "reset: delete the database collatz-frontier"
    $ans = Read-Host "This deletes the database and everything in it (blocks, statistics). Continue? [y/N]"
    if ($ans -notmatch '^[Yy]') { Fail "reset cancelled" }
    Push-Location $Server
    try {
        & npx wrangler d1 delete collatz-frontier -y 2>&1 | ForEach-Object { Write-Host "    $_" }
    } finally { Pop-Location }
    # `d1 delete collatz-frontier` removes the database wrangler.jsonc names, so its id goes
    # back to the template's placeholder, in every D1 binding that carries it and nowhere
    # else; the database steps below fill in the new one. A missing wrangler.jsonc is made
    # from the template there.
    if (Test-Path $Wrangler -PathType Leaf) {
        $old = Get-JsoncValue $Wrangler "database_id" "d1_databases"
        if ($old -and $old -cnotlike "REPLACE*") {
            Set-JsoncValue $Wrangler "database_id" "REPLACE_WITH_YOUR_D1_ID" -Block "d1_databases" -From $old | Out-Null
        }
    }
    foreach ($k in @("database_id", "campaign", "commits", "start")) {
        if ($state.PSObject.Properties[$k]) { $state.PSObject.Properties.Remove($k) }
    }
    Save-State $state
    $progress = Join-Path $Server ".migrate-progress.json"
    if (Test-Path $progress) { Remove-Item $progress -Force }
    Done "done; setting up again"
}

function Assert-NodeVersion {
    # wrangler needs Node.js 20 or newer.
    Step "Node.js"
    $node = Get-Command node -ErrorAction SilentlyContinue
    $nodeOk = $false
    if ($node) {
        $v = (& node --version) -replace '^v', ''
        if ([int]($v.Split('.')[0]) -ge 20) { $nodeOk = $true; Done "node $v" }
    }
    if (-not $nodeOk) {
        $ans = Read-Host "Node.js 20+ is needed. Install it now with winget? [Y/n]"
        if ($ans -eq "" -or $ans -match '^[Yy]') {
            winget install --id OpenJS.NodeJS.LTS -e --accept-source-agreements --accept-package-agreements
            Write-Host "Node.js installed. Close this window, open a new PowerShell and run the setup again."
            exit 0
        }
        Fail "install Node.js 20 or newer from https://nodejs.org and run the setup again"
    }
}

function Install-ServerPackage {
    $lock = Join-Path $Server "package-lock.json"
    $mark = Join-Path $Server "node_modules\.package-lock.json"
    if ((Test-Path $mark) -and ((Get-Item $mark).LastWriteTime -ge (Get-Item $lock).LastWriteTime)) {
        Done "already installed"
    } else {
        Run "npm" @("install", "--no-audit", "--no-fund") | Out-Null
    }
}

function Connect-Cloudflare {
    Step "Cloudflare login"
    $who = & npx wrangler whoami 2>&1 | ForEach-Object { "$_" }
    if (($who -join "`n") -match "not authenticated|You are not logged in") {
        Write-Host "    A browser window opens: log in to Cloudflare (a free account is enough) and allow access."
        Run "npx" @("wrangler", "login") | Out-Null
    } else { Done "logged in" }
}

function Initialize-Database {
    Step "D1 database collatz-frontier"
    $dbId = Get-StateValue $state "database_id"
    if (-not $dbId) {
        $db = Get-D1List | Where-Object { $_.name -eq "collatz-frontier" } | Select-Object -First 1
        if (-not $db) {
            # --update-config=false: wrangler would otherwise offer to add a second binding
            # and reformat the whole file; the wrangler.jsonc step below sets the id instead.
            Run "npx" @("wrangler", "d1", "create", "collatz-frontier", "--update-config=false") | Out-Null
            $db = Get-D1List | Where-Object { $_.name -eq "collatz-frontier" } | Select-Object -First 1
        }
        if (-not $db) { Fail "could not find the database collatz-frontier after creating it" }
        $dbId = $db.uuid
        Set-StateValue $state "database_id" $dbId
    }
    Done "id $dbId"
}

function Edit-WranglerConfig {
    Step "wrangler.jsonc"
    # wrangler.jsonc is git-ignored. A missing one is copied from the tracked template, and
    # the edits below change only that copy. An existing file is never overwritten.
    if (-not (Test-Path $Wrangler)) {
        if (-not (Test-Path $WranglerExample -PathType Leaf)) { Fail "$WranglerExample is missing; restore it with git" }
        try { [IO.File]::Copy($WranglerExample, $Wrangler, $false) }
        catch { Fail "cannot copy wrangler.example.jsonc to ${Wrangler}: $($_.Exception.Message)" }
        Done "copied wrangler.example.jsonc to wrangler.jsonc"
    }
    # Placeholders get their values, each in place; a real database id or owner name is
    # never replaced.
    $changed = $false
    $fileId = Get-JsoncValue $Wrangler "database_id" "d1_databases"
    if ($null -eq $fileId) { Fail "wrangler.jsonc has no `"database_id`" in `"d1_databases`"; add it by hand" }
    if ($fileId -clike "REPLACE*") {
        if (Set-JsoncValue $Wrangler "database_id" $dbId -Block "d1_databases" -From $fileId) { $changed = $true }
    } elseif ($fileId -cne $dbId) {
        Write-Host "    wrangler.jsonc names database $fileId, the setup found $dbId; wrangler.jsonc is left as it is" -ForegroundColor Yellow
    }
    $ownerNow = Get-JsoncValue $Wrangler "OWNER_NAME"
    if ($ownerNow -clike "REPLACE*") {
        if (-not $OwnerName) {
            $OwnerName = Read-Host "Name to show on the page [$env:USERNAME]"
            if (-not $OwnerName) { $OwnerName = $env:USERNAME }
        }
        if (Set-JsoncValue $Wrangler "OWNER_NAME" $OwnerName -From $ownerNow) { $changed = $true }
    }
    if ($changed) { Done "updated; the file as it was is in wrangler.jsonc.bak" } else { Done "already set" }
}

function Initialize-DatabaseTable {
    Step "database tables"
    # Not through Run: Run prints only when a program ends, and this one takes minutes.
    & node scripts/apply-remote.mjs collatz-frontier
    if ($LASTEXITCODE -ne 0) { Fail "creating the database tables failed (rerun the setup to continue where it stopped)" }
}
