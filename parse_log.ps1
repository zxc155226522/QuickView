$lines = Get-Content 'C:\Windows\Temp\qvthumb_provider.log' -Encoding Unicode
$curPath = ''
$slow = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    if ($line -match 'Initialize:\s+(Z:\\.+)$') {
        $curPath = $Matches[1]
    }
    elseif ($curPath -and $line -match 'OK\s+\((\d+)ms\)') {
        $ms = [int]$Matches[1]
        $slow += [PSCustomObject]@{ ms = $ms; path = $curPath }
        $curPath = ''
    }
    elseif ($curPath -and $line -match '(watchdog|fallback|disk cache|failed)') {
        $slow += [PSCustomObject]@{ ms = -1; path = $curPath; note = $line }
        $curPath = ''
    }
}
$slow | Sort-Object ms -Descending | ForEach-Object {
    if ($_.ms -ge 0) {
        Write-Host ("{0,5} ms  {1}" -f $_.ms, $_.path)
    } else {
        Write-Host ("  >>>  {0}  {1}" -f $_.note, $_.path)
    }
}
Write-Host ""
Write-Host "Total: $($slow.Count) files"
$ok = $slow | Where-Object { $_.ms -ge 0 }
$avg = ($ok | Measure-Object ms -Average).Average
Write-Host "Average: $([math]::Round($avg))ms"
$over1s = ($ok | Where-Object { $_.ms -gt 1000 }).Count
Write-Host "Over 1s: $over1s files"
