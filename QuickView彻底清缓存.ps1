# Thorough thumbnail/icon cache clear + Explorer restart (ASCII-only safe for any codepage)
$ErrorActionPreference = 'SilentlyContinue'

$wasRunning = Get-Process -Name explorer -ErrorAction SilentlyContinue
if ($wasRunning) {
    Write-Host "Stopping Explorer..."
    taskkill /f /im explorer.exe | Out-Null
    Start-Sleep -Seconds 2
}

$cacheDir = Join-Path $env:LOCALAPPDATA "Microsoft\Windows\Explorer"
$patterns = @("thumbcache_*.db", "iconcache_*.db", "thumbcache_idx.db", "thumbcache_exif.db",
              "thumbcache_wide.db", "thumbcache_custom_stream.db")
$removed = 0
foreach ($pat in $patterns) {
    Get-ChildItem -Path $cacheDir -Filter $pat -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
        if ($?) { $removed++; Write-Host ("Removed " + $_.Name) }
    }
}
Write-Host ("Total removed: " + $removed)

Write-Host "Refreshing icon cache..."
$ie4u = Join-Path $env:SystemRoot "System32\ie4uinit.exe"
if (Test-Path $ie4u) {
    & $ie4u -show | Out-Null
    & $ie4u -ClearIconCache | Out-Null
}

if ($wasRunning) {
    Write-Host "Restarting Explorer..."
    Start-Process explorer.exe
}

Write-Host "=== Cache cleared. ==="
