# 诊断 CDR 渲染问题
$exe = "E:\qv_build_tmp\out\build\Release-LTO\QuickView.exe"
$testFile = "e:\项目\看图软件\_cdr_samples_basics\03.cdr"

Write-Host "=== Test 1: CDR to PDF (full mode, MuPDF) ==="
$result = & $exe --cdr-to-pdf $testFile "e:\项目\看图软件\_test_diag.pdf" 2>&1
Write-Host $result

Write-Host ""
Write-Host "=== Test 2: Check if QuickView can open CDR directly ==="
# 启动 QuickView 并打开 CDR 文件
$proc = Start-Process -FilePath $exe -ArgumentList $testFile -PassThru
Write-Host "Started QuickView PID: $($proc.Id)"
Start-Sleep -Seconds 8
Write-Host "Process still running: $(-not $proc.HasExited)"
if (-not $proc.HasExited) {
    Write-Host "Stopping QuickView..."
    Stop-Process -Id $proc.Id -Force
}
Write-Host "Done."
