# pack.ps1 — compile then generate assets/uia_agent.exe.b64 + .sha256
# CI calls this after a successful build; devs can run it too.
param(
    [string]$ExePath = "",
    [string]$AssetDir = ""
)
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
if (-not $AssetDir) { $AssetDir = Join-Path $root 'assets' }
if (-not $ExePath) {
    $candidates = @(
        (Join-Path $root 'build\uia_agent.exe'),
        (Join-Path $root 'release\uia_agent.exe')
    )
    foreach ($c in $candidates) { if (Test-Path $c) { $ExePath = $c; break } }
}
if (-not $ExePath -or -not (Test-Path $ExePath)) {
    throw "uia_agent.exe not found. Build first (cmake --build build) or pass -ExePath."
}

New-Item -ItemType Directory -Path $AssetDir -Force | Out-Null

$bytes = [IO.File]::ReadAllBytes($ExePath)
$b64 = [Convert]::ToBase64String($bytes)
$hash = (Get-FileHash $ExePath -Algorithm SHA256).Hash.ToLower()

$b64Path = Join-Path $AssetDir 'uia_agent.exe.b64'
$shaPath = Join-Path $AssetDir 'uia_agent.exe.sha256'
[IO.File]::WriteAllText($b64Path, $b64, (New-Object System.Text.UTF8Encoding $false))
[IO.File]::WriteAllText($shaPath, $hash, (New-Object System.Text.UTF8Encoding $false))

Write-Host "packed: $($bytes.Length) bytes -> $b64Path"
Write-Host "sha256: $hash -> $shaPath"

# self-check: decode b64 back and compare hash
$tmp = Join-Path $env:TEMP "uia_agent_pack_check.exe"
[IO.File]::WriteAllBytes($tmp, [Convert]::FromBase64String($b64))
$got = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
if ($got -ne $hash) { throw "self-check MISMATCH: b64-decoded sha=$got exe sha=$hash" }
Remove-Item $tmp -Force
Write-Host "self-check OK (b64 round-trip hash matches)"