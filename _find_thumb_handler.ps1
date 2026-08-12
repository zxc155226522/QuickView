$ErrorActionPreference = "Continue"

Write-Host "=== HKCR\.cdr default value ==="
$key = "HKCR:\.cdr"
if (Test-Path $key) {
    $k = Get-Item $key
    Write-Host ("default = " + $k.GetValue([string]::Empty))
} else {
    Write-Host "(no .cdr key)"
}

Write-Host "=== .cdr Shellex thumbnail handler ==="
$p = "HKCR:\.cdr\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}"
if (Test-Path $p) {
    Write-Host ("handler clsid = " + (Get-Item $p).GetValue([string]::Empty))
} else {
    Write-Host "(no thumbnail handler key under .cdr)"
}

Write-Host "=== CLSID entries whose default name mentions quickview/thumbnail/qv ==="
$g = Get-ChildItem "HKCR:\CLSID" -ErrorAction SilentlyContinue
foreach ($c in $g) {
    $def = $c.GetValue([string]::Empty)
    if ($def -match 'quickview|thumbnail|qvthumb|qv') {
        Write-Host ($c.PSChildName + " -> " + $def)
        # print InprocServer32 path
        $inproc = Join-Path $c.PSPath "InprocServer32"
        if (Test-Path $inproc) {
            Write-Host ("   InprocServer32 = " + (Get-Item $inproc).GetValue([string]::Empty))
        }
    }
}

Write-Host "DONE"
