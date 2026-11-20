# PSVms — Windows build package

This package contains the PSVms C++ source, the CMake build definition, the Windows GitHub Actions workflow, and the artwork assets required at runtime.

## What changed

The original source embedded three large images as Base64 string literals:
- AI assistant avatar (`assets/psvms_avatar.png`)
- Navigation wallpaper (`assets/psvms_wallpaper.jpg`)
- Startup splash screen (`assets/psvms_splash.png`)

Those multi-megabyte literals were removed from `psvms.cpp`. The application now loads the files from the `assets` folder next to the executable. CMake copies that folder after each build, and the workflow checks that the assets are present before and after packaging.

This directly addresses the MSVC `C2026` oversized-string error and removes the largest source of compiler memory pressure associated with the `C1060` error. The project is still a large, single C++ translation unit, so a successful Windows build must be confirmed by GitHub Actions.

## Camera-cell improvements

- Added an independent **Show FPS in camera cells** setting (enabled by default). It can be changed in Settings → Video and from a camera cell's right-click menu; the change applies immediately to all cells and is saved.
- FPS is displayed in a compact badge at the top-left of each active camera image. It measures incoming video frames, not the configured render cap.
- Kept the existing lower-left resolution and connection-quality indicator separate, so hiding FPS does not hide the other camera information.
- The cell's existing LIVE/PAUSE, audio, volume, close, timestamp, snapshot, zoom, and fullscreen controls remain in place.

## GitHub build

1. Back up the existing repository.
2. Replace the repository contents with the contents of this package, including the hidden `.github` folder.
3. Commit and push to `main` or `master`, or start **Actions → Build PSVms for Windows → Run workflow**.
4. When the workflow succeeds, download the `PSVms-windows` artifact. Keep the `assets` folder beside `PSVms.exe`.

## Local build

Install Qt 6.8.1 (MSVC 2022 x64), FFmpeg development libraries, and OpenSSL development libraries. Then configure and build:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="$env:QT_ROOT_DIR" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT\scripts\buildsystems\vcpkg.cmake"

cmake --build build --config Release --parallel 2
```

CMake automatically copies `assets` beside the executable.

## Important verification note

The source and asset extraction were checked in this environment, but this package has not been compiled with the Windows MSVC/Qt/FFmpeg toolchain here. The next authoritative check is the GitHub Actions run. If it fails, download the new complete workflow log and share it so the remaining compiler or linker issue can be fixed against the actual output.


## Camera cell size stability fix

Fixed camera tiles that could grow or shrink when playback started, stopped, or switched between the empty and video pages. The grid now owns the tile dimensions: the tile, stacked page container, empty page, and video surface use size policies that do not propagate changing content size hints. The camera title bar has a fixed 34 px height, so its controls cannot change the row height. The grid's stretch factors continue to distribute available space evenly.


## Automatic camera discovery improvements

The **Devices → Add → Automatic network search** workflow now:

- Detects a usable local IPv4 subnet from active network interfaces and pre-fills the IP range when the subnet is within the safe scan limit.
- Runs Hikvision SADP and ONVIF WS-Discovery in parallel.
- Automatically follows multicast discovery with a TCP scan of the detected subnet (or the user-entered range).
- Checks common camera/NVR ports: 80, 81, 443, 554, 8000, 8080, 8443, 8899, 34567 and 37777, plus the configured RTSP port when it is different.
- Reports the port that answered and keeps the HTTP management port separate from RTSP/SDK ports. HTTP 80 is preferred; HTTP 8080 is used when detected and no confirmed port 80 is available.
- Merges duplicate discoveries by IP and enriches the row with model/vendor details when SADP or ONVIF provides them.

The scan is limited to 2,048 IPv4 addresses per run. Multicast discovery normally works only in the same Layer-2 network. Some cameras do not expose ONVIF/SADP, use non-standard ports, require credentials, or block network probes; those devices may need manual IP/RTSP setup. An open TCP port only indicates that a service answered, not that it is definitely a camera. The application has not been compiled with the Windows Qt/MSVC toolchain in this environment.


## MSVC build fix (November 2026)

The Windows CI log reported MSVC C2338 because `qobject_cast<CameraStatusDialog*>` was used for a dialog class without `Q_OBJECT`. Both casts were changed to `dynamic_cast`, which performs a runtime-checked C++ cast and does not require adding Qt meta-object support to that class.


## Modernization baseline (November 2026)

This update makes the build more maintainable without risking a large rewrite:

- Keeps C++17 deliberately for compatibility with the current Qt 6.8 / FFmpeg / camera integration. C++17 is still a modern standard; moving to C++20 alone would not fix runtime or build defects.
- Enables stricter compiler diagnostics (`/W4`, `/permissive-`, and correct `__cplusplus` reporting on MSVC; `-Wall -Wextra -Wpedantic` on GCC/Clang). Warnings are not treated as errors yet because the existing application has not had a full warning-cleanup pass.
- Disables compiler-specific language extensions for more predictable builds.
- Adds a second GitHub Actions artifact named `PSVms-source-project`, containing the complete editable source project. The existing `PSVms-windows` artifact remains the packaged application.

### Next engineering stages

1. Stabilize and verify the Windows CI build before structural changes.
2. Extract camera workers, discovery, recording/indexing, and UI into separate source/header modules while preserving behavior.
3. Add focused tests for URL parsing, discovery result merging, camera configuration persistence, and recording segment indexing.
4. Improve runtime observability: structured logs, clearer per-camera connection states, and bounded reconnect/backoff behavior.
5. Profile multi-camera playback and recording before changing decoding or thread scheduling.

The code has not been split into modules in this update: its current declarations and implementation are tightly coupled in one translation unit, and moving them without a working Windows build would risk breaking existing features.


## Этап 2 — структурное разделение исходного кода

Реализации распределены по папке `modules/`: диалоги, страницы, главное окно,
архив и настройки, видеоподсистема, сервисы ядра и точка входа. Это первый
безопасный этап рефакторинга: `psvms.cpp` подключает модули в прежнем порядке,
поэтому сохраняются текущая видимость внутренних вспомогательных функций,
анонимные пространства имён и поведение сборки.

**Важно:** сейчас это логические модули, а не отдельные единицы компиляции.
Следующий этап — выделить публичные интерфейсы `.h` и переносить подсистемы
в отдельные `.cpp` по одной, проверяя каждую сборку Windows/Qt. Такой порядок
уменьшает риск поломать обнаружение камер, RTSP, запись и интерфейс.

## Диагностика ошибок и журналы (LOG)

При каждом запуске PSVms создаёт отдельный файл журнала с датой и временем.
Обычно журналы находятся в папке `logs` рядом с `PSVms.exe`: `logs/PSVms_ГГГГ-ММ-ДД_ЧЧ-ММ-СС.log`.
Если папка программы защищена от записи, журнал автоматически сохраняется в пользовательской папке Windows (`__PROTECTED_PSVms_5__\PSVms\logs`).

В журнал записываются этапы запуска (заставка, авторизация, создание менеджера камер и главного окна), сообщения Qt, предупреждения, критические ошибки и сведения о необработанных исключениях Windows/C++. При проблеме после заставки откройте самый новый `.log` и отправьте его разработчику целиком. В журнал не следует вручную добавлять пароли камер.

## Улучшенное логирование и диагностика (этап 2)

- В каждую строку журнала добавляются точная дата/время с миллисекундами и ID потока.
- Сообщения Qt теперь содержат уровень, категорию, файл, строку и функцию (если Qt предоставил эти данные).
- Секреты в распространённых форматах URL и полях `password` / `token` / `authorization` маскируются перед записью.
- Каталог журнала выбирается последовательно: `logs` рядом с программой, `AppLocalDataLocation/logs`, затем временный каталог `__PROTECTED_PSVms_7__`.
- Проверяется возможность реальной записи, а не только создания каталога.
- Старые журналы автоматически очищаются: сохраняются последние 20 запусков.
- В заголовок журнала добавлены версия Qt, ОС, архитектура, тип сборки, компилятор, PID, рабочий каталог и аргументы запуска.
- Для аварий Windows в запись добавляются код SEH, адрес исключения, PID и ID потока.
- Добавлены контрольные отметки настройки стиля и входа в главный цикл событий.

Журналы могут содержать имена файлов, пути и сетевые адреса. Перед публикацией журнала проверьте его содержимое; маскирование секретов покрывает типовые варианты, но не гарантирует распознавание всех пользовательских форматов.

Примечание: это расширяет диагностику, но не заменяет полноценный Windows minidump. При аварии процесса ОС может завершить приложение до выполнения обычного C++-кода.


## Дамп памяти Windows при аварии (этап 3)

При необработанном структурном исключении Windows (например, `0xC0000005`, access violation) PSVms дополнительно пытается сохранить мини-дамп памяти `.dmp` рядом с журналом. Имя файла содержит дату, время и PID процесса. Для создания используется системная `DbgHelp.dll`, загружаемая динамически, поэтому отдельная библиотека для линковки не требуется. Дамп содержит контекст исключения и сведения о потоках; строка `[CRASH]` в `.log` сообщает результат создания и путь к файлу.

Если Windows не может создать дамп (например, из-за нехватки места или ограничений доступа), основной журнал всё равно записывает сведения об аварии. Мини-дамп может содержать фрагменты данных процесса, поэтому перед отправкой учитывайте его конфиденциальность. Для диагностики отправляйте `.log` и соответствующий `.dmp` вместе.

Ограничение: дамп создаётся для необработанных Windows SEH-аварий. Перехваченные C++-исключения, штатное закрытие и принудительное завершение процесса не обязаны создавать `.dmp`.

## Редизайн интерфейса —  PREMIUM

В актуальной версии PSVms используется новая тёмно-синяя система оформления с бирюзовыми акцентами, скруглёнными поверхностями, обновлённой типографикой и единым набором значков. Новые обои, заставка и аватар встроены в ресурсы приложения. Заставка содержит анимированное HUD-кольцо и сканирующую линию. Подробности — в `REDESIGN_NOTES_RU.md`.
