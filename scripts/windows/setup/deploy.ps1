# The steps of setup-server.ps1 that set the Worker's secrets and deploy it.
# setup-server.ps1 dot-sources this file and dot-calls each step, so a step reads and sets
# the variables of setup-server.ps1. The steps run in server\ (setup-server.ps1 pushes the
# location).

function New-Secret {
    $bytes = New-Object byte[] 32
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    return (($bytes | ForEach-Object { $_.ToString("x2") }) -join "")
}

# Pipes a file into `wrangler secret put NAME` through cmd, so no newline is added and the
# value never appears on a command line.
function Put-Secret([string]$name, [string]$file) {
    Push-Location $Server
    try {
        cmd /c "npx wrangler secret put $name < `"$file`"" 2>&1 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { Fail "could not set the secret $name" }
    } finally { Pop-Location }
}

function Add-ServerSecret {
    Step "secrets"
    if (-not (Get-StateValue $state "rl_secret_set")) {
        $tmp = [IO.Path]::GetTempFileName()
        try {
            [IO.File]::WriteAllText($tmp, (New-Secret), [Text.Encoding]::ASCII)
            Put-Secret "RL_SECRET" $tmp
        } finally { Remove-Item $tmp -Force }
        Set-StateValue $state "rl_secret_set" $true
    } else { Done "RL_SECRET already set" }
    if (-not (Test-Path $TokenFile)) {
        [IO.File]::WriteAllText($TokenFile, (New-Secret), [Text.Encoding]::ASCII)
        icacls $TokenFile /inheritance:r /grant:r "$($env:USERNAME):(R,W)" | Out-Null
        Set-StateValue $state "worker_token_set" $false
        Done "new worker token in $TokenFile"
    }
    if (-not (Get-StateValue $state "worker_token_set")) {
        Put-Secret "WORKER_TOKEN" $TokenFile
        Set-StateValue $state "worker_token_set" $true
    } else { Done "WORKER_TOKEN already set" }
}

function Publish-Server {
    Step "deploy"
    $out = & npx wrangler deploy 2>&1 | ForEach-Object { "$_" }
    $out | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) {
        if (($out -join "`n") -match "workers\.dev subdomain") {
            # wrangler offers to register the subdomain only when it can ask, so the deploy
            # runs once attached to this window, then again to read the address.
            Write-Host ""
            Write-Host "Your Cloudflare account needs a workers.dev name (once)." -ForegroundColor Yellow
            Write-Host "  Answer y, then type a name: lowercase letters, digits and -, e.g. $($env:USERNAME.ToLower())-collatz."
            Write-Host "  The page address becomes https://collatz-frontier.<name>.workers.dev"
            Write-Host ""
            & npx wrangler deploy
            if ($LASTEXITCODE -ne 0) { Fail "deploy failed; see the messages above" }
            $out = & npx wrangler deploy 2>&1 | ForEach-Object { "$_" }
            $out | ForEach-Object { Write-Host "    $_" }
            if ($LASTEXITCODE -ne 0) { Fail "deploy failed" }
        } else {
            Fail "deploy failed"
        }
    }
    $url = ($out | Select-String -Pattern 'https://[A-Za-z0-9.-]+\.workers\.dev' -AllMatches |
            ForEach-Object { $_.Matches } | Select-Object -First 1).Value
    if ($url) { Set-StateValue $state "url" $url } else { $url = Get-StateValue $state "url" }
    if (-not $url) { Fail "could not find the workers.dev address in the deploy output" }
    Done $url
}
