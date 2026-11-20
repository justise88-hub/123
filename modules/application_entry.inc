// PSVms module: Запуск приложения
// This file is included by psvms.cpp to preserve the original single-TU build
// while the code is being separated safely.

// app/main.cpp
// ======================================================================
// Подгоняет однострочные подписи под доступную ширину элемента.
// Базовый шрифт сохраняется отдельно, поэтому при увеличении окна текст снова растёт.
class AutoFitTextFilter : public QObject {
public:
    using QObject::QObject;

    bool eventFilter(QObject *o, QEvent *e) override
    {
        // FontChange вызывается самим setFont() ниже. Не обрабатываем его повторно:
        // иначе фильтр может рекурсивно входить в себя во время построения интерфейса.
        if (e->type() != QEvent::Resize && e->type() != QEvent::Show &&
            e->type() != QEvent::StyleChange)
            return false;

        auto *w = qobject_cast<QWidget *>(o);
        if (!w || w->property("psvmsAutoFitDisabled").toBool() ||
            w->width() < 28 || w->height() < 12)
            return false;

        auto *label = qobject_cast<QLabel *>(w);
        auto *button = qobject_cast<QAbstractButton *>(w);
        auto *lineEdit = qobject_cast<QLineEdit *>(w);
        auto *combo = qobject_cast<QComboBox *>(w);
        auto *group = qobject_cast<QGroupBox *>(w);
        auto *tabs = qobject_cast<QTabBar *>(w);

        // Текстовые редакторы и большие текстовые области намеренно не уменьшаем:
        // для них важнее прокрутка и возможность читать/редактировать длинный текст.
        if (!label && !button && !lineEdit && !combo && !group && !tabs)
            return false;
        if (label && label->textFormat() == Qt::RichText)
            return false;

        // Защита от повторного входа при изменении шрифта/геометрии виджета.
        if (w->property("psvmsAutoFitInProgress").toBool())
            return false;
        w->setProperty("psvmsAutoFitInProgress", true);
        [[maybe_unused]] const auto autoFitGuard = qScopeGuard([w] {
            w->setProperty("psvmsAutoFitInProgress", false);
        });

        QFont base;
        const QVariant saved = w->property("psvmsAutoFitBaseFont");
        if (saved.isValid())
            base = saved.value<QFont>();
        else {
            base = w->font();
            w->setProperty("psvmsAutoFitBaseFont", base);
        }
        if (base.pointSizeF() <= 0.0)
            return false;

        auto fitText = [](const QFont &source, const QString &caption,
                          int availableWidth, int availableHeight,
                          bool wrap, int minimumPt) {
            QFont result = source;
            if (caption.isEmpty() || availableWidth < 8 || availableHeight < 8)
                return result;
            qreal pointSize = source.pointSizeF();
            for (; pointSize >= minimumPt; pointSize -= 0.5) {
                result.setPointSizeF(pointSize);
                const QFontMetrics fm(result);
                const QRect bounds = fm.boundingRect(
                    QRect(0, 0, availableWidth, availableHeight),
                    wrap ? (Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignVCenter)
                         : (Qt::TextSingleLine | Qt::AlignLeft | Qt::AlignVCenter),
                    caption);
                if (bounds.width() <= availableWidth && bounds.height() <= availableHeight)
                    break;
            }
            result.setPointSizeF(qMax<qreal>(minimumPt, pointSize));
            return result;
        };

        if (tabs) {
            // Все подписи вкладок используют единый размер, рассчитанный по самой длинной.
            int widest = 0;
            for (int i = 0; i < tabs->count(); ++i)
                widest = qMax(widest, tabs->tabRect(i).width() - 18);
            QFont fitted = base;
            for (int i = 0; i < tabs->count(); ++i) {
                fitted = fitText(base, tabs->tabText(i), widest,
                                 qMax(12, tabs->height() - 4), false, 10);
                if (fitted.pointSizeF() < base.pointSizeF())
                    break;
            }
            if (tabs->font() != fitted)
                tabs->setFont(fitted);
            return false;
        }

        QString caption;
        bool wrap = false;
        int horizontalPadding = 10;
        int verticalPadding = 4;
        if (label) {
            caption = label->text();
            wrap = label->wordWrap();
            horizontalPadding = 8;
            verticalPadding = 4;
        } else if (button) {
            caption = button->text();
            horizontalPadding = button->icon().isNull() ? 24 : 42;
            verticalPadding = 10;
        } else if (lineEdit) {
            caption = lineEdit->text().isEmpty()
                ? lineEdit->placeholderText() : lineEdit->text();
            horizontalPadding = 18;
            verticalPadding = 8;
        } else if (combo) {
            caption = combo->currentText().isEmpty()
                ? combo->placeholderText() : combo->currentText();
            horizontalPadding = 34;
            verticalPadding = 8;
        } else if (group) {
            caption = group->title();
            horizontalPadding = 24;
            verticalPadding = 8;
        }

        const int availableWidth = qMax(8, w->contentsRect().width() - horizontalPadding);
        const int availableHeight = qMax(8, w->contentsRect().height() - verticalPadding);
        const QFont fitted = fitText(base, caption, availableWidth, availableHeight, wrap, 10);
        if (w->font() != fitted)
            w->setFont(fitted);
        return false;
    }
};

// Windows 10/11: у каждого окна программы тёмная рамка заголовка
class DarkTitleFilter : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Show && o->isWidgetType()) {
            auto *w = static_cast<QWidget *>(o);
            if (w->windowType() == Qt::Window || w->windowType() == Qt::Dialog)
                applyDarkTitleBar(w);
        }
        return false;
    }
};

namespace {
QMutex g_psvmsLogMutex;
QString g_psvmsLogFilePath;
QString g_psvmsLogDirPath;
#ifdef Q_OS_WIN
std::wstring g_psvmsLogFilePathWide;
std::wstring g_psvmsDumpFilePathWide;
using PsvmsMiniDumpWriteDumpFn = BOOL (WINAPI *)(
    HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);
HMODULE g_psvmsDbgHelpModule = nullptr;
PsvmsMiniDumpWriteDumpFn g_psvmsMiniDumpWriteDump = nullptr;
#endif

QString psvmsLogTypeName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return QStringLiteral("DEBUG");
    case QtInfoMsg:     return QStringLiteral("INFO");
    case QtWarningMsg:  return QStringLiteral("WARNING");
    case QtCriticalMsg: return QStringLiteral("CRITICAL");
    case QtFatalMsg:    return QStringLiteral("FATAL");
    }
    return QStringLiteral("UNKNOWN");
}

QString psvmsRedactSecrets(QString value)
{
    // Журнал не должен случайно сохранять пароли из RTSP/HTTP URL.
    static const QRegularExpression urlCredentials(
        QStringLiteral(R"((://[^:/@\s]+:)[^@/\s]+(@))"),
        QRegularExpression::CaseInsensitiveOption);
    value.replace(urlCredentials, QStringLiteral("\\1<REDACTED>\\2"));
    static const QRegularExpression namedSecrets(
        QStringLiteral(R"(((?:password|passwd|pwd|token|authorization)\s*[=:]\s*)[^\s&,;]+)"),
        QRegularExpression::CaseInsensitiveOption);
    value.replace(namedSecrets, QStringLiteral("\\1<REDACTED>"));
    return value;
}

void psvmsPruneOldLogs(const QString &directory)
{
    QDir dir(directory);
    const QFileInfoList files = dir.entryInfoList(
        QStringList{QStringLiteral("__PROTECTED_PSVms_8__")}, QDir::Files, QDir::Time);
    // Оставляем последние 20 запусков, не удаляя текущий файл.
    for (int i = 20; i < files.size(); ++i)
        QFile::remove(files.at(i).absoluteFilePath());
}
}

// Пишем журнал с защитой от повторного входа: QFile/Qt могут выдать
// диагностическое сообщение при ошибке открытия/записи, а Qt message handler
// снова вызывает writePsvmsLog. Без guard это приводит к взаимной блокировке.
static void writePsvmsLog(const QString &message)
{
    static thread_local bool insideWrite = false;
    if (insideWrite)
        return;
    insideWrite = true;
    struct PsvmsLogWriteGuard {
        bool &flag;
        ~PsvmsLogWriteGuard() { flag = false; }
    } resetGuard{insideWrite};

    QMutexLocker locker(&g_psvmsLogMutex);
    if (g_psvmsLogFilePath.isEmpty())
        return;

    const QString thread = QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16);
    const QByteArray line = QStringLiteral("[%1] [T:%2] %3\r\n")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
             thread, psvmsRedactSecrets(message)).toUtf8();

#ifdef Q_OS_WIN
    // Win32-запись не вызывает Qt message handler при ошибке файлового ввода-вывода.
    if (!g_psvmsLogFilePathWide.empty()) {
        HANDLE file = CreateFileW(g_psvmsLogFilePathWide.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, line.constData(), static_cast<DWORD>(line.size()), &written, nullptr);
            FlushFileBuffers(file);
            CloseHandle(file);
        }
        return;
    }
#endif

    QFile file(g_psvmsLogFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    file.write(line);
    file.flush();
}

static void psvmsQtMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QString line = QStringLiteral("[%1] %2").arg(psvmsLogTypeName(type), message);
    if (context.category && *context.category)
        line += QStringLiteral(" [категория: %1]").arg(QString::fromUtf8(context.category));
    if (context.file && *context.file)
        line += QStringLiteral(" [файл: %1:%2]").arg(QString::fromUtf8(context.file)).arg(context.line);
    if (context.function && *context.function)
        line += QStringLiteral(" [функция: %1]").arg(QString::fromUtf8(context.function));
    writePsvmsLog(line);
    if (type == QtFatalMsg)
        std::abort();
}

static void psvmsTerminateHandler()
{
    QString details = QStringLiteral("НЕОБРАБОТАННОЕ ИСКЛЮЧЕНИЕ C++ / std::terminate");
    const std::exception_ptr ex = std::current_exception();
    if (ex) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception &e) {
            details += QStringLiteral(": %1").arg(QString::fromLocal8Bit(e.what()));
        } catch (...) {
            details += QStringLiteral(": неизвестное исключение");
        }
    }
    writePsvmsLog(details);
    std::abort();
}

#ifdef Q_OS_WIN
// SEH-обработчик использует только Win32 API: Qt в аварийном контексте небезопасен.
static void psvmsWriteCrashLine(const char *line, size_t length)
{
    if (g_psvmsLogFilePathWide.empty() || !line || length == 0)
        return;
    HANDLE file = CreateFileW(g_psvmsLogFilePathWide.c_str(),
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
}

static bool psvmsCreateMiniDump(EXCEPTION_POINTERS *info)
{
    if (g_psvmsDumpFilePathWide.empty())
        return false;

    HANDLE dumpFile = CreateFileW(g_psvmsDumpFilePathWide.c_str(),
        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dumpFile == INVALID_HANDLE_VALUE)
        return false;

    // DbgHelp заранее загружена при старте, чтобы не вызывать загрузчик DLL
    // из контекста аварийного обработчика.
    if (!g_psvmsMiniDumpWriteDump) {
        CloseHandle(dumpFile);
        DeleteFileW(g_psvmsDumpFilePathWide.c_str());
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = info;
    exceptionInfo.ClientPointers = FALSE;

    const BOOL ok = g_psvmsMiniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), dumpFile,
        MiniDumpWithThreadInfo, info ? &exceptionInfo : nullptr,
        nullptr, nullptr);
    FlushFileBuffers(dumpFile);
    CloseHandle(dumpFile);

    if (!ok) {
        DeleteFileW(g_psvmsDumpFilePathWide.c_str());
        return false;
    }
    return true;
}

static LONG WINAPI psvmsUnhandledExceptionFilter(EXCEPTION_POINTERS *info)
{
    const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    const void *address = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    const DWORD threadId = GetCurrentThreadId();
    const DWORD processId = GetCurrentProcessId();
    const bool dumpCreated = psvmsCreateMiniDump(info);

    char buffer[768] = {};
    const int len = std::snprintf(buffer, sizeof(buffer),
        "[CRASH] Windows SEH: code=0x%08lX address=%p thread=%lu process=%lu minidump=%s path=%ls\r\n",
        static_cast<unsigned long>(code), address,
        static_cast<unsigned long>(threadId), static_cast<unsigned long>(processId),
        dumpCreated ? "created" : "failed",
        g_psvmsDumpFilePathWide.empty() ? L"(unavailable)" : g_psvmsDumpFilePathWide.c_str());
    if (len > 0)
        psvmsWriteCrashLine(buffer, static_cast<size_t>(len));
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// Early log is also used inside logging initialization, so a stalled step is visible.
static QString g_psvmsEarlyLogPath;
static void psvmsAppendEarlyStartupLog(const QString &path, const QString &stage);

static void initializePsvmsLogging()
{
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: entered"));
    QStringList candidates;
    candidates << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logs"));
    candidates << QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                    .filePath(QStringLiteral("logs"));
    candidates << QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                    .filePath(QStringLiteral("__PROTECTED_PSVms_7__"));

    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: candidates prepared"));
    for (const QString &candidate : candidates) {
        psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: testing directory: %1").arg(candidate));
        if (QDir().mkpath(candidate)) {
            const QString probe = QDir(candidate).filePath(QStringLiteral(".write_test"));
            QFile test(probe);
            if (test.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                test.close();
                QFile::remove(probe);
                g_psvmsLogDirPath = candidate;
                psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: writable directory selected"));
                break;
            }
        }
    }
    if (g_psvmsLogDirPath.isEmpty()) {
        // Keep logging active in memory-independent emergency fallback if disk is unavailable.
#ifdef Q_OS_WIN
        OutputDebugStringW(L"PSVms: unable to create a writable log directory\n");
#endif
        return;
    }

    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: directory selection complete"));
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    g_psvmsLogFilePath = QDir(g_psvmsLogDirPath).filePath(QStringLiteral("__PROTECTED_PSVms_9__").arg(stamp));
#ifdef Q_OS_WIN
    g_psvmsLogFilePathWide = g_psvmsLogFilePath.toStdWString();
    g_psvmsDumpFilePathWide = QDir(g_psvmsLogDirPath)
        .filePath(QStringLiteral("__PROTECTED_PSVms_10__").arg(stamp).arg(QCoreApplication::applicationPid()))
        .toStdWString();
#endif
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: before installing Qt message handler"));
    qInstallMessageHandler(psvmsQtMessageHandler);
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: Qt message handler installed"));
    std::set_terminate(psvmsTerminateHandler);
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: terminate handler installed"));
#ifdef Q_OS_WIN
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: before loading DbgHelp.dll"));
    g_psvmsDbgHelpModule = LoadLibraryW(L"DbgHelp.dll");
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: after loading DbgHelp.dll; loaded=%1").arg(g_psvmsDbgHelpModule != nullptr));
    if (g_psvmsDbgHelpModule) {
        g_psvmsMiniDumpWriteDump = reinterpret_cast<PsvmsMiniDumpWriteDumpFn>(
            GetProcAddress(g_psvmsDbgHelpModule, "MiniDumpWriteDump"));
    }
    if (!g_psvmsMiniDumpWriteDump)
        writePsvmsLog(QStringLiteral("Предупреждение: MiniDumpWriteDump недоступна; .dmp создаваться не будут"));
    SetUnhandledExceptionFilter(psvmsUnhandledExceptionFilter);
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: crash filter installed"));
#endif
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: before pruning old logs"));
    psvmsPruneOldLogs(g_psvmsLogDirPath);
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: after pruning old logs"));
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: before first writePsvmsLog"));
    writePsvmsLog(QStringLiteral("============================================================"));
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: after first writePsvmsLog"));
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: before startup header write"));
    writePsvmsLog(QStringLiteral("Запуск PSVms; журнал: %1").arg(g_psvmsLogFilePath));
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: after startup header write"));
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: before app version write"));
    writePsvmsLog(QStringLiteral("Версия приложения: %1").arg(App::title()));
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: after app version write"));
    writePsvmsLog(QStringLiteral("Версия Qt: %1; ОС: %2; архитектура: %3-bit")
        .arg(QString::fromLatin1(qVersion()), QSysInfo::prettyProductName())
        .arg(QSysInfo::WordSize));
#ifdef _MSC_VER
    const QString compiler = QStringLiteral("MSVC %1").arg(_MSC_VER);
#else
    const QString compiler = QStringLiteral("неизвестен");
#endif
#ifdef NDEBUG
    const QString buildType = QStringLiteral("Release");
#else
    const QString buildType = QStringLiteral("Debug");
#endif
    writePsvmsLog(QStringLiteral("Компилятор: %1; сборка: %2").arg(compiler, buildType));
    writePsvmsLog(QStringLiteral("PID: %1; папка программы: %2")
        .arg(QCoreApplication::applicationPid())
        .arg(QCoreApplication::applicationDirPath()));
    writePsvmsLog(QStringLiteral("Рабочая папка: %1").arg(QDir::currentPath()));
    writePsvmsLog(QStringLiteral("Аргументы запуска: %1").arg(psvmsRedactSecrets(QCoreApplication::arguments().join(QLatin1Char(' ')))));
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("logging init: completed all log header writes"));
}


// Самая ранняя диагностика: выполняется ДО создания QApplication.
// Это позволяет отличить зависание конструктора QApplication от более поздних этапов.
static QString psvmsCreateEarlyStartupLog(int argc, char *argv[])
{
    QString exePath = argc > 0 && argv && argv[0]
        ? QFileInfo(QString::fromLocal8Bit(argv[0])).absoluteFilePath()
        : QDir::current().filePath(QStringLiteral("PSVms.exe"));
    const QString logDir = QDir(QFileInfo(exePath).absolutePath()).filePath(QStringLiteral("logs"));
    if (!QDir().mkpath(logDir))
        return {};
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz"));
    const QString filePath = QDir(logDir).filePath(QStringLiteral("__PROTECTED_PSVms_11__")
        .arg(stamp).arg(QCoreApplication::applicationPid()));
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return {};
    const QByteArray line = QStringLiteral("[%1] [EARLY] main() entered; executable=%2; argc=%3\r\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), exePath)
        .arg(argc).toUtf8();
    file.write(line);
    file.flush();
    return filePath;
}

static void psvmsAppendEarlyStartupLog(const QString &path, const QString &stage)
{
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    const QByteArray line = QStringLiteral("[%1] [EARLY] %2\r\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), stage).toUtf8();
    file.write(line);
    file.flush();
}

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // Захватываем системный mutex ДО QApplication и любых окон/инициализации.
    // Имя фиксированное для всех копий PSVms текущего пользователя/сеанса Windows.
    HANDLE psvmsInstanceMutex = CreateMutexW(nullptr, TRUE, L"__PROTECTED_PSVms_0__");
    if (!psvmsInstanceMutex) {
        OutputDebugStringW(L"PSVms: не удалось создать mutex единственного экземпляра\\n");
        return 10;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(psvmsInstanceMutex);
        OutputDebugStringW(L"PSVms: повторный запуск заблокирован; первый экземпляр уже работает\\n");
        return 0;
    }
#endif
    const QString earlyLogPath = psvmsCreateEarlyStartupLog(argc, argv);
    g_psvmsEarlyLogPath = earlyLogPath;
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("main: before application identity"));
    QCoreApplication::__PROTECTED_PSVms_2__;
    QCoreApplication::__PROTECTED_PSVms_3__;
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("main: application identity set"));

    // Важно: QSettings должен быть направлен в профиль текущего компьютера ДО
    // первого чтения настроек. Каталог вычисляется от папки EXE и machine-id.
    const QString executableDir = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
    AppPaths::initSettingsLocation(executableDir);
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath,
        QStringLiteral("main: computer-bound settings profile: %1").arg(AppPaths::dataDir()));

    // Автомасштабирование по разрешению экрана (1920x1080 компактнее, 3440x1440 без уменьшения)
    {
        psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("main: before reading scale settings"));
        QSettings settings;
        const double presetScale = settings.value(QStringLiteral("ui/scale"), 0.0).toDouble();
        const double manualScale = qBound(75, settings.value(QStringLiteral("ui/manualInterfacePercent"), 100).toInt(), 200) / 100.0;
        const bool autoScreenScale = settings.value(QStringLiteral("ui/autoScreenScale"), true).toBool();
        const double fhdAutoFactor = qBound(70, settings.value(QStringLiteral("ui/fullHdAutoScalePercent"), 85).toInt(), 125) / 100.0;

        double baseScale = presetScale > 0.0 ? presetScale : 1.0;
        if (presetScale <= 0.0 && autoScreenScale) {
#ifdef Q_OS_WIN
            const int scrW = GetSystemMetrics(SM_CXSCREEN);
            const int scrH = GetSystemMetrics(SM_CYSCREEN);
            if (scrW > 0 && scrH > 0) {
                if (scrW <= 1920 && scrH <= 1080) {
                    baseScale = fhdAutoFactor;
                } else if (scrW >= 3440) {
                    baseScale = 1.0;
                }
            }
#endif
        }

        const double scale = baseScale * manualScale;
        if (scale >= 0.75 && scale <= 4.0 && qAbs(scale - 1.0) > 0.001)
            qputenv("QT_SCALE_FACTOR", QByteArray::number(scale, 'f', 3));
    }
    psvmsAppendEarlyStartupLog(g_psvmsEarlyLogPath, QStringLiteral("main: scale settings processed"));
    // Дробное масштабирование Windows 10/11 (125%, 150%) без искажений
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    psvmsAppendEarlyStartupLog(earlyLogPath, QStringLiteral("before QApplication constructor"));
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(QStringLiteral(":/branding/taskbar.png")));
#ifdef Q_OS_WIN
    // Если PSVms запущена непосредственно с USB-накопителя, отслеживаем его
    // присутствие. При извлечении закрываем главное окно штатно, чтобы камеры,
    // запись и настройки получили обычную процедуру завершения.
    const QString launchedExe = QCoreApplication::applicationFilePath();
    const QString launchedDir = QFileInfo(launchedExe).absolutePath();
    QString launchRoot = QDir::toNativeSeparators(launchedDir);
    if (launchRoot.size() >= 3 && launchRoot.at(1) == QLatin1Char(':'))
        launchRoot = launchRoot.left(3);
    const bool launchedFromRemovable = !launchRoot.isEmpty()
        && GetDriveTypeW(reinterpret_cast<LPCWSTR>(launchRoot.utf16())) == DRIVE_REMOVABLE;
    if (launchedFromRemovable) {
        auto *mediaWatch = new QTimer(&app);
        mediaWatch->setInterval(300);
        QObject::connect(mediaWatch, &QTimer::timeout, &app, [&app, launchedExe, launchRoot, mediaWatch] {
            if (QFileInfo::exists(launchedExe) && QDir(launchRoot).exists()) return;
            mediaWatch->stop();
            writePsvmsLog(QStringLiteral("USB-носитель с программой извлечён; закрываются все окна и завершается PSVms"));
            // Закрываем и главное окно, и модальные диалоги (включая выбор пользователя).
            const auto windows = QApplication::topLevelWidgets();
            for (QWidget *widget : windows) {
                if (!widget) continue;
                widget->hide();
                if (auto *dialog = qobject_cast<QDialog *>(widget))
                    dialog->reject();
                else
                    widget->close();
            }
            QTimer::singleShot(0, &app, [&app] { app.exit(0); });
        });
        mediaWatch->start();
    }
#endif
    psvmsAppendEarlyStartupLog(earlyLogPath, QStringLiteral("after QApplication constructor"));
    // До показа главного окна закрытие заставки/диалогов авторизации не должно
    // завершать приложение. Включим quitOnLastWindowClosed только перед app.exec().
    app.setQuitOnLastWindowClosed(false);
    psvmsAppendEarlyStartupLog(earlyLogPath, QStringLiteral("before initializePsvmsLogging"));
    initializePsvmsLogging();
    psvmsAppendEarlyStartupLog(earlyLogPath, QStringLiteral("after initializePsvmsLogging"));
    writePsvmsLog(QStringLiteral("Этап запуска: QApplication создан"));
#ifdef Q_OS_WIN
    // Аварийное закрытие работает в отдельном системном потоке, поэтому
    // доступно даже тогда, когда главный поток Qt полностью заблокирован.
    // Ctrl+Alt+F12 -> отдельное системное подтверждение -> принудительное завершение.
    std::thread([] {
        bool wasDown = false;
        writePsvmsLog(QStringLiteral("Аварийная комбинация закрытия активна: Ctrl+Alt+F12"));
        for (;;) {
            const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                              (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                              (GetAsyncKeyState(VK_F12) & 0x8000);
            if (down && !wasDown) {
                const int answer = MessageBoxW(nullptr,
                    L"PSVms будет принудительно закрыта.\n\n"
                    L"Используйте это только если программа не отвечает.\n"
                    L"Несохранённые изменения могут быть потеряны.\n\n"
                    L"Завершить PSVms сейчас?",
                    L"PSVms — аварийное закрытие",
                    MB_YESNO | MB_ICONWARNING | MB_SYSTEMMODAL | MB_SETFOREGROUND);
                if (answer == IDYES) {
                    OutputDebugStringW(L"PSVms: emergency close requested by Ctrl+Alt+F12\n");
                    // Не вызываем Qt cleanup: главный поток может держать его mutex.
                    // Windows освободит все ресурсы процесса при завершении.
                    TerminateProcess(GetCurrentProcess(), 0xE001);
                    return;
                }
            }
            wasDown = down;
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
    }).detach();
#endif

    // Защита от нескольких экземпляров. Файл блокировки находится в профиле
    // приложения, а не рядом с EXE, поэтому работает и в Program Files.
    const QString instanceDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(instanceDir)) {
        writePsvmsLog(QStringLiteral("ПРЕДУПРЕЖДЕНИЕ: не удалось создать каталог блокировки: %1").arg(instanceDir));
    }
    QLockFile instanceLock(QDir(instanceDir).filePath(QStringLiteral("psvms-instance.lock")));
    instanceLock.setStaleLockTime(30000);
    if (!instanceLock.tryLock(100)) {
        qint64 ownerPid = 0;
        QString ownerHost;
        QString ownerApp;
        instanceLock.getLockInfo(&ownerPid, &ownerHost, &ownerApp);
        writePsvmsLog(QStringLiteral("Повторный запуск заблокирован. Владелец блокировки: PID=%1, host=%2, app=%3")
            .arg(ownerPid).arg(ownerHost, ownerApp));
        QMessageBox::information(nullptr, QStringLiteral("PSVms уже запущен"),
            QStringLiteral("Программа PSVms уже работает.\n\nПроверьте открытые окна или завершите предыдущий процесс в Диспетчере задач перед повторным запуском."));
        return 0;
    }
    writePsvmsLog(QStringLiteral("Защита от повторного запуска активирована; файл блокировки: %1")
        .arg(QDir(instanceDir).filePath(QStringLiteral("psvms-instance.lock"))));
    app.setApplicationDisplayName(App::title());
    writePsvmsLog(QStringLiteral("Этап запуска: инициализация локализации — начало"));
    I18n::init();      // язык из настроек или по языку системы
    writePsvmsLog(QStringLiteral("Этап запуска: локализация готова"));
    Cfg::reload();
    writePsvmsLog(QStringLiteral("Этап запуска: настройки загружены"));

    writePsvmsLog(QStringLiteral("Этап запуска: настройка стиля интерфейса — начало"));
    app.setStyle(QStringLiteral("Fusion"));
    // Безопасная типографика для Windows 10/11.
    // В дампе Windows 11 26H1 переполнение стека происходит внутри Qt6Gui
    // при первом рисовании главного окна. В предыдущем журнале выбирался
    // variable-шрифт Segoe UI Variable. Исключаем variable-font и принудительные
    // стратегии DirectWrite: используем обычный статический Segoe UI и настройки
    // растеризации Qt по умолчанию. Это также не зависит от списка установленных
    // шрифтов и исключает динамический выбор шрифта во время старта.
    const QString uiFamily = QStringLiteral("Segoe UI");
    QFont uiFont(uiFamily, 10);
    uiFont.setWeight(QFont::Normal);
    const int manualTextPercent = qBound(75, QSettings().value(QStringLiteral("ui/manualTextPercent"), 100).toInt(), 200);
    uiFont.setPointSizeF(uiFont.pointSizeF() * manualTextPercent / 100.0);
    app.setFont(uiFont);
    writePsvmsLog(QStringLiteral("Этап запуска: безопасное семейство шрифтов выбрано: %1; стратегии растеризации Qt по умолчанию").arg(uiFamily));
    QString effectiveStyle = appStyleSheet();
    const int elementPercent = qBound(75, QSettings().value(QStringLiteral("ui/manualElementsPercent"), 100).toInt(), 200);
    const double elementFactor = elementPercent / 100.0;
    // Компактные элементы управления в рабочих страницах. Пункты навигации
    // имеют собственные objectName-стили и сохраняют исходные габариты.
    const int controlMinHeight = qMax(27, qRound(30 * elementFactor));
    const int controlPadV = qMax(3, qRound(4 * elementFactor));
    const int controlPadH = qMax(6, qRound(8 * elementFactor));
    effectiveStyle += QStringLiteral("\nQPushButton, QToolButton, QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox { min-height:%1px; padding:%2px %3px; font-size:10pt; }\n")
        .arg(controlMinHeight).arg(controlPadV).arg(controlPadH);
    writePsvmsLog(QStringLiteral("Этап запуска: таблица стилей сформирована; длина=%1").arg(effectiveStyle.size()));
    app.setStyleSheet(effectiveStyle);
    writePsvmsLog(QStringLiteral("Этап запуска: таблица стилей установлена"));
    DarkTitleFilter titleFilter;
    AutoFitTextFilter autoFitTextFilter;
    app.installEventFilter(&titleFilter);
    // AutoFitTextFilter подключается только после авторизации.
    // Это исключает его обработку событий элементов окна входа
    // и позволяет проверить переполнение стека в QtGui/DirectWrite.
    app.removeEventFilter(&autoFitTextFilter);
    writePsvmsLog(QStringLiteral("Этап запуска: фильтр заголовка установлен; автоподгонка текста отложена до входа"));

    // После заставки показывается выбор учётной записи; гостевой режим предлагается первым.
    // Пользователи создаются без пароля, кроме защищённой учётной записи Разработчик.

    // ---- заставка ----
    // Заставка — обязательный экран запуска, а не эффект интерфейса.
    // Режим «Простой» отключает анимации рабочих страниц, но не заставку.
    // Пропускать заставку можно только явным параметром --no-splash.
    const bool skipSplash = QCoreApplication::arguments().contains(QStringLiteral("--no-splash"));
    writePsvmsLog(QStringLiteral("Этап запуска: заставка; пропуск=%1").arg(skipSplash));
    QSplashScreen *splash = nullptr;
    if (!skipSplash) {
        writePsvmsLog(QStringLiteral("Этап запуска: создание заставки — начало"));
        splash = createSplash();
        writePsvmsLog(QStringLiteral("Этап запуска: создание заставки — завершено"));
    }
    writePsvmsLog(QStringLiteral("Этап запуска: заставка создана/пропущена"));
    writePsvmsLog(QStringLiteral("Этап запуска: проверка и создание учётной записи по умолчанию — начало"));
    Users::ensureDefault();
    writePsvmsLog(QStringLiteral("Этап запуска: проверка учётной записи по умолчанию завершена"));
    // Не блокируем главный поток циклом processEvents + sleep:
    // Qt сам обслуживает перерисовку заставки и системные события.
    if (!skipSplash) {
        writePsvmsLog(QStringLiteral("Этап запуска: ожидание заставки — начало"));
        QEventLoop splashWait;
        // Длительность заставки: 3,5 секунды.
        QTimer::singleShot(3500, &splashWait, &QEventLoop::quit);
        splashWait.exec();
        writePsvmsLog(QStringLiteral("Этап запуска: ожидание заставки — завершено"));
        finishSplash(splash);
        splash = nullptr;
        writePsvmsLog(QStringLiteral("Этап запуска: заставка закрыта"));
    } else {
        writePsvmsLog(QStringLiteral("Этап запуска: заставка пропущена по параметру --no-splash"));
    }

    int rc = 0;
    std::unique_ptr<CameraManager> manager;
    try {
    for (;;) {
        // При каждом запуске и переключении сеанса пользователь выбирает учётную запись в диалоге.
        writePsvmsLog(QStringLiteral("Этап запуска: открытие окна авторизации"));
        if (!authenticate(nullptr)) {
            writePsvmsLog(QStringLiteral("Авторизация закрыта пользователем; завершение запуска"));
            break;
        }
        writePsvmsLog(QStringLiteral("Этап запуска: авторизация завершена"));
        // Глобальный AutoFitTextFilter на уровне QApplication отключён.
        // По журналу пользователя авария возникает непосредственно внутри
        // showMaximized() сразу после установки этого фильтра: он получает
        // Show/Resize от всего дерева виджетов и меняет шрифты во время
        // показа главного окна. Это создаёт небезопасную повторную обработку
        // событий Qt. Локальные CameraButtonAutoFit-фильтры, установленные
        // непосредственно на страницах камер, продолжают работать.
        app.removeEventFilter(&autoFitTextFilter);
        writePsvmsLog(QStringLiteral("Этап запуска: глобальная автоподгонка отключена; локальная автоподгонка страниц сохранена"));
        if (!manager) {
            writePsvmsLog(QStringLiteral("Этап запуска: создание CameraManager"));
            manager = std::make_unique<CameraManager>();   // при выключенном автозапуске камеры не стартуют
            writePsvmsLog(QStringLiteral("Этап запуска: CameraManager создан"));
        }

        {
            writePsvmsLog(QStringLiteral("Этап запуска: создание главного окна MainWindow"));
            MainWindow window(manager.get());
            writePsvmsLog(QStringLiteral("Этап запуска: MainWindow создан"));
            // На части систем Windows 11 showMaximized() аварийно завершает
            // процесс при первом создании нативного окна. Устанавливаем состояние
            // до показа и вызываем обычный show(): Qt применит Maximized при
            // создании HWND, минуя отдельный путь showMaximized(). Заголовок и
            // системные кнопки окна остаются доступны (это не fullscreen).
            writePsvmsLog(QStringLiteral("Этап запуска: подготовка состояния окна Maximized"));
            window.setWindowState(window.windowState() | Qt::WindowMaximized);
            writePsvmsLog(QStringLiteral("Этап запуска: состояние Maximized установлено; вызов QWidget::show"));
            window.show();
            writePsvmsLog(QStringLiteral("Этап запуска: QWidget::show завершён"));
            app.setQuitOnLastWindowClosed(true);
            writePsvmsLog(QStringLiteral("Этап запуска: главное окно показано; автоматическое завершение при закрытии последнего окна включено"));
            // Диагностика ресурсов и отзывчивости. Таймер пишет показатели раз в
            // 5 секунд; независимый watchdog замечает блокировку UI-потока,
            // даже когда цикл событий Qt перестал обрабатывать таймеры.
            static std::atomic<qint64> uiHeartbeatMs{0};
            static std::atomic<bool> watchdogStop{false};
            uiHeartbeatMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            watchdogStop.store(false);
            QTimer diagnosticsTimer;
            diagnosticsTimer.setInterval(5000);
            QObject::connect(&diagnosticsTimer, &QTimer::timeout, &app, [] {
                const qint64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                uiHeartbeatMs.store(now);
#ifdef Q_OS_WIN
                PROCESS_MEMORY_COUNTERS_EX memory{};
                memory.cb = sizeof(memory);
                FILETIME created{}, exited{}, kernel{}, user{};
                double processCpuPercent = -1.0;
                static quint64 previousProcessCpu100ns = 0;
                static qint64 previousWallMs = 0;
                if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
                    ULARGE_INTEGER k{}, u{};
                    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
                    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
                    const quint64 totalProcessCpu = k.QuadPart + u.QuadPart;
                    if (previousWallMs > 0 && now > previousWallMs) {
                        const double wall100ns = double(now - previousWallMs) * 10000.0;
                        // Значение процесса нормировано на одно логическое ядро; поэтому может быть >100%.
                        processCpuPercent = 100.0 * double(totalProcessCpu - previousProcessCpu100ns) / wall100ns;
                    }
                    previousProcessCpu100ns = totalProcessCpu;
                    previousWallMs = now;
                }

                // Общая загрузка процессора Windows, независимо от нагрузки PSVms.
                double systemCpuPercent = -1.0;
                FILETIME idleTime{}, kernelTime{}, userTime{};
                static quint64 previousSystemIdle100ns = 0;
                static quint64 previousSystemTotal100ns = 0;
                if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
                    ULARGE_INTEGER idle{}, systemKernel{}, systemUser{};
                    idle.LowPart = idleTime.dwLowDateTime; idle.HighPart = idleTime.dwHighDateTime;
                    systemKernel.LowPart = kernelTime.dwLowDateTime; systemKernel.HighPart = kernelTime.dwHighDateTime;
                    systemUser.LowPart = userTime.dwLowDateTime; systemUser.HighPart = userTime.dwHighDateTime;
                    const quint64 idleNow = idle.QuadPart;
                    const quint64 totalNow = systemKernel.QuadPart + systemUser.QuadPart;
                    if (previousSystemTotal100ns > 0 && totalNow > previousSystemTotal100ns) {
                        const quint64 totalDelta = totalNow - previousSystemTotal100ns;
                        const quint64 idleDelta = idleNow >= previousSystemIdle100ns
                            ? idleNow - previousSystemIdle100ns : 0;
                        systemCpuPercent = 100.0 * double(totalDelta - qMin(totalDelta, idleDelta)) / double(totalDelta);
                    }
                    previousSystemIdle100ns = idleNow;
                    previousSystemTotal100ns = totalNow;
                }

                MEMORYSTATUSEX systemMemory{};
                systemMemory.dwLength = sizeof(systemMemory);
                const bool hasSystemMemory = GlobalMemoryStatusEx(&systemMemory) != FALSE;
                DWORD processHandleCount = 0;
                GetProcessHandleCount(GetCurrentProcess(), &processHandleCount);
                DWORD processThreadCount = 0;
                HANDLE threadSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (threadSnapshot != INVALID_HANDLE_VALUE) {
                    THREADENTRY32 threadEntry{};
                    threadEntry.dwSize = sizeof(threadEntry);
                    if (Thread32First(threadSnapshot, &threadEntry)) {
                        do {
                            if (threadEntry.th32OwnerProcessID == GetCurrentProcessId())
                                ++processThreadCount;
                        } while (Thread32Next(threadSnapshot, &threadEntry));
                    }
                    CloseHandle(threadSnapshot);
                }
                if (GetProcessMemoryInfo(GetCurrentProcess(),
                        reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory))) {
                    writePsvmsLog(QStringLiteral(
                        "Мониторинг ресурсов: CPU процесса=%1% (на одно логическое ядро), CPU системы=%2%, "
                        "RAM процесса=%3 MB, пик RAM=%4 MB, private=%5 MB, virtual=%6 MB, "
                        "RAM системы занято=%7%, доступно=%8/%9 MB, threads=%10, handles=%11")
                        .arg(processCpuPercent < 0.0 ? QStringLiteral("n/a") : QString::number(processCpuPercent, 'f', 1))
                        .arg(systemCpuPercent < 0.0 ? QStringLiteral("n/a") : QString::number(systemCpuPercent, 'f', 1))
                        .arg(double(memory.WorkingSetSize) / (1024.0 * 1024.0), 0, 'f', 1)
                        .arg(double(memory.PeakWorkingSetSize) / (1024.0 * 1024.0), 0, 'f', 1)
                        .arg(double(memory.PrivateUsage) / (1024.0 * 1024.0), 0, 'f', 1)
                        .arg(double(memory.PagefileUsage) / (1024.0 * 1024.0), 0, 'f', 1)
                        .arg(hasSystemMemory ? QString::number(systemMemory.dwMemoryLoad) : QStringLiteral("n/a"))
                        .arg(hasSystemMemory ? QString::number(double(systemMemory.ullAvailPhys) / (1024.0 * 1024.0), 'f', 0) : QStringLiteral("n/a"))
                        .arg(hasSystemMemory ? QString::number(double(systemMemory.ullTotalPhys) / (1024.0 * 1024.0), 'f', 0) : QStringLiteral("n/a"))
                        .arg(processThreadCount)
                        .arg(processHandleCount));
                }
#else
                writePsvmsLog(QStringLiteral("Диагностика ресурсов: heartbeat UI обновлён; PID=%1")
                    .arg(QCoreApplication::applicationPid()));
#endif
            });
            diagnosticsTimer.start();
            std::thread uiWatchdog([] {
                bool reportedHang = false;
                qint64 hangStartedAt = 0;
                while (!watchdogStop.load()) {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    if (watchdogStop.load()) break;
                    const qint64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    const qint64 stalledFor = now - uiHeartbeatMs.load();
                    if (stalledFor >= 10000 && !reportedHang) {
                        hangStartedAt = now - stalledFor;
                        writePsvmsLog(QStringLiteral("ВОЗМОЖНОЕ ЗАВИСАНИЕ UI: главный поток не отвечал %1 мс; PID=%2; heartbeat просрочен")
                            .arg(stalledFor).arg(QCoreApplication::applicationPid()));
                        reportedHang = true;
                    } else if (stalledFor < 4000 && reportedHang) {
                        const qint64 hangDuration = qMax<qint64>(0, now - hangStartedAt);
                        writePsvmsLog(QStringLiteral("UI ВОССТАНОВИЛ ОТЗЫВЧИВОСТЬ: длительность задержки около %1 мс")
                            .arg(hangDuration));
                        reportedHang = false;
                        hangStartedAt = 0;
                    }
                }
            });
            writePsvmsLog(QStringLiteral("Этап запуска: вход в app.exec()"));
            rc = app.exec();
            diagnosticsTimer.stop();
            watchdogStop.store(true);
            if (uiWatchdog.joinable()) uiWatchdog.join();
            writePsvmsLog(QStringLiteral("Цикл событий Qt завершён, код: %1").arg(rc));
        }
        if (I18n::takeRestartRequest())      // смена языка: окно создаётся заново
            continue;
        if (App::takeLogoutRequest()) {      // выход из учётной записи: снова спрашиваем вход
            Users::forgetRemembered();
            Users::setCurrent(QString());
            continue;
        }
        break;
    }
    } catch (const std::exception &e) {
        const QString details = QStringLiteral("Перехвачено исключение C++ при запуске/работе: %1")
            .arg(QString::fromLocal8Bit(e.what()));
        writePsvmsLog(details);
        QMessageBox::critical(nullptr, QStringLiteral("PSVms — ошибка"), details);
        return 2;
    } catch (...) {
        const QString details = QStringLiteral("Перехвачено неизвестное исключение C++ при запуске/работе");
        writePsvmsLog(details);
        QMessageBox::critical(nullptr, QStringLiteral("PSVms — ошибка"), details);
        return 3;
    }
    writePsvmsLog(QStringLiteral("Завершение PSVms, код возврата: %1").arg(rc));
#ifdef Q_OS_WIN
    if (psvmsInstanceMutex)
        CloseHandle(psvmsInstanceMutex);
#endif
    return rc;
}

