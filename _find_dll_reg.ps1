$ErrorActionPreference = "Continue"

function Find-DllReg {
    param($root, $hiveName)
    Write-Host "=== $hiveName : search for QuickViewThumbnailProvider.dll ==="
    $found = $false
    # search InprocServer32 values
    $classes = Join-Path $root "Software\Classes"
    if (-not (Test-Path $classes)) { Write-Host "(no $classes)"; return }
    $keys = Get-ChildItem $classes -Recurse -ErrorAction SilentlyContinue
    foreach ($k in $keys) {
        $vals = Get-ItemProperty -Path $k.PSPath -ErrorAction SilentlyContinue
        foreach ($prop in $vals.PSObject.Properties) {
            if ($prop.Value -match 'QuickViewThumbnailProvider\.dll') {
                Write-Host ("  FOUND in " + $k.PSPath)
                Write-Host ("    " + $prop.Name + " = " + $prop.Value)
                $found = $true
            }
        }
    }
    if (-not $found) { Write-Host "  (not found under $classes)" }
}

Find-DllReg "HKLM:" "HKLM"
Find-DllReg "HKCU:" "HKCU"

Write-Host "=== direct .cdr probe (HKLM + HKCU) ==="
foreach ($r in @("HKLM:", "HKCU:")) {
    foreach ($base in @((Join-Path $r "Software\Classes"), $r)) {
        $p = Join-Path $base ".cdr"
        if (Test-Path $p) {
            Write-Host ("$p default = " + (Get-Item $p).GetValue([string]::Empty))
            $sh = Join-Path $p "ShellEx\{e357fccd-a995-4576-b01f-234630154e96}"
            if (Test-Path $sh) { Write-Host ("  thumb handler = " + (Get-Item $sh).GetValue([string]::Empty)) }
        }
    }
}

Write-Host "DONE"
