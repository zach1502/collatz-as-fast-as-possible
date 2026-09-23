# The steps of setup-server.ps1 after the deploy: the collatz.exe build, the binary's
# commit, the campaign and the summary. setup-server.ps1 dot-sources this file and
# dot-calls each step, so a step reads and sets the variables of setup-server.ps1.
# Initialize-Campaign runs in server\ (setup-server.ps1 pushes the location).

# The start (l0) of campaign $name in the remote database, or $null if it cannot be read.
function Get-CampaignStart([string]$name) {
    $sql = "SELECT l0 FROM campaigns WHERE name = '$($name.Replace("'", "''"))'"
    $raw = (& npx wrangler d1 execute collatz-frontier --remote --json --yes --command $sql 2>$null | Out-String)
    $i = $raw.IndexOf("[")
    if ($LASTEXITCODE -ne 0 -or $i -lt 0) { return $null }
    $rows = @(($raw.Substring($i) | ConvertFrom-Json)[0].results)
    if ($rows.Count -eq 0) { return $null }
    return [string]$rows[0].l0
}

# The commit string the way CMake records it without git: <sha>-nogit. Resolve-BinaryCommit
# uses it only with -AllowDirty.
function Get-CommitFromGitDir {
    $gitdir = Join-Path $Repo ".git"
    $headFile = Join-Path $gitdir "HEAD"
    if (-not (Test-Path $headFile -PathType Leaf)) { return $null }
    $head = (Get-Content $headFile -Raw).Trim()
    $sha = $null
    if ($head -match '^ref: (.+)$') {
        $ref = $Matches[1]
        $refFile = Join-Path $gitdir $ref
        if (Test-Path $refFile) { $sha = (Get-Content $refFile -Raw).Trim() }
        elseif (Test-Path (Join-Path $gitdir "packed-refs")) {
            $line = Get-Content (Join-Path $gitdir "packed-refs") | Where-Object { $_ -match " $([regex]::Escape($ref))$" } | Select-Object -First 1
            if ($line) { $sha = ($line -split " ")[0] }
        }
    } else { $sha = $head }
    if ($sha -match '^[0-9a-f]{40}$') { return "$sha-nogit" }
    return $null
}

# A commit that ends in -dirty or -nogit, or is unknown, does not say which code ran, so the
# setup lists it in a campaign only with -AllowDirty; scripts/campaign.mjs refuses the same
# commits unless given --allow-dirty.
function Assert-CleanCommit([string]$c) {
    if ($AllowDirty -or $c -cnotmatch '-(dirty|nogit)$|^unknown$') { return }
    Fail "the binary reports commit $c, which does not say which code ran; build collatz.exe from a clean checkout, or rerun the setup with -AllowDirty to list it anyway"
}

function Invoke-CollatzBuild {
    Step "build collatz.exe (MSYS2 UCRT64)"
    if (-not (Test-Path $bash)) { Fail "MSYS2 not found at $MsysRoot (use -MsysRoot, or -SkipBuild if you build it yourself)" }
    $posix = "/" + $Repo.Substring(0, 1).ToLower() + $Repo.Substring(2).Replace('\', '/')
    $env:MSYSTEM = "UCRT64"
    $env:CHERE_INVOKING = "1"
    Run $bash @("-lc", "cd '$posix' && cmake --preset windows-release && cmake --build --preset windows-release") | Out-Null
}

function Resolve-BinaryCommit {
    # The campaign accepts results only from the binary commits it lists.
    Step "binary commit"
    $commit = $null
    if (Test-Path $exe) {
        $env:PATH = (Join-Path $MsysRoot "ucrt64\bin") + ";" + $env:PATH
        $tmpJson = Join-Path ([IO.Path]::GetTempPath()) "collatz-commit.json"
        & $exe verify --N 20 --threads 1 --json $tmpJson 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0 -and (Test-Path $tmpJson)) {
            $commit = (Get-Content $tmpJson -Raw | ConvertFrom-Json).commit
            Remove-Item $tmpJson -Force
        }
    }
    # The guess from .git is what a build without git would report, not what this binary does.
    if (-not $commit -and $AllowDirty) { $commit = Get-CommitFromGitDir }
    if (-not $commit -or $commit -eq "unknown") { Fail "could not determine the binary's commit (build collatz.exe; -AllowDirty falls back to <sha>-nogit from .git)" }
    Done $commit
}

function Initialize-Campaign {
    Step "campaign $Campaign"
    # The campaign starts at Barina's frontier, 2075*2^60 (pcbarina.fit.vutbr.cz, retrieved
    # 2026-10-10), below which every number is verified, and checks every number from there.
    $L0 = "2392312122059207475200"
    if (-not (Get-StateValue $state "campaign")) {
        Assert-CleanCommit $commit
        $source = "all n below 2075 times 2^60 (Barina, pcbarina.fit.vutbr.cz, retrieved 2026-10-10) and the earlier blocks of this campaign"
        $mjsArgs = @("scripts/campaign.mjs", "--name", $Campaign, "--commit", $commit,
                     "--L0", $L0, "--assumption-source", $source, "--max-bits", "$MaxBits",
                     "--lease-hours", "$LeaseHours", "--out", "campaign.sql")
        if ($AllowDirty) { $mjsArgs += "--allow-dirty" }
        Run "node" $mjsArgs | Out-Null
        Run "npx" @("wrangler", "d1", "execute", "collatz-frontier", "--remote", "--file", "campaign.sql", "--yes") | Out-Null
        Set-StateValue $state "campaign" $Campaign
        Set-StateValue $state "commits" @($commit)
        Set-StateValue $state "start" $L0
    } else {
        Done "already created"
        # An existing campaign may start elsewhere, such as at 2^71; it is not moved.
        $start = Get-StateValue $state "start"
        if (-not $start) {
            $start = Get-CampaignStart $Campaign
            if ($start) { Set-StateValue $state "start" $start }
        }
        if ($start -and $start -ne $L0) {
            Write-Host "    $Campaign starts at $start, not at 2075*2^60; run the setup with -Reset to delete the database and start over there" -ForegroundColor Yellow
        }
    }
    # Sets the block size range and the lease length, also on an existing campaign. The
    # campaign starts in use, 2075*2^60 and 2^71, are multiples of 2^60, so blocks of up
    # to 2^60 numbers stay aligned.
    if ($MaxBits -lt 44 -or $MaxBits -gt 60) { Fail "-MaxBits must be from 44 to 60" }
    if ((Get-StateValue $state "max_bits") -ne $MaxBits -or (Get-StateValue $state "lease_hours") -ne $LeaseHours) {
        Step "blocks up to 2^$MaxBits, leases of $LeaseHours hours"
        $sql = "UPDATE campaigns SET max_bits = $MaxBits, lease_seconds = $($LeaseHours * 3600) WHERE name = '$Campaign';"
        [IO.File]::WriteAllText((Join-Path $Server "campaign-update.sql"), $sql, $Utf8)
        Run "npx" @("wrangler", "d1", "execute", "collatz-frontier", "--remote", "--file", "campaign-update.sql", "--yes") | Out-Null
        Remove-Item (Join-Path $Server "campaign-update.sql") -Force
        Set-StateValue $state "max_bits" $MaxBits
        Set-StateValue $state "lease_hours" $LeaseHours
    }
    $known = @(Get-StateValue $state "commits")
    if ($known -notcontains $commit) {
        # This SQL bypasses campaign.mjs, so the commit is checked here.
        Assert-CleanCommit $commit
        Step "allow commit $commit"
        # Through a file: Windows PowerShell 5.1 drops embedded quotes from native arguments.
        $sql = "UPDATE campaigns SET commits = json_insert(commits, '`$[#]', '$commit') " +
               "WHERE name = '$Campaign' AND instr(commits, '`"$commit`"') = 0;"
        [IO.File]::WriteAllText((Join-Path $Server "add-commit.sql"), $sql, $Utf8)
        Run "npx" @("wrangler", "d1", "execute", "collatz-frontier", "--remote", "--file", "add-commit.sql", "--yes") | Out-Null
        Remove-Item (Join-Path $Server "add-commit.sql") -Force
        Set-StateValue $state "commits" ($known + $commit)
    }
}

function Show-Summary {
    Write-Host ""
    Write-Host "Setup finished." -ForegroundColor Green
    Write-Host "  Public page:   $url"
    Write-Host "  Worker token:  $TokenFile (keep it private; copy it to other machines you run)"
    Write-Host "  Start working: double-click scripts\windows\start-worker.cmd"
    Write-Host "                 or: scripts\windows\start-worker.cmd -BlockBits $BlockBits"
}
