# 停止 Explorer → 删除缓存 → 重启 Explorer
$cacheDir = "$env:LOCALAPPDATA\Microsoft\Windows\Explorer"
$files = Get-ChildItem $cacheDir -Filter 'thumbcache_*.db' -ErrorAction SilentlyContinue

if (-not $files) {
    Write-Host "[INFO] 无缓存文件需要清理"
    return
}

# 备份
$backupDir = "$env:USERPROFILE\Desktop\qv_thumbcache_bak_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
Copy-Item $files.FullName $backupDir
Write-Host "[OK] 已备份到: $backupDir"

# 停止 Explorer
Write-Host "[1/3] 停止 Explorer..."
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# 删除缓存
Write-Host "[2/3] 删除缩略图缓存..."
$deleted = 0
$failed = 0
foreach ($f in $files) {
    try {
        Remove-Item $f.FullName -Force -ErrorAction Stop
        $deleted++
    } catch {
        $failed++
        Write-Host "  [FAIL] $($f.Name): $_"
    }
}
Write-Host "  成功: $deleted, 失败: $failed"

# 重启 Explorer
Write-Host "[3/3] 重启 Explorer..."
Start-Process explorer.exe
Start-Sleep -Seconds 2
Write-Host "[OK] 完成！请在资源管理器中用大图标/超大图标视图查看 SVG/AI 文件验证缩略图"
