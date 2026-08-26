﻿# ===========================================================================
# QuickView 编译并启动脚本
# 解决中文路径导致 NASM 汇编器乱码的问题
# 原理：创建无中文路径的 junction，从 junction 编译，完成后删除 junction
# ===========================================================================

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# 项目路径
$ProjectPath = $PSScriptRoot
$JunctionPath = "E:\qv_build_tmp"

# 工具路径
$LLVM_BIN = "C:\Program Files\LLVM\bin"
$VS_CMAKE = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$VCPKG_ROOT = Join-Path $ProjectPath "third_party\vcpkg"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  QuickView 编译并启动" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# 1. 强制结束旧进程
Write-Host "`n[1/5] 结束旧进程..." -ForegroundColor Yellow
$oldProc = Get-Process -Name "QuickView" -ErrorAction SilentlyContinue
if ($oldProc) {
    $oldProc | Stop-Process -Force
    Write-Host "  已结束旧 QuickView 进程" -ForegroundColor Green
} else {
    Write-Host "  无运行中的 QuickView 进程" -ForegroundColor Gray
}

# 2. 创建无中文路径的 junction
Write-Host "`n[2/5] 创建临时编译路径..." -ForegroundColor Yellow
if (Test-Path $JunctionPath) {
    cmd /c rmdir $JunctionPath 2>$null
}
if (Test-Path $JunctionPath) {
    Remove-Item -Recurse -Force $JunctionPath -ErrorAction SilentlyContinue
}
cmd /c mklink /J $JunctionPath $ProjectPath | Out-Null
if (Test-Path $JunctionPath) {
    Write-Host "  临时路径: $JunctionPath -> $ProjectPath" -ForegroundColor Green
} else {
    Write-Host "  创建 junction 失败！" -ForegroundColor Red
    exit 1
}

# 3. 设置环境
$env:PATH = "$LLVM_BIN;$VS_CMAKE;$env:PATH"
$env:VCPKG_ROOT = "$JunctionPath\third_party\vcpkg"
$env:VCPKG_COMMAND = "$JunctionPath\third_party\vcpkg\vcpkg.exe"
$env:HTTP_PROXY = "http://127.0.0.1:7890"
$env:HTTPS_PROXY = "http://127.0.0.1:7890"

# 4. 编译
Write-Host "`n[3/5] CMake 配置..." -ForegroundColor Yellow
Set-Location $JunctionPath
cmake --preset Release-LTO 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake 配置失败！" -ForegroundColor Red
    cmd /c rmdir $JunctionPath
    exit 1
}
Write-Host "  CMake 配置完成" -ForegroundColor Green

Write-Host "`n[4/5] 编译 QuickView..." -ForegroundColor Yellow
cmake --build out/build/Release-LTO 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Host "编译失败！" -ForegroundColor Red
    cmd /c rmdir $JunctionPath
    exit 1
}
Write-Host "  编译成功！" -ForegroundColor Green

# 4.5 复制 PDFium DLL
$pdfiumDll = Join-Path $ProjectPath "third_party\pdfium\bin\pdfium.dll"
$outDir = Join-Path $ProjectPath "out\build\Release-LTO"
if (Test-Path $pdfiumDll) {
    Copy-Item $pdfiumDll -Destination $outDir -Force
    Write-Host "  PDFium DLL 已复制到输出目录" -ForegroundColor Green
}

# 5. 启动
$exePath = Join-Path $ProjectPath "out\build\Release-LTO\QuickView.exe"
Write-Host "`n[5/5] 启动 QuickView..." -ForegroundColor Yellow
Write-Host "  $exePath" -ForegroundColor Gray

# 删除 junction（不影响已编译的文件，因为文件在项目目录中）
cmd /c rmdir $JunctionPath 2>$null
Write-Host "  临时路径已清理" -ForegroundColor Gray

Start-Process $exePath
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  QuickView 已启动！" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
