# 一次性清理：SageThumbs 悬空残留注册 + 关闭 Windows"在缩略图上显示文件图标"
# 背景：SageThumbs（指向已删除的 E:\项目\win7 看图软件\SageThumbs\Release\SageThumbs.dll）
# 的残留注册占着各图片格式的老式缩略图(IExtractImage)/PropertyHandler 槽位，其图标叠加
# 效果使用旧缓存图标（删不掉的 iconcache），导致缩略图右下角一直显示旧版 logo。
# QuickView 现在自己绘制右下角每格式字母章，Windows 的类型叠加需关闭避免重叠。
# ASCII-only body, safe for any codepage.
$ErrorActionPreference = 'SilentlyContinue'

$deadClsid = '{4A34B3E3-F50E-4FF6-8979-7E4176466FF2}'   # SageThumbs Shell Extension
$thumbIID  = '{BB2E617C-0920-11D1-9A0B-00C04FC2D6C1}'   # legacy IExtractImage category
$exts = @(
    ".jpg",".jpeg",".jpe",".jfif",".png",".bmp",".dib",".gif",".tif",".tiff",".ico",
    ".webp",".avif",".avifs",".heic",".heif",".svg",".svgz",".jxl",".apng",".cdr",".cmx",".pdf",".ai",".plt",".dxf",".dwg",
    ".exr",".hdr",".pic",".psd",".psb",".tga",".icb",".vda",".vst",".pcx",".qoi",".wbmp",".pam",".pbm",".pgm",".ppm",".pnm",".wdp",".hdp",".jxr",".hif",
    ".arw",".cr2",".cr3",".crw",".dng",".nef",".orf",".raf",".rw2",".srw",".x3f",".mrw",".mos",".kdc",".dcr",".sr2",".pef",".erf",".3fr",".mef",".nrw",".raw"
)

# 1) delete dead SageThumbs ShellEx values (.ext level) — only where they point at the dead CLSID
$removed = 0
foreach ($ext in $exts) {
    foreach ($sub in @($thumbIID, "PropertyHandler")) {
        $key = "HKCU:\Software\Classes\" + $ext + "\ShellEx\" + $sub
        $v = (Get-ItemProperty -Path $key -Name "(default)" -ErrorAction SilentlyContinue)."(default)"
        if ($v -eq $deadClsid) {
            Remove-ItemProperty -Path $key -Name "(default)" -Force -ErrorAction SilentlyContinue
            $removed++
        }
        $v2 = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).'(default)'
        if ($v2 -eq $deadClsid) {
            Remove-ItemProperty -Path $key -Name "(default)" -Force -ErrorAction SilentlyContinue
            $removed++
        }
    }
}
Write-Host ("Removed dead ShellEx values: " + $removed)

# 2) delete leftover SageThumbsImage.* ProgIDs + their OpenWithProgids references
$progids = Get-ChildItem "HKCU:\Software\Classes" -Name | Where-Object { $_ -like "SageThumbsImage.*" }
foreach ($p in $progids) {
    Remove-Item -Path ("HKCU:\Software\Classes\" + $p) -Recurse -Force -ErrorAction SilentlyContinue
    $ext = $p.Substring("SageThumbsImage.".Length)
    $owp = "HKCU:\Software\Classes\" + $ext + "\OpenWithProgids"
    Remove-ItemProperty -Path $owp -Name $p -Force -ErrorAction SilentlyContinue
}
Write-Host ("Removed SageThumbsImage ProgIDs: " + @($progids).Count)

# 3) delete SageThumbs CLSID registration (HKCU both views) + settings
Remove-Item -Path ("HKCU:\Software\Classes\WOW6432Node\CLSID\" + $deadClsid) -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -Path ("HKCU:\Software\Classes\CLSID\" + $deadClsid) -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -Path "HKCU:\Software\SageThumbs" -Recurse -Force -ErrorAction SilentlyContinue

# 4) HKLM 32-bit view (needs admin; skip silently if not permitted)
reg delete "HKLM\SOFTWARE\WOW6432Node\Classes\CLSID\{4A34B3E3-F50E-4FF6-8979-7E4176466FF2}" /f 2>$null | Out-Null

# 5) turn off "Display file icon on thumbnails" (Explorer draws old cached logo; QuickView draws its own badge)
Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced" -Name "ShowTypeOverlay" -Type DWord -Value 0
Write-Host "ShowTypeOverlay -> 0 (display file icon on thumbnails OFF)"

Write-Host "=== SageThumbs cleanup done ==="
