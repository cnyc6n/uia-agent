# verify.ps1 — local, offline check that b64 decodes to the declared sha256.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$b64Path = Join-Path $root 'assets\uia_agent.exe.b64'
$shaPath = Join-Path $root 'assets\uia_agent.exe.sha256'
if (-not (Test-Path $b64Path)) { throw "missing $b64Path" }
if (-not (Test-Path $shaPath)) { throw "missing $shaPath" }

$b64 = Get-Content $b64Path -Raw
$bytes = [Convert]::FromBase64String($b64.Trim())
$tmp = Join-Path $env:TEMP "uia_from_b64.exe"
[IO.File]::WriteAllBytes($tmp, $bytes)

$got = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
$want = (Get-Content $shaPath -Raw).Trim().ToLower()

if ($got -eq $want) {
    Write-Host "OK: $($bytes.Length) bytes, sha256=$got"
    Remove-Item $tmp -Force
    exit 0
} else {
    Write-Host "MISMATCH: got=$got want=$want"
    exit 1
}