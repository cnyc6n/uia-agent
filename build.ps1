# uia_agent 构建脚本：一次性完成 vcvars + cmake configure + build
$vs = 'D:\Program Files\Microsoft Visual Studio\2022\Community'
$vcvars = "$vs\Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $vcvars)) { Write-Error "VsDevCmd not found: $vcvars"; exit 1 }
$out = cmd /c "`"$vcvars`" -arch=amd64 -host_arch=amd64 -no_logo >nul 2>&1 && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build 2>&1"
$out
# 真正的失败只出现在 "error"/"fatal error" 行；"注意: 包含文件" 等噪音行忽略
$errs = $out | Where-Object { $_ -match '^\S+.*error C|fatal error|FAILED|ninja: build stopped' }
if (-not $errs) {
    $exe = Join-Path $PSScriptRoot 'build\uia_agent.exe'
    if (Test-Path $exe) {
        $f = Get-Item $exe
        Write-Output ("BUILD OK: {0} ({1:N2} MB)" -f $exe, ($f.Length / 1MB))
        exit 0
    }
}
Write-Output 'BUILD FAILED'
exit 1