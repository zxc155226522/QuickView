# Temporary verification script for the one-shot thumbnail worker fix.
$exe = "E:\项目\看图软件\out\build\Release-LTO\QuickView.exe"
$tests = @(
  @{ name="WIC-fallback BMP"; file="E:\项目\看图软件\_test_wic.bmp"; size=256 },
  @{ name="Bitmap CDR";       file="C:\Users\Administrator\Downloads\JRG63387烫画工艺单.cdr"; size=256 },
  @{ name="AI logo";          file="C:\Users\Administrator\Desktop\胡梅尔LOGO.ai"; size=256 }
)
foreach ($t in $tests) {
  if (-not (Test-Path $t.file)) { Write-Host ("SKIP {0}: missing" -f $t.name); continue }
  $out = Join-Path $env:TEMP ("qv_verify_{0}.bmp" -f [guid]::NewGuid().ToString("N"))
  $p = Start-Process -FilePath $exe -ArgumentList ("--thumbnail --input `"{0}`" --out `"{1}`" --size {2}" -f $t.file, $out, $t.size) -PassThru -Wait
  $code = $p.ExitCode
  $bytes = 0; $blank = $null
  if (Test-Path $out) {
    $b = [System.IO.File]::ReadAllBytes($out)
    $bytes = $b.Length
    # crude "not blank" check: count distinct-ish bytes beyond header
    $blank = ($bytes -le 54) -or ($b[54..($b.Length-1)] | Where-Object { $_ -ne $b[54] } | Select-Object -First 1 | Measure-Object).Count -eq 0
  }
  Write-Host ("TEST={0} EXIT={1} BMPbytes={2} BlankOrEmpty={3}" -f $t.name, $code, $bytes, $blank)
  Remove-Item $out -ErrorAction SilentlyContinue
}
Write-Host "=== QuickView app errors in last 2 min ==="
wevtutil qe Application /c:10 /rd:true /f:text /q:"*[System[(Level=2) and (TimeCreated[timediff(@SystemTime) <= 120000])]]" 2>$null | Select-String -Pattern "QuickView" | Select-Object -First 8
