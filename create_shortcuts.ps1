$ErrorActionPreference = "Stop"
$exe = Join-Path $PSScriptRoot "build\Release\PSVMS.exe"
if (-not (Test-Path $exe)) { $exe = Join-Path $PSScriptRoot "build\PSVMS.exe" }
if (-not (Test-Path $exe)) { throw "PSVMS.exe not found. Build the project first." }
$icon = Join-Path $PSScriptRoot "PSVMS.ico"
$wsh = New-Object -ComObject WScript.Shell
$targets = @(
    [Environment]::GetFolderPath("Desktop"),
    (Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\PSVMS")
)
foreach ($folder in $targets) {
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $lnk = $wsh.CreateShortcut((Join-Path $folder "PSVMS.lnk"))
    $lnk.TargetPath = $exe
    $lnk.WorkingDirectory = Split-Path $exe
    $lnk.IconLocation = "$icon,0"
    $lnk.Description = "PSVms — система видеонаблюдения"
    $lnk.Save()
}
Write-Host "Desktop and Start Menu shortcuts created with PSVMS.ico"
