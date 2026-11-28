@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
if errorlevel 1 goto :error

echo ================================================
echo   PS GuardPRO 8.10.0 - сборка Windows-программы
echo ================================================
echo.

where python >nul 2>nul
if errorlevel 1 goto :python_missing

if exist ".venv\Scripts\python.exe" goto :venv_ready
echo Создание изолированного окружения Python...
python -m venv ".venv"
if errorlevel 1 goto :error

:venv_ready
echo Установка зависимостей...
".venv\Scripts\python.exe" -m pip install --upgrade pip
if errorlevel 1 goto :error
".venv\Scripts\python.exe" -m pip install -r "requirements.txt"
if errorlevel 1 goto :error

if not exist ".venv\Lib\site-packages\cv2\data\haarcascade_frontalface_default.xml" goto :opencv_data_missing

echo Сборка приложения через PyInstaller...
".venv\Scripts\python.exe" -m PyInstaller --noconfirm --clean --windowed --onedir ^
  --name "PS_GuardPRO" ^
  --contents-directory "." ^
  --icon "Resources\PS_GuardPRO.ico" ^
  --add-data "Resources;Resources" ^
  --add-data "Templates;Templates" ^
  --add-data ".venv\Lib\site-packages\cv2\data\haarcascade_frontalface_default.xml;cv2\data" ^
  --hidden-import openpyxl ^
  --hidden-import openpyxl.cell._writer ^
  --hidden-import reportlab.pdfbase.ttfonts ^
  --hidden-import reportlab.platypus ^
  --exclude-module PySide6.QtQml ^
  --exclude-module PySide6.QtQuick ^
  --exclude-module PySide6.Qt3DCore ^
  --exclude-module PySide6.Qt3DRender ^
  --exclude-module PySide6.QtCharts ^
  --exclude-module PySide6.QtDataVisualization ^
  --exclude-module PySide6.QtWebEngineCore ^
  --exclude-module PySide6.QtWebEngineWidgets ^
  --exclude-module PySide6.QtWebEngineQuick ^
  --exclude-module PySide6.QtMultimedia ^
  --exclude-module PySide6.QtBluetooth ^
  --exclude-module PySide6.QtNfc ^
  --exclude-module PySide6.QtLocation ^
  --exclude-module PySide6.QtPositioning ^
  --exclude-module PySide6.QtSensors ^
  --exclude-module PySide6.QtSerialPort ^
  --exclude-module PySide6.QtTest ^
  --exclude-module PySide6.QtDesigner ^
  --exclude-module PySide6.QtUiTools ^
  "app\main.py"
if errorlevel 1 goto :error

if not exist "dist\PS_GuardPRO\Resources" mkdir "dist\PS_GuardPRO\Resources"
if errorlevel 1 goto :error
copy /Y "create_shortcuts.ps1" "dist\PS_GuardPRO\create_shortcuts.ps1" >nul
if errorlevel 1 goto :error

echo.
echo Сборка завершена: dist\PS_GuardPRO\PS_GuardPRO.exe
echo Для создания Setup.exe запустите build_installer.bat.
echo.
exit /b 0

:opencv_data_missing
echo ОШИБКА: не найден файл каскада OpenCV haarcascade_frontalface_default.xml.
echo Проверьте установку opencv-python-headless в .venv.
exit /b 1

:python_missing
echo ОШИБКА: Python 3.11 или 3.12 x64 не найден в PATH.
echo Установите Python с python.org и включите Add Python to PATH.
exit /b 1

:error
echo.
echo СБОРКА НЕ УДАЛАСЬ. Подробности указаны выше.
exit /b 1
