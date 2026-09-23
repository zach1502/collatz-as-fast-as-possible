# Starts the frontier worker on this machine (GPU and blocks of 2^50 by default).
#     scripts\windows\start-worker.cmd [-BlockBits 50] [-Cpu] [-Threads 16] [-Server URL]
# Ctrl-C pauses and hands the block back; the next run resumes it.
param(
    [string]$Server = "",
    [int]$BlockBits = 50,
    [switch]$Cpu,
    [int]$Threads = 0,
    [string]$MsysRoot = "C:\msys64",
    [string]$TokenFile = (Join-Path $env:USERPROFILE "collatz-token.txt")
)
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$StateFile = Join-Path $Repo "server\.setup-state.json"
$exe = Join-Path $Repo "build\windows-release\collatz.exe"

if (-not $Server -and (Test-Path $StateFile)) { $Server = (Get-Content $StateFile -Raw | ConvertFrom-Json).url }
if (-not $Server) { Write-Host "No server address: run scripts\windows\setup-server.cmd first, or pass -Server URL"; exit 1 }
if (-not (Test-Path $exe)) { Write-Host "Missing ${exe}: run scripts\windows\setup-server.cmd (it builds it)"; exit 1 }
if (-not (Test-Path $TokenFile)) { Write-Host "Missing worker token $TokenFile (copy it from the machine that ran the setup)"; exit 1 }

# collatz.exe needs the MSYS2 UCRT64 DLLs (GMP, OpenCL loader).
$env:PATH = (Join-Path $MsysRoot "ucrt64\bin") + ";" + $env:PATH

$argv = @("worker", "--server", $Server, "--block-bits", "$BlockBits", "--token-file", $TokenFile)
if ($Cpu) { if ($Threads -gt 0) { $argv += @("--threads", "$Threads") } }
else { $argv += "--gpu" }

Write-Host "Running: collatz worker on $Server (blocks of 2^$BlockBits). Ctrl-C to pause."
& $exe @argv
exit $LASTEXITCODE
