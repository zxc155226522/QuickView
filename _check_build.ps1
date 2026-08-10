$log = 'C:\Users\Administrator\.catpaw\projects\idee--项目-看图软件\76f93cc9-2954-4a35-a0cd-9d719b00fac5\terminals\f0cb55fd-d5e7-49a3-99b7-c28ee2bae39b-shell-7.log'
Start-Sleep -Seconds 90
if (Test-Path $log) {
    Get-Content $log -Tail 30
} else {
    Write-Host 'Build log not found yet'
}
