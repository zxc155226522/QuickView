# ===========================================================================
# QuickView 编译并启动脚本
# ===========================================================================

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$ProjectPath = "E:\项目\看图软件"
$JunctionPath = "E:\qv_build_tmp"

$LLVM_BIN = "C:\Program Files\LLVM\bin"
$VS_CMAKE = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  QuickView 编译并启动" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# 1. 结束旧进程
Write-Host "`n[1/5] 结束旧进程..." -ForegroundColor Yellow
$oldProc = Get-Process -Name "QuickView" -ErrorAction SilentlyContinue
if ($null -ne $oldProc) {
    $oldProc | Stop-Process -Force
    Write-Host "  已结束旧 QuickView 进程" -ForegroundColor Green
}

# 清理 vcpkg 锁
$vcpkgLock = "$ProjectPath\out\build\Release-LTO\vcpkg_installed\vcpkg\vcpkg-running.lock"
if (Test-Path $vcpkgLock) {
    Remove-Item $vcpkgLock -Force -ErrorAction SilentlyContinue
}

# 2. 创建 junction
Write-Host "`n[2/5] 创建临时编译路径..." -ForegroundColor Yellow
if (Test-Path $JunctionPath) {
    Remove-Item -Recurse -Force $JunctionPath -ErrorAction SilentlyContinue
}
cmd /c "mklink /J `"$JunctionPath`" `"$ProjectPath`""

# 3. 设置环境
$env:PATH = "$LLVM_BIN;$VS_CMAKE;$env:PATH"
$env:VCPKG_ROOT = "$JunctionPath\third_party\vcpkg"
$env:VCPKG_COMMAND = "$JunctionPath\third_party\vcpkg\vcpkg.exe"
$env:HTTP_PROXY = "http://127.0.0.1:7890"
$env:HTTPS_PROXY = "http://127.0.0.1:7890"

# 4. 编译
Write-Host "`n[3/5] CMake 配置..." -ForegroundColor Yellow
Set-Location $JunctionPath
cmake --preset Release-LTO
if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake 配置失败！" -ForegroundColor Red
    Set-Location $ProjectPath
    exit 1
}

Write-Host "`n[4/5] 编译 QuickView..." -ForegroundColor Yellow
cmake --build out/build/Release-LTO
if ($LASTEXITCODE -ne 0) {
    Write-Host "编译失败！" -ForegroundColor Red
    Set-Location $ProjectPath
    exit 1
}

# 4.5 复制 PDFium DLL
$pdfiumDll = "$ProjectPath\third_party\pdfium\bin\pdfium.dll"
$outDir = "$ProjectPath\out\build\Release-LTO"
if (Test-Path $pdfiumDll) {
    Copy-Item $pdfiumDll -Destination $outDir -Force
    Write-Host "  PDFium DLL 已复制到输出目录" -ForegroundColor Green
}

# 5. 启动
$exePath = "$ProjectPath\out\build\Release-LTO\QuickView.exe"
Write-Host "`n[5/5] 启动 QuickView..." -ForegroundColor Yellow
Write-Host "  $exePath" -ForegroundColor Gray

Set-Location $ProjectPath
Remove-Item -Recurse -Force $JunctionPath -ErrorAction SilentlyContinue

Start-Process $exePath
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  QuickView 已启动！" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
