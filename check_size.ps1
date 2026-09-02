Get-ChildItem 'Z:\汇志\*.tif' | Sort-Object Length -Descending | Select-Object -First 15 @{N='SizeMB';E={[math]::Round($_.Length/1MB,1)}}, Name | Format-Table -AutoSize
