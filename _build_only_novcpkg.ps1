# 跳过 cmake --preset(避免重跑 vcpkg),仅增量 cmake --build
# 不使用 cmd /c(被 PowerShell 工具安全策略禁用),改用原生 New-Item Junction
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$ProjectPath = "E:/项目/看图软件"
$JunctionPath = "E:/qv_build_tmp"
$LLVM_BIN = "C:/Program Files\LLVM\bin"
$VS_CMAKE = "C:/Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$outDir = Join-Path $ProjectPath "out\build\Release-LTO"
$buildLog = Join-Path $ProjectPath "build_novcpkg.log"

function Unlock-Products {
    $old = Get-Process -Name "QuickView" -ErrorAction SilentlyContinue
    if ($old) { $old | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 1 }
    $dll = Join-Path $outDir "QuickViewThumbnailProvider.dll"
    if (Test-Path $dll) { try { Move-Item $dll ($dll + ".bak") -Force } catch { } }
    $exe = Join-Path $outDir "QuickView.exe"
    if (Test-Path $exe) { try { Move-Item $exe ($exe + ".bak") -Force } catch { } }
}

try {
    if (Test-Path $JunctionPath) { Remove-Item $JunctionPath -Recurse -Force -ErrorAction SilentlyContinue }
    New-Item -ItemType Junction -Path $JunctionPath -Target $ProjectPath -Force | Out-Null
    if (-not (Test-Path $JunctionPath)) { Write-Host "junction fail" -ForegroundColor Red; exit 1 }
    Write-Host ("junction ok -> " + $JunctionPath)

    $env:PATH = "$LLVM_BIN;$VS_CMAKE;$env:PATH"
    $env:VCPKG_ROOT = "$JunctionPath\third_party\vcpkg"
    $env:VCPKG_COMMAND = "$JunctionPath\third_party\vcpkg\vcpkg.exe"

    Set-Location $JunctionPath
    Unlock-Products

    $ok = $false
    for ($i = 0; $i -lt 3; $i++) {
        cmake --build out/build/Release-LTO 2>&1 | Tee-Object -FilePath $buildLog -Append | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -eq 0) { $ok = $true; break }
        Write-Host ("编译失败重试 (" + ($i+1) + "/3)") -ForegroundColor Yellow
        Unlock-Products
    }
    if (-not $ok) { Write-Host "BUILD_FAILED" -ForegroundColor Red; exit 1 }
    Write-Host "BUILD_OK"
} finally {
    if (Test-Path $JunctionPath) { Remove-Item $JunctionPath -Recurse -Force -ErrorAction SilentlyContinue }
    Write-Host "junction 已清理"
}