# QuickView thumbnails re-registration (single-click deploy)
# Mirrors QuickView/ThumbnailExts.h: every browsable image format (SUPPORTED_EXTENSIONS
# minus VFS archive containers .zip/.cbz/.cbr/.rar = 70) gets an .ext-level
# IThumbnailProvider ShellEx handler, so badges show regardless of default app.
# Body is ASCII-only to avoid PowerShell UTF-8/BOM encoding issues.

$ErrorActionPreference = 'SilentlyContinue'

$clsid     = "{4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}"
$thumbIID  = "{E357FCCD-A995-4576-B01F-234630154E96}"
$dllPath   = "E:\项目\看图软件\out\build\Release-LTO\QuickViewThumbnailProvider.dll"

# 70 browsable image extensions (SupportedExtensions.h minus archive segments)
$exts = @(
    # Standard raster (11)
    ".jpg",".jpeg",".jpe",".jfif",".png",".bmp",".dib",".gif",".tif",".tiff",".ico",
    # Web / Modern (16)
    ".webp",".avif",".avifs",".heic",".heif",".svg",".svgz",".jxl",".apng",".cdr",".cmx",".pdf",".ai",".plt",".dxf",".dwg",
    # Pro / Legacy (21)
    ".exr",".hdr",".pic",".psd",".psb",".tga",".icb",".vda",".vst",".pcx",".qoi",".wbmp",".pam",".pbm",".pgm",".ppm",".pnm",".wdp",".hdp",".jxr",".hif",
    # Camera RAW (22)
    ".arw",".cr2",".cr3",".crw",".dng",".nef",".orf",".raf",".rw2",".srw",".x3f",".mrw",".mos",".kdc",".dcr",".sr2",".pef",".erf",".3fr",".mef",".nrw",".raw"
)

Write-Host "=== QuickView thumbnail re-registration ==="
Write-Host ("Extension count = " + $exts.Count + " (expect 70)")

# --- Sanity: DLL present? ---
if (-not (Test-Path $dllPath)) {
    Write-Host ("WARNING: DLL not found at: " + $dllPath)
    Write-Host "Registration will still write the keys; rebuild to drop the DLL there."
} else {
    Write-Host ("DLL found: " + $dllPath)
}

# --- CLSID InprocServer32 (HKCU, no admin needed) ---
$hkcuCls = ("HKCU:\Software\Classes\CLSID\" + $clsid)
New-Item -Path $hkcuCls -Force | Out-Null
New-Item -Path ($hkcuCls + "\InprocServer32") -Force | Out-Null
Set-ItemProperty -Path ($hkcuCls + "\InprocServer32") -Name "(default)" -Value $dllPath
Set-ItemProperty -Path ($hkcuCls + "\InprocServer32") -Name "ThreadingModel" -Value "Apartment"
Write-Host ("CLSID InprocServer32 -> " + $dllPath)

# --- ProgIDs (QuickView.Image / QuickView.Vector) carry ShellEx too ---
foreach ($p in @("QuickView.Image", "QuickView.Vector")) {
    $pk = ("HKCU:\Software\Classes\" + $p + "\ShellEx\" + $thumbIID)
    New-Item -Path $pk -Force | Out-Null
    Set-ItemProperty -Path $pk -Name "(default)" -Value $clsid
    Write-Host ("ProgID " + $p + " ShellEx -> " + $clsid)
}

# --- .ext-level ShellEx for all 70 formats (highest priority, beats UserChoice) ---
$ok = 0
foreach ($ext in $exts) {
    $ek = ("HKCU:\Software\Classes\" + $ext + "\ShellEx\" + $thumbIID)
    New-Item -Path $ek -Force | Out-Null
    Set-ItemProperty -Path $ek -Name "(default)" -Value $clsid
    $ok++
}
Write-Host ("Registered .ext ShellEx for " + $ok + " extensions")

# --- Clear thumbnail cache + restart Explorer (only if it was running) ---
$wasRunning = Get-Process -Name explorer -ErrorAction SilentlyContinue
if ($wasRunning) {
    Write-Host "Stopping Explorer to release thumbnail cache..."
    taskkill /f /im explorer.exe | Out-Null
    Start-Sleep -Seconds 2
}
$cacheDir = Join-Path $env:LOCALAPPDATA "Microsoft\Windows\Explorer"
$removed = 0
Get-ChildItem -Path $cacheDir -Filter "thumbcache_*.db" -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
    $removed++
}
Write-Host ("Cleared " + $removed + " thumbnail cache db files")
if ($wasRunning) {
    Write-Host "Restarting Explorer..."
    Start-Process explorer.exe
}

Write-Host "=== Done. Explorer will now show QuickView badges for all formats. ==="
