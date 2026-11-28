$ErrorActionPreference = 'Stop'
$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe = Join-Path $AppDir 'PS_GuardPRO.exe'
$Icon = Join-Path $AppDir 'Resources\PS_GuardPRO.ico'
if (!(Test-Path $Exe)) { throw "Не найден файл: $Exe. Скопируйте этот скрипт в папку с PS_GuardPRO.exe." }
if (!(Test-Path $Icon)) { throw "Не найден значок: $Icon" }
$Wsh = New-Object -ComObject WScript.Shell
$Desktop = [Environment]::GetFolderPath('Desktop')
$StartMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'PS GuardPRO'
New-Item -ItemType Directory -Force -Path $StartMenu | Out-Null
foreach ($Target in @((Join-Path $Desktop 'PS GuardPRO.lnk'), (Join-Path $StartMenu 'PS GuardPRO.lnk'))) {
  $Shortcut = $Wsh.CreateShortcut($Target)
  $Shortcut.TargetPath = $Exe
  $Shortcut.WorkingDirectory = $AppDir
  $Shortcut.IconLocation = "$Icon,0"
  $Shortcut.Description = 'Подготовка фотографий сотрудников и создание пропусков для Magicard HUB'
  $Shortcut.Save()
}
Write-Host 'Ярлыки рабочего стола и меню Пуск созданы.' -ForegroundColor Green
