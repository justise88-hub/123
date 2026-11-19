# Исправления ошибок сборки PSVMS (12.11.2026)

По журналу `logs_116931706170(2).zip` устранены причины MSVC exit code 1:
- `uiHeartbeatMs` и `watchdogStop` объявлены как static local, поэтому лямбды используют их без некорректного захвата по ссылке.
- Вызов сигнала таймера квалифицирован как `QObject::connect`, чтобы избежать конфликта с `connect` из Winsock.
- Поле `NumberOfThreads`, которого нет в `PROCESS_MEMORY_COUNTERS_EX`, заменено фактическим подсчётом потоков процесса через `CreateToolhelp32Snapshot` / `Thread32First` / `Thread32Next`.
- Добавлен заголовок Windows Toolhelp API.

Диагностика CPU, Working Set, Peak Working Set, Private Bytes и UI-watchdog сохранена. Реальная компиляция MSVC в этой среде не запускалась; необходим контрольный GitHub Actions build на Windows.
