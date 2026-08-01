# ===========================================================================
# QuickView 编译发布脚本
# 编译 Release-LTO 版本并输出到「发布」文件夹
# ===========================================================================

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# 项目路径
$ProjectPath = $PSScriptRoot
$JunctionPath = "E:\qv_release_tmp"

# 工具路径
$LLVM_BIN = "C:\Program Files\LLVM\bin"
$VS_CMAKE = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$VCPKG_ROOT = Join-Path $ProjectPath "third_party\vcpkg"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  QuickView 编译发布" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# 1. 创建无中文路径的 junction
Write-Host "`n[1/4] 创建临时编译路径..." -ForegroundColor Yellow
if (Test-Path $JunctionPath) {
    cmd /c rmdir $JunctionPath 2>$null
}
cmd /c mklink /J $JunctionPath $ProjectPath | Out-Null
if (-not (Test-Path $JunctionPath)) {
    Write-Host "  创建 junction 失败！" -ForegroundColor Red
    exit 1
}
Write-Host "  临时路径: $JunctionPath" -ForegroundColor Green

# 2. 设置环境并编译
$env:PATH = "$LLVM_BIN;$VS_CMAKE;$env:PATH"
$env:VCPKG_ROOT = "$JunctionPath\third_party\vcpkg"
$env:VCPKG_COMMAND = "$JunctionPath\third_party\vcpkg\vcpkg.exe"
$env:HTTP_PROXY = "http://127.0.0.1:7890"
$env:HTTPS_PROXY = "http://127.0.0.1:7890"

Write-Host "`n[2/4] CMake 配置..." -ForegroundColor Yellow
Set-Location $JunctionPath
cmake --preset Release-LTO 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake 配置失败！" -ForegroundColor Red
    cmd /c rmdir $JunctionPath
    exit 1
}

Write-Host "`n[3/4] 编译 QuickView (Release-LTO)..." -ForegroundColor Yellow
cmake --build out/build/Release-LTO 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Host "编译失败！" -ForegroundColor Red
    cmd /c rmdir $JunctionPath
    exit 1
}
Write-Host "  编译成功！" -ForegroundColor Green

# 4. 发布
$publishDir = Join-Path $ProjectPath "发布"
$timestamp = Get-Date -Format "yyyy年M月d日H时m分s秒"
$releaseDir = Join-Path $publishDir $timestamp
New-Item -ItemType Directory -Force $releaseDir | Out-Null

$exePath = Join-Path $ProjectPath "out\build\Release-LTO\QuickView.exe"
Copy-Item $exePath $releaseDir -Force

# 删除 junction
cmd /c rmdir $JunctionPath 2>$null

Write-Host "`n[4/4] 发布完成！" -ForegroundColor Green
Write-Host "  发布目录: $releaseDir" -ForegroundColor Cyan
Write-Host "  文件: QuickView.exe ($([math]::Round((Get-Item $exePath).Length / 1MB, 2)) MB)" -ForegroundColor Gray
Write-Host "`n========================================" -ForegroundColor Cyan
