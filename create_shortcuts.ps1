$ErrorActionPreference = "Stop"
$exe = Join-Path $PSScriptRoot "build\Release\PSVms.exe"
if (-not (Test-Path $exe)) { $exe = Join-Path $PSScriptRoot "build\PSVms.exe" }
if (-not (Test-Path $exe)) { throw "PSVms.exe not found. Build the project first." }
$icon = Join-Path $PSScriptRoot "PSVms.ico"
$wsh = New-Object -ComObject WScript.Shell
$targets = @(
    [Environment]::GetFolderPath("Desktop"),
    (Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\PSVms")
)
foreach ($folder in $targets) {
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $lnk = $wsh.CreateShortcut((Join-Path $folder "PSVms.lnk"))
    $lnk.TargetPath = $exe
    $lnk.WorkingDirectory = Split-Path $exe
    $lnk.IconLocation = "$icon,0"
    $lnk.Description = "PSVms — система видеонаблюдения"
    $lnk.Save()
}
Write-Host "Desktop and Start Menu shortcuts created with PSVms.ico"
