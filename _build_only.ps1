# ===========================================================================
# QuickView 仅编译脚本（+ 编译后自动重注册部署）
# 复用编译并启动脚本的 junction 逻辑，规避中文路径 NASM 问题
# 编译前释放被运行进程锁定的产物：
#   - 结束 thumbnail-server/GUI 进程（QuickView.exe）
#   - 移走已注册的 DLL：explorer 后续加载 provider 失败 -> 不再拉起
#     thumbnail-server -> QuickView.exe 整个编译期都不会被占用
#   - EXE 若仍被锁则重命名解锁（运行中 exe 也可被 NTFS 重命名）
# 编译日志写入 build.log；编译成功后调用重注册脚本部署新 DLL
# ===========================================================================

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$ProjectPath = $PSScriptRoot
$JunctionPath = "E:\qv_build_tmp"
$LLVM_BIN = "C:\Program Files\LLVM\bin"
$VS_CMAKE = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$VCPKG_ROOT = Join-Path $ProjectPath "third_party\vcpkg"
$buildLog = Join-Path $ProjectPath "build.log"
$outDir = Join-Path $ProjectPath "out\build\Release-LTO"

function Unlock-Products {
    # 结束 server/GUI，避免 EXE 被锁
    $old = Get-Process -Name "QuickView" -ErrorAction SilentlyContinue
    if ($old) {
        $old | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
        Write-Host ("已结束旧 QuickView 进程 x" + $old.Count) -ForegroundColor Yellow
    }
    # 移走已注册的 DLL：explorer 后续加载失败 -> 不再拉起 thumbnail-server
    $dll = Join-Path $outDir "QuickViewThumbnailProvider.dll"
    if (Test-Path $dll) {
        try { Move-Item $dll ($dll + ".bak") -Force; Write-Host "已移走被注册 DLL 以阻止 server 拉起" -ForegroundColor Yellow }
        catch { Write-Host ("警告: DLL 移走失败: " + $_.Exception.Message) -ForegroundColor Red }
    }
    # EXE 兜底解锁
    $exe = Join-Path $outDir "QuickView.exe"
    if (Test-Path $exe) {
        try { Move-Item $exe ($exe + ".bak") -Force } catch { }
    }
}

try {
    if (Test-Path $JunctionPath) { cmd /c rmdir $JunctionPath 2>$null }
    if (Test-Path $JunctionPath) { Remove-Item -Recurse -Force $JunctionPath -ErrorAction SilentlyContinue }
    cmd /c mklink /J $JunctionPath $ProjectPath | Out-Null
    if (-not (Test-Path $JunctionPath)) { Write-Host "创建 junction 失败！" -ForegroundColor Red; exit 1 }

    $env:PATH = "$LLVM_BIN;$VS_CMAKE;$env:PATH"
    $env:VCPKG_ROOT = "$JunctionPath\third_party\vcpkg"
    $env:VCPKG_COMMAND = "$JunctionPath\third_party\vcpkg\vcpkg.exe"
    $env:HTTP_PROXY = "http://127.0.0.1:7890"
    $env:HTTPS_PROXY = "http://127.0.0.1:7890"

    Set-Location $JunctionPath
    cmake --preset Release-LTO 2>&1 | Tee-Object -FilePath $buildLog | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { Write-Host "CMake 配置失败！" -ForegroundColor Red; exit 1 }

    Unlock-Products

    $ok = $false
    for ($i = 0; $i -lt 2; $i++) {
        cmake --build out/build/Release-LTO 2>&1 | Tee-Object -FilePath $buildLog -Append | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -eq 0) { $ok = $true; break }
        Write-Host ("编译失败，尝试释放锁定产物后重试 (" + ($i+1) + "/2)...") -ForegroundColor Yellow
        Unlock-Products
    }
    if (-not $ok) { Write-Host "编译失败！" -ForegroundColor Red; exit 1 }
    Write-Host "编译成功！" -ForegroundColor Green

    # 清理残留 .bak（若未被 explorer 占用）
    Remove-Item (Join-Path $outDir "*.bak") -Force -ErrorAction SilentlyContinue

    # 部署：重注册新 DLL 并重启 explorer 加载新版
    Write-Host "`n[部署] 重注册缩略图提供器..." -ForegroundColor Cyan
    & "$ProjectPath\QuickView缩略图重注册.ps1"
} finally {
    cmd /c rmdir $JunctionPath 2>$null
    Write-Host "临时路径已清理" -ForegroundColor Gray
}
