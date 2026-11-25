$ErrorActionPreference = 'Stop'
$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe = Join-Path $AppDir 'PS_PassMaster_PRO.exe'
$Icon = Join-Path $AppDir 'Resources\PS_PassMaster.ico'
if (!(Test-Path $Exe)) { throw "Не найден файл: $Exe. Скопируйте этот скрипт в папку с PS_PassMaster_PRO.exe." }
if (!(Test-Path $Icon)) { throw "Не найден значок: $Icon" }
$Wsh = New-Object -ComObject WScript.Shell
$Desktop = [Environment]::GetFolderPath('Desktop')
$StartMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'PS PassMaster PRO'
New-Item -ItemType Directory -Force -Path $StartMenu | Out-Null
foreach ($Target in @((Join-Path $Desktop 'PS PassMaster PRO.lnk'), (Join-Path $StartMenu 'PS PassMaster PRO.lnk'))) {
  $Shortcut = $Wsh.CreateShortcut($Target)
  $Shortcut.TargetPath = $Exe
  $Shortcut.WorkingDirectory = $AppDir
  $Shortcut.IconLocation = "$Icon,0"
  $Shortcut.Description = 'Подготовка фотографий сотрудников и создание пропусков для Magicard HUB'
  $Shortcut.Save()
}
Write-Host 'Ярлыки рабочего стола и меню Пуск созданы.' -ForegroundColor Green
