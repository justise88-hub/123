// PSVMS — все исходники программы в одном файле.
// Собирается вместе с CMakeLists.txt из этой же папки (нужны Qt 6.8+, FFmpeg и OpenSSL).
// Файл собран автоматически из проекта; секции помечены исходными именами файлов.

// ======================================================================
// ВСЁ В ОДНОМ ФАЙЛЕ: ниже, в комментариях, — инструкция и файлы сборки.
// Чтобы собрать программу, создайте рядом с этим файлом файлы CMakeLists.txt, build_windows.bat
// и .github/workflows/build-windows.yml и скопируйте в них текст из разделов ниже (без «//   »).
// ======================================================================
//
//   --- README.md ---
//   # PSVMS — видеонаблюдение для Windows (проект в одном файле)
//
//   Весь код программы лежит в одном файле `psvms.cpp`. Готового `PSVMS.exe` в архиве нет:
//   его собирает GitHub (бесплатно), компьютер с Visual Studio для этого не нужен.
//
//   ## Как получить PSVMS.exe
//
//   1. Зайдите на github.com, создайте аккаунт и новый репозиторий (New repository).
//   2. Распакуйте этот архив и перетащите мышью **все файлы и папку `.github`** на страницу репозитория
//      (ссылка «uploading an existing file»), затем нажмите **Commit changes**.
//      Если папка `.github` не загрузилась: Add file → Create new file, введите имя
//      `.github/workflows/build-windows.yml` и вставьте текст из раздела «Файл сборки» ниже.
//   3. Откройте вкладку **Actions**, выберите «Build PSVMS for Windows» и нажмите **Run workflow**.
//      Первая сборка идёт 20-40 минут (собирается FFmpeg).
//   4. Когда сборка закончится, откройте её страницу и внизу, в разделе **Artifacts**, скачайте `PSVMS-windows`.
//      Распакуйте архив правой кнопкой («Извлечь всё») и запустите `PSVMS.exe` из этой папки.
//
//   Если сборка завершилась красным крестиком, откройте шаг **Build**, скопируйте строки со словом `error`
//   и отправьте их разработчику: код ещё ни разу не компилировался.
//
//   ## Первый запуск
//
//   - При запуске и смене сеанса показывается выбор активной учётной записи; пароль запрашивается, если задан.
//     Пароли и новые пользователи добавляются в «Настройки → Пользователи».
//   - Камеры: «Устройства → Добавить» (вручную, Hikvision / HiWatch по IP, поиск в сети, ONVIF, iCSee).
//   - Язык (русский или английский) выбирается в боковом меню.
//
//   ## Если у вас есть Windows с Visual Studio 2022, Qt 6.5+ и vcpkg
//
//   Поправьте два пути в начале `build_windows.bat` и запустите его: готовая программа окажется в `build\Release`.
//
//   ## Файл сборки (`.github/workflows/build-windows.yml`)
//
//   ```yaml
//   name: Build PSVMS for Windows
//
//   # Запуск: вкладка Actions -> "Build PSVMS for Windows" -> Run workflow.
//   # Готовая программа появится внизу страницы запуска в разделе Artifacts (PSVMS-windows.zip).
//   on:
//     workflow_dispatch:
//     push:
//       branches: [main, master]
//
//   jobs:
//     build:
//       runs-on: windows-2022
//       steps:
//         - uses: actions/checkout@v4
//
//         - name: Install Qt
//           uses: jurplel/install-qt-action@v4
//           with:
//             version: '6.8.1'
//             arch: 'win64_msvc2022_64'
//             modules: 'qtmultimedia'
//             cache: true
//
//         # FFmpeg собирается из исходников: первый запуск занимает 20-40 минут
//         - name: Install FFmpeg and OpenSSL (vcpkg)
//           shell: pwsh
//           run: |
//             & "$env:VCPKG_INSTALLATION_ROOT\vcpkg.exe" install "ffmpeg[core,avcodec,avformat]:x64-windows" openssl:x64-windows
//
//         - name: Configure
//           shell: pwsh
//           run: |
//             cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
//               -DCMAKE_PREFIX_PATH="$env:QT_ROOT_DIR" `
//               -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT\scripts\buildsystems\vcpkg.cmake"
//
//         - name: Build
//           run: cmake --build build --config Release
//
//         - name: Collect Qt libraries next to the exe
//           shell: pwsh
//           run: |
//             & "$env:QT_ROOT_DIR\bin\windeployqt.exe" --release --no-translations build\Release\PSVMS.exe
//
//         - name: Upload
//           uses: actions/upload-artifact@v4
//           with:
//             name: PSVMS-windows
//             path: build/Release/
//   ```
//
//   --- CMakeLists.txt ---
//   cmake_minimum_required(VERSION 3.21)
//   project(PSVMS LANGUAGES CXX)
//
//   set(CMAKE_CXX_STANDARD 17)
//   set(CMAKE_CXX_STANDARD_REQUIRED ON)
//   set(CMAKE_AUTOMOC ON)
//
//   if(MSVC)
//       add_compile_options(/utf-8)   # русские строки в исходниках
//   endif()
//
//   find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia MultimediaWidgets Sql Network)
//
//   # ---- FFmpeg: сначала pkg-config (Linux/macOS), иначе поиск файлов (Windows + vcpkg) ----
//   find_package(PkgConfig QUIET)
//   if(PkgConfig_FOUND)
//       pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil)
//   endif()
//
//   if(FFMPEG_FOUND)
//       set(FFMPEG_TARGET PkgConfig::FFMPEG)
//   else()
//       add_library(ffmpeg_deps INTERFACE)
//       foreach(lib avformat avcodec avutil)
//           find_path(FF_${lib}_INC lib${lib}/${lib}.h)
//           find_library(FF_${lib}_LIB ${lib})
//           if(NOT FF_${lib}_INC OR NOT FF_${lib}_LIB)
//               message(FATAL_ERROR "FFmpeg (${lib}) не найден. Установите FFmpeg (dev-пакеты) "
//                                   "или передайте -DCMAKE_PREFIX_PATH / vcpkg toolchain.")
//           endif()
//           target_include_directories(ffmpeg_deps INTERFACE ${FF_${lib}_INC})
//           target_link_libraries(ffmpeg_deps INTERFACE ${FF_${lib}_LIB})
//       endforeach()
//       set(FFMPEG_TARGET ffmpeg_deps)
//   endif()
//
//   # ---- Всё приложение — один файл исходников ----
//   qt_add_executable(PSVMS WIN32 psvms.cpp)
//   find_package(OpenSSL REQUIRED)
//   target_link_libraries(PSVMS PRIVATE
//       Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Sql Qt6::Network
//       OpenSSL::Crypto ${FFMPEG_TARGET})
//   if(WIN32)
//       target_link_libraries(PSVMS PRIVATE dwmapi)   # тёмная рамка окон в Windows 10/11
//   endif()
//
//   --- build_windows.bat ---
//   @echo off
//   REM ============================================================
//   REM  Build PSVMS.exe on Windows
//   REM  Requirements:
//   REM    1) Visual Studio 2022 with "Desktop development with C++"
//   REM    2) Qt 6.5+ for MSVC 2022 64-bit (Qt Multimedia + Qt SQL)
//   REM    3) vcpkg (https://github.com/microsoft/vcpkg)
//   REM  Edit the two paths below, then run this file.
//   REM ============================================================
//   set QT_DIR=C:\Qt\6.8.1\msvc2022_64
//   set VCPKG_ROOT=C:\vcpkg
//
//   if not exist "%QT_DIR%\bin\windeployqt.exe" (
//       echo Qt not found in %QT_DIR%
//       exit /b 1
//   )
//   if not exist "%VCPKG_ROOT%\vcpkg.exe" (
//       echo vcpkg not found in %VCPKG_ROOT%
//       exit /b 1
//   )
//
//   echo [1/4] Installing FFmpeg via vcpkg...
//   "%VCPKG_ROOT%\vcpkg.exe" install "ffmpeg[core,avcodec,avformat]:x64-windows" openssl:x64-windows || exit /b 1
//
//   echo [2/4] Configuring...
//   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
//     -DCMAKE_PREFIX_PATH="%QT_DIR%" ^
//     -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" || exit /b 1
//
//   echo [3/4] Building...
//   cmake --build build --config Release || exit /b 1
//
//   echo [4/4] Collecting Qt libraries next to the exe...
//   "%QT_DIR%\bin\windeployqt.exe" --release --no-translations build\Release\PSVMS.exe || exit /b 1
//
//   echo.
//   echo Done. Ready-to-run folder: build\Release   (PSVMS.exe)
//
//   --- .github/workflows/build-windows.yml ---
//   name: Build PSVMS for Windows
//
//   # Запуск: вкладка Actions -> "Build PSVMS for Windows" -> Run workflow.
//   # Готовая программа появится внизу страницы запуска в разделе Artifacts (PSVMS-windows.zip).
//   on:
//     workflow_dispatch:
//     push:
//       branches: [main, master]
//
//   jobs:
//     build:
//       runs-on: windows-2022
//       steps:
//         - uses: actions/checkout@v4
//
//         - name: Install Qt
//           uses: jurplel/install-qt-action@v4
//           with:
//             version: '6.8.1'
//             arch: 'win64_msvc2022_64'
//             modules: 'qtmultimedia'
//             cache: true
//
//         # FFmpeg собирается из исходников: первый запуск занимает 20-40 минут
//         - name: Install FFmpeg and OpenSSL (vcpkg)
//           shell: pwsh
//           run: |
//             & "$env:VCPKG_INSTALLATION_ROOT\vcpkg.exe" install "ffmpeg[core,avcodec,avformat]:x64-windows" openssl:x64-windows
//
//         - name: Configure
//           shell: pwsh
//           run: |
//             cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
//               -DCMAKE_PREFIX_PATH="$env:QT_ROOT_DIR" `
//               -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT\scripts\buildsystems\vcpkg.cmake"
//
//         - name: Build
//           run: cmake --build build --config Release
//
//         - name: Collect Qt libraries next to the exe
//           shell: pwsh
//           run: |
//             & "$env:QT_ROOT_DIR\bin\windeployqt.exe" --release --no-translations build\Release\PSVMS.exe
//
//         - name: Upload
//           uses: actions/upload-artifact@v4
//           with:
//             name: PSVMS-windows
//             path: build/Release/
//
// ======================================================================
// КОНЕЦ ВСТРОЕННЫХ ФАЙЛОВ. Дальше — код программы.
// ======================================================================

#include <QString>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QDateTime>
#include <QFile>
#include <QClipboard>
#include <openssl/evp.h>
#include <QThread>
#include <atomic>
#include <chrono>
#include <thread>
#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QDialog>
#include <QCoreApplication>
#include <QMimeData>
#include <QTreeWidget>
#include <QWidget>
#include <QUrl>
#include <QSet>
#include <QPointer>
#include <QSharedPointer>
#include <QMainWindow>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QAbstractButton>
#include <QPushButton>
#include <QFrame>
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QRectF>
#include <QtGlobal>
#include <QFormLayout>
#include <QLabel>
#include <QMediaDevices>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QDir>
#include <QSettings>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <QTime>
#include <QTimeEdit>
#include <QResizeEvent>
#include <QUuid>
#include <memory>
#include <QBrush>
#include <QColor>
#include <QPalette>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QRegularExpression>
#include <QTableWidget>
#include <QToolButton>
#include <QDateTime>
#include <QAudioOutput>
#include <QKeyEvent>
#include <QMediaPlayer>
#include <QScreen>
#include <QHostAddress>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QGroupBox>
#include <QActionGroup>
#include <QEasingCurve>
#include <QGridLayout>
#include <QIcon>
#include <QMap>
#include <QPainter>
#include <QPolygonF>
#include <QPaintEvent>
#include <QPropertyAnimation>
#include <QSplitter>
#include <QTreeWidgetItem>
#include <QCloseEvent>
#include <QCursor>
#include <QKeySequence>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QCalendarWidget>
#include <QFileInfo>
#include <QListWidget>
#include <QVideoWidget>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTextEdit>
#include <QUrl>
#include <QStandardPaths>
#include <QLockFile>
#include <QDesktopServices>
#include <QStorageInfo>
#include <QFileDialog>
#include <QSaveFile>
#include <QProcess>
#include <QTabWidget>
#include <QTabBar>
#include <functional>
#include <QButtonGroup>
#include <QEvent>
#include <QStyle>
#include <QApplication>
#include <QFontDatabase>
#include <QEventLoop>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QConicalGradient>
#include <QPainterPath>
#include <QSplashScreen>
#include <QCamera>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QGraphicsOpacityEffect>
#include <QGraphicsDropShadowEffect>
#include <QMediaCaptureSession>
#include <QMouseEvent>
#include <QStackedLayout>
#include <QAction>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWheelEvent>
#include <cmath>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>
#include <QScopeGuard>
#include <cerrno>
#include <cstdint>
#include <vector>
#include <QAuthenticator>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUdpSocket>
#include <QXmlStreamReader>
#include <QLocale>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <algorithm>
#include <QSqlDatabase>
#include <QSqlError>
static void writePsvmsLog(const QString &message);
#include <QMutex>
#include <QMutexLocker>
#include <QDateTime>
#include <QDir>
#include <QThread>
#include <exception>
#include <csignal>
#include <cstdio>
#include <string>
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#  include <tlhelp32.h>
#  include <dbghelp.h>
#  include <wincrypt.h>
#  pragma comment(lib, "Crypt32.lib")
#endif
#include <QSqlQuery>
#include <QByteArray>
#include <windows.h>
#include <QPasswordDigestor>
#include <QElapsedTimer>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

// ======================================================================
// core/camera_worker.h
// ======================================================================
class RecordingIndex;

struct CameraConfig {
    QString id;                 // уникальный id камеры: попадает в пути файлов и в БД
    QString name;               // отображаемое имя
    QString url;                // rtsp://login:password@host:554/...
    QString storageRoot;        // корневая папка архива
    QString dbPath;             // путь к SQLite-файлу с индексом
    int segmentSeconds = 300;   // длительность одного сегмента
    int ioTimeoutMs = 10000;    // таймаут открытия и чтения потока
    qint64 minimumFreeBytes = 1024LL * 1024LL * 1024LL; // защитный порог свободного места
};

// Один поток на камеру: читает пакеты через libavformat и пишет их БЕЗ перекодирования
// в mkv-сегменты фиксированной длительности. Сегмент всегда начинается с ключевого кадра.
// Метаданные сегментов попадают в SQLite (RecordingIndex).
// При обрыве потока переподключается с нарастающей паузой (1 с ... 30 с).
class CameraWorker : public QThread {
    Q_OBJECT
public:
    enum class State { Stopped, Connecting, Recording, Reconnecting };
    Q_ENUM(State)

    explicit CameraWorker(CameraConfig cfg, QObject *parent = nullptr);
    ~CameraWorker() override;

    const CameraConfig &config() const { return m_cfg; }

    void startRecording();   // вместо start(): сбрасывает флаг остановки
    void stop();             // не блокирует; после него вызвать wait()

signals:
    void stateChanged(const QString &cameraId, CameraWorker::State state);
    void segmentStarted(const QString &cameraId, const QString &path, qint64 startMs);
    void segmentFinished(const QString &cameraId, const QString &path,
                         qint64 startMs, qint64 endMs, qint64 sizeBytes);
    void errorOccurred(const QString &cameraId, const QString &message);

protected:
    void run() override;

private:
    void recordSession(RecordingIndex &index, bool &gotData);
    QString segmentPath(qint64 startMs) const;
    void armDeadline(int ms);
    void sleepInterruptible(int ms);
    static int interruptCallback(void *opaque);

    CameraConfig m_cfg;
    std::atomic<bool> m_stop{false};
    std::atomic<qint64> m_deadlineMs{0};
};

// ======================================================================
// core/i18n.h
// ======================================================================
// Язык интерфейса: русский или английский.
// Исходные тексты в коде написаны по-русски: T("Текст") возвращает его же (русский язык)
// или английский перевод из словаря (core/i18n.cpp). Если перевода нет — остаётся русский текст.
namespace I18n {

enum class Lang { Ru, En };

void init();                               // язык из настроек, иначе по языку системы
Lang language();
QString code();                            // "ru" или "en"
void setLanguage(Lang lang);               // запоминает выбор и язык календарей/дат
void setLanguageByCode(const QString &code);

// Смена языка пересоздаёт окно программы (запись камер при этом не прерывается)
void requestRestart();
bool takeRestartRequest();

QString translate(const char *ru);

} // namespace I18n

inline QString T(const char *ru)
{
    return I18n::translate(ru);
}

// ======================================================================
// app/camera_manager.h
// ======================================================================
class QTimer;

struct CameraEntry {
    QString id;
    QString name;
    QString group;
    QString url;               // основной поток: по нему идёт запись в архив
    QString liveUrl;           // подпоток для мозаики; может быть пустым
    QString webUrl;            // HTTP/HTTPS интерфейс камеры или регистратора
    int bitrateKbps = 4096;    // расчётный суммарный битрейт потока, кбит/с
    QString codec = QStringLiteral("H.264"); // справочная настройка для оценки архива
    bool record = false;       // локальная запись выключена по умолчанию; архив берётся с регистратора
    bool scheduleEnabled = false; // индивидуальное расписание; иначе используется общее
    QTime scheduleFrom = QTime(0, 0);
    QTime scheduleTo = QTime(23, 59);
    quint8 scheduleDays = 0x7f; // биты 0..6: Пн..Вс
    int viewQuality = 0;       // качество просмотра: 0 — авто, 1 — основной поток, 2 — подпоток
    int brightness = 0;        // яркость картинки, от -50 до 50
    bool showTime = true;      // показывать дату и время на камере
    bool audio = false;        // включать звук при показе
    bool fillCell = false;   // заполнять ячейку целиком с обрезкой лишних краёв

    // PTZ по ONVIF (заполняется автоматически при импорте по ONVIF, если камера поворотная)
    QString ptzXAddr;
    QString ptzToken;
    QString ptzUser;
    QString ptzPassword;
    bool hasPtz() const { return !ptzXAddr.isEmpty() && !ptzToken.isEmpty(); }
};

inline QString stateText(CameraWorker::State s)
{
    switch (s) {
    case CameraWorker::State::Recording:    return T("Запись идёт");
    case CameraWorker::State::Connecting:   return T("Подключение…");
    case CameraWorker::State::Reconnecting: return T("Нет связи, переподключение…");
    case CameraWorker::State::Stopped:      break;
    }
    return T("Остановлено");
}

// Список камер (в QSettings), их рекордеры и проверка доступности.
// Рекордеры запускаются по очереди (по одному каждые ~120 мс), чтобы сотни камер не подключались
// одновременно. Если в настройках отключён автозапуск, при старте программы ничего не запускается.
class CameraManager : public QObject {
    Q_OBJECT
public:
    explicit CameraManager(QObject *parent = nullptr);
    ~CameraManager() override;

    QString storageRoot() const { return m_storageRoot; }
    QString dbPath() const { return m_storageRoot + QStringLiteral("/index.sqlite"); }

    const QList<CameraEntry> &cameras() const { return m_cameras; }
    const CameraEntry *find(const QString &id) const;
    CameraWorker::State stateOf(const QString &id) const;
    bool isStarted(const QString &id) const;       // рекордер запущен или стоит в очереди
    bool isOnline(const QString &id) const;        // камера отвечает по сети
    int onlineCount() const;
    int offlineCount() const;
    int recordingCount() const;

    QString add(CameraEntry e);                    // возвращает сгенерированный id
    void addMany(QList<CameraEntry> list);
    void update(const CameraEntry &e);
    void setLiveUrl(const QString &id, const QString &url);
    void remove(const QString &id);

    void startCamera(const QString &id);
    void stopCamera(const QString &id);
    void startAll();
    void stopAll();
    void setStorageRoot(const QString &path);      // новая папка записей; запущенные рекордеры перезапускаются
    void probeNow();                               // проверить, какие камеры в сети
    void shutdown();
    void persist() const { save(); }
    void enforceDemoLimit();
    void applyRecordingSchedule();

signals:
    void camerasChanged();
    void cameraStateChanged(const QString &id, CameraWorker::State state);
    void cameraError(const QString &id, const QString &message);
    void onlineChanged();

private:
    void load();
    void save() const;
    void syncWorker(const CameraEntry &e);
    void stopWorker(const QString &id);
    void startNext();
    void pumpProbe();

    QList<CameraEntry> m_cameras;
    QHash<QString, CameraWorker *> m_workers;
    QHash<QString, CameraWorker::State> m_states;
    QStringList m_startQueue;
    QTimer *m_startTimer = nullptr;
    QString m_storageRoot;

    QTimer *m_probeTimer = nullptr;
    QTimer *m_scheduleTimer = nullptr;
    QStringList m_probeQueue;
    QHash<QString, bool> m_probeResults;
    QHash<QString, bool> m_online;
    int m_probeActive = 0;
    bool m_probing = false;
};

// ======================================================================
// app/analog_camera_dialog.h
// ======================================================================
class QComboBox;
class QLineEdit;

// Аналоговая камера, подключённая через карту видеозахвата (USB- или PCI-капкарта): Windows видит
// её как обычное видеоустройство. Список устройств берётся из Qt (QMediaDevices::videoInputs()).
// Ограничение: запись в архив для таких камер не ведётся, доступен только живой просмотр —
// у программы нет RTSP-потока с капкарты, а запись устроена через захват именно RTSP (core/camera_worker).
class AnalogCameraDialog : public QDialog {
public:
    explicit AnalogCameraDialog(QWidget *parent = nullptr);

    CameraEntry entry() const;
    void accept() override;

private:
    QLineEdit *m_name = nullptr;
    QLineEdit *m_group = nullptr;
    QComboBox *m_device = nullptr;
};

// ======================================================================
// app/app_info.h
// ======================================================================
namespace App {

inline constexpr const char *kVersion = "1.0";
inline constexpr const char *kAuthor = "P.Sveshnikov";

inline QString title()
{
    return QString::fromUtf8("PSVMS");
}

// Выход из учётной записи: окно закрывается, затем снова спрашивается вход
inline bool &logoutFlag()
{
    static bool f = false;
    return f;
}
inline void requestLogout()
{
    logoutFlag() = true;
    QCoreApplication::quit();
}
inline bool takeLogoutRequest()
{
    const bool r = logoutFlag();
    logoutFlag() = false;
    return r;
}

} // namespace App

// ======================================================================
// app/camera_dialog.h
// ======================================================================
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSlider;

// Настройки камеры: название, группа, потоки записи и просмотра, качество, яркость, время, звук.
// Используется и при добавлении вручную, и при правом щелчке по камере.
class CameraDialog : public QDialog {
public:
    CameraDialog(const CameraEntry &base, const QStringList &groups, QWidget *parent = nullptr);

    CameraEntry entry() const;
    void accept() override;

private:
    QWidget *withPicker(QLineEdit *edit, QComboBox *combo);
    void rebuildPickers();

    CameraEntry m_base;
    QLineEdit *m_name = nullptr;
    QComboBox *m_group = nullptr;
    QLineEdit *m_url = nullptr;
    QLineEdit *m_liveUrl = nullptr;
    QLineEdit *m_webUrl = nullptr;
    QSpinBox *m_bitrate = nullptr;
    QComboBox *m_codec = nullptr;
    QComboBox *m_urlPick = nullptr;
    QComboBox *m_livePick = nullptr;
    QComboBox *m_quality = nullptr;
    QSlider *m_brightness = nullptr;
    QLabel *m_brightLabel = nullptr;
    QCheckBox *m_record = nullptr;
    QCheckBox *m_cameraScheduleEnabled = nullptr;
    QTimeEdit *m_cameraScheduleFrom = nullptr;
    QTimeEdit *m_cameraScheduleTo = nullptr;
    QList<QCheckBox *> m_cameraScheduleDays;
    QCheckBox *m_showTime = nullptr;
    QCheckBox *m_audio = nullptr;
};

// ======================================================================
// app/common.h
// ======================================================================
// MIME-типы перетаскивания
inline constexpr const char *kCameraMime = "application/x-psvms-camera";   // камера из списка в ячейку
inline constexpr const char *kCellMime = "application/x-psvms-cell";       // ячейка на другую ячейку (обмен местами)
inline constexpr const char *kGroupMime = "application/x-psvms-camera-group"; // группа камер

// ======================================================================
// app/camera_tree.h
// ======================================================================
// Дерево «группа → камера». Элемент камеры хранит id в Qt::UserRole
// и при перетаскивании отдаёт его в MIME kCameraMime.
class CameraTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override
    {
        QStringList ids;
        auto collect = [&ids](const QTreeWidgetItem *root, auto &&collectRef) -> void {
            const QString id = root->data(0, Qt::UserRole).toString();
            if (!id.isEmpty() && !ids.contains(id)) ids.append(id);
            for (int i = 0; i < root->childCount(); ++i)
                collectRef(root->child(i), collectRef);
        };
        for (const QTreeWidgetItem *it : items) collect(it, collect);
        if (ids.isEmpty()) return nullptr;
        auto *m = new QMimeData;
        if (ids.size() == 1)
            m->setData(QString::fromLatin1(kCameraMime), ids.first().toUtf8());
        else
            m->setData(QString::fromLatin1(kGroupMime), ids.join(QLatin1Char('\n')).toUtf8());
        return m;
    }
};

// ======================================================================
// app/cloud_dialog.h
// ======================================================================
class QComboBox;
class QLabel;
class QLineEdit;

// Облако Hik-Connect (кнопка справа сверху). Вход сохраняет только название аккаунта;
// пароль на диске не хранится.
class CloudDialog : public QDialog {
public:
    explicit CloudDialog(QWidget *parent = nullptr);

private:
    QComboBox *m_region = nullptr;
    QLineEdit *m_account = nullptr;
    QLineEdit *m_password = nullptr;
    QLabel *m_status = nullptr;
};

// ======================================================================
// app/credentials_dialog.h
// ======================================================================
class QWidget;

// Запрашивает логин и пароль у устройства, которое отказало во входе. false — пользователь пропустил.
bool askCredentials(QWidget *parent, const QString &address, QString *user, QString *password);

// ======================================================================
// app/devices_page.h
// ======================================================================
class CameraManager;
class QPushButton;
class QTableWidget;
class QToolButton;

// Устройства: список камер с состоянием связи и записи, добавление разными способами,
// запуск и остановка записи.
class DevicesPage : public QWidget {
    Q_OBJECT
public:
    explicit DevicesPage(CameraManager *mgr, QWidget *parent = nullptr);

public slots:

private slots:
    void rebuild();
    void refreshStates();
    void editCamera();
    void removeCamera();
    void removeGroup();
    void startSelected();
    void stopSelected();
    void updateButtons();

private:
    QString currentId() const;
    QStringList selectedIds() const;

    CameraManager *m_mgr;
    QTableWidget *m_table = nullptr;
    QToolButton *m_addBtn = nullptr;
    QPushButton *m_edit = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_removeAll = nullptr;
};

// ======================================================================
// app/events_page.h
// ======================================================================
class QTableWidget;

// Журнал событий: потеря связи, ошибки записи, смена состояния камер.
// Позже сюда же попадут события детектора движения.
class EventsPage : public QWidget {
    Q_OBJECT
public:
    explicit EventsPage(QWidget *parent = nullptr);

    void addEvent(const QString &camera, const QString &text);

private:
    void acknowledgeRow(int row);
    void saveHistoryFromTable();
    QTableWidget *m_table = nullptr;
    QTimer *m_persistTimer = nullptr;
    QStringList m_history;
    bool m_historyDirty = false;
};

// ======================================================================
// app/fullscreen_view.h
// ======================================================================
class QLabel;
class QMediaPlayer;
class QAudioOutput;
class QTimer;
class QToolButton;
class VideoSurface;
class VideoCell;

// Камера на весь экран (двойной щелчок по ячейке). Всегда основной поток.
// Выход: Esc, двойной щелчок или кнопка ✕. Колесо мыши увеличивает картинку.
class FullscreenView : public QWidget {
public:
    explicit FullscreenView(VideoCell *cell, QWidget *parent = nullptr);
    void openOnScreenOf(QWidget *w);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

private:
    VideoCell *m_cell = nullptr;
    VideoSurface *m_surface = nullptr;
    QLabel *m_hint = nullptr;
    QLabel *m_logo = nullptr; // фирменный знак в пустой ячейке
    QToolButton *m_close = nullptr;
};

// ======================================================================
// core/device_adder.h
// ======================================================================
struct AddedChannel {
    int number = 1;
    QString name;
    QString mainUrl;    // основной поток (для записи)
    QString subUrl;     // подпоток (для мозаики), может быть пустым
};

struct AddRequest {
    QString address;
    quint16 httpPort = 80;
    quint16 rtspPort = 554;
    QString user;
    QString password;
    int channels = 0;   // сколько каналов у регистратора, если определить автоматически не удастся
};

struct AddResult {
    bool ok = false;
    bool authFailed = false;     // нужен (другой) логин или пароль
    QString error;
    QString model;
    QString name;
    QString type;                // IPCamera, NVR, DVR ... (если известен)
    bool recorder = false;
    QString method;              // "ISAPI" или "RTSP"
    QList<AddedChannel> channels;
};

// Добавление камер и регистраторов Hikvision / HiWatch / Polyvision по IP.
// 1) Новые модели: ISAPI (модель, тип, каналы, номера потоков).
// 2) Старые модели и другие бренды: подбор рабочего RTSP-пути (core/rtsp_probe).
class DeviceAdder : public QObject {
    Q_OBJECT
public:
    explicit DeviceAdder(QObject *parent = nullptr);
    void start(const QList<AddRequest> &requests, int concurrency = 6);
    void cancel();

signals:
    void resolved(int index, const AddResult &result);
    void progress(int done, int total);
    void finished();

private:
    void pump();
    void runOne(int idx);
    void probeRtsp(int idx, const AddRequest &rq, const QString &prevError, int gen);
    void finishOne(int idx, const AddResult &r, int gen);

    QList<AddRequest> m_reqs;
    int m_next = 0;
    int m_done = 0;
    int m_active = 0;
    int m_conc = 6;
    int m_gen = 0;
    bool m_running = false;
};

// ======================================================================
// app/hik_ip_dialog.h
// ======================================================================
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

// Добавление камеры или регистратора Hikvision / HiWatch по IP-адресу (новые и старые модели).
// Если устройство просит пароль, программа его запрашивает.
class HikIpDialog : public QDialog {
public:
    explicit HikIpDialog(CameraManager *mgr, QWidget *parent = nullptr);

private:
    void start(const QString &user, const QString &password);
    void onResolved(const AddResult &res);

    CameraManager *m_mgr;
    DeviceAdder *m_adder = nullptr;
    QLineEdit *m_ip = nullptr;
    QSpinBox *m_httpPort = nullptr;
    QSpinBox *m_rtspPort = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_pass = nullptr;
    QSpinBox *m_channels = nullptr;
    QLineEdit *m_group = nullptr;
    QLineEdit *m_name = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_btn = nullptr;
    QString m_address;
    QString m_curUser;
};

// ======================================================================
// core/portscan.h
// ======================================================================
// Быстрая проверка, у каких адресов диапазона открыт TCP-порт (например, RTSP 554).
class PortScanner : public QObject {
    Q_OBJECT
public:
    explicit PortScanner(QObject *parent = nullptr);
    void start(quint32 firstIp, quint32 lastIp, quint16 port, int concurrency = 64, int timeoutMs = 800);
    void stop();

signals:
    void found(const QString &ip); // совместимость со старым импортёром iCSee
    void foundPort(const QString &ip, quint16 port);
    void progress(int done, int total);
    void finished();

private:
    void pump();

    qint64 m_next = 0;
    qint64 m_last = -1;
    quint16 m_port = 554;
    int m_conc = 64;
    int m_timeout = 800;
    int m_active = 0;
    int m_done = 0;
    int m_total = 0;
    int m_gen = 0;
    bool m_running = false;
};

// ======================================================================
// app/icsee_import_dialog.h
// ======================================================================
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;

// Добавление камер iCSee (Xiongmai): сканируем диапазон IP на открытый RTSP-порт,
// отмечаем найденные и добавляем. RTSP-адреса строятся по шаблону iCSee:
// основной поток (stream=0) для записи и подпоток (stream=1) для мозаики.
// Для одной камеры укажите одинаковые адреса «от» и «до».
class IcseeImportDialog : public QDialog {
    Q_OBJECT
public:
    explicit IcseeImportDialog(CameraManager *mgr, QWidget *parent = nullptr);

private slots:
    void startScan();
    void addHost(const QString &ip);
    void addSelected();
    void setAllChecked(bool on);

private:
    CameraManager *m_mgr;
    PortScanner *m_scanner = nullptr;

    QLineEdit *m_from = nullptr;
    QLineEdit *m_to = nullptr;
    QSpinBox *m_port = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_pass = nullptr;
    QLineEdit *m_group = nullptr;
    QTableWidget *m_table = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_btnScan = nullptr;
    QPushButton *m_btnAdd = nullptr;

    QStringList m_hosts;          // строка таблицы i == m_hosts[i]
    QSet<QString> m_existing;     // уже добавленные камеры (по IP)
};

// ======================================================================
// core/hikvision.h
// ======================================================================
class QNetworkAccessManager;
class QTimer;
class QUdpSocket;

// Hikvision и её бренд HiWatch используют одни и те же протоколы:
//   RTSP:  rtsp://логин:пароль@IP:554/Streaming/Channels/<канал*100 + поток>
//          поток 1 — основной, 2 — дополнительный (канал 1: 101 и 102, канал 2: 201 и 202, ...)
//   ISAPI: http://IP/ISAPI/System/deviceInfo и /ISAPI/Streaming/channels (HTTP Digest)
inline QString hikRtspUrl(const QString &host, quint16 port, const QString &user, const QString &pass,
                          int channel, int stream)
{
    QUrl u;
    u.setScheme(QStringLiteral("rtsp"));
    u.setHost(host);
    u.setPort(port);
    u.setUserName(user);
    u.setPassword(pass);
    u.setPath(QStringLiteral("/Streaming/Channels/%1").arg(channel * 100 + stream));
    return u.toString(QUrl::FullyEncoded);
}

struct HikChannel {
    int number = 1;
    QString name;
};

struct HikDevice {
    QString address;
    quint16 httpPort = 80;
    QString model;
    QString name;
    QString serial;
    QString type;           // IPCamera, NVR, DVR ...
    bool activated = true;
    QList<HikChannel> channels;

    bool isRecorder() const
    {
        return type.contains(QLatin1String("NVR"), Qt::CaseInsensitive)
               || type.contains(QLatin1String("DVR"), Qt::CaseInsensitive);
    }
};

struct HikRequest {
    QString address;
    quint16 httpPort = 80;
    QString user;
    QString password;
    int fallbackChannels = 0;   // сколько каналов у регистратора, если список получить не удалось
};

struct HikResult {
    bool ok = false;
    bool authFailed = false;     // нужен (другой) логин или пароль
    QString error;
    HikDevice device;
};

// Производитель по модели или названию: "Hikvision", "HiWatch", "Polyvision" или пустая строка
QString guessVendor(const QString &model, const QString &name = QString());

// Поиск устройств по протоколу SADP (multicast 239.255.255.250:37020) — как в программе SADP Tool.
// Работает в своей подсети. Ответ зависит от прошивки, поэтому для других подсетей
// используйте сканирование диапазона IP.
class HikDiscovery : public QObject {
    Q_OBJECT
public:
    explicit HikDiscovery(QObject *parent = nullptr);
    void start(int timeoutMs = 4000);
    void stop();

signals:
    void deviceFound(const HikDevice &dev);
    void finished();

private:
    void sendProbes();
    void onReadyRead();

    QUdpSocket *m_socket = nullptr;
    QTimer *m_timer = nullptr;
    QSet<QString> m_seen;
    bool m_running = false;
};

// Пакетный опрос по ISAPI: тип устройства, модель и список каналов (камера = 1 канал,
// регистратор = все его каналы). Одновременно опрашивается не более `concurrency` устройств.
class HikResolver : public QObject {
    Q_OBJECT
public:
    explicit HikResolver(QObject *parent = nullptr);
    void start(const QList<HikRequest> &requests, int concurrency = 8);
    void cancel();

signals:
    void resolved(int index, const HikResult &result);
    void progress(int done, int total);
    void finished();

private:
    void pump();
    void runJob(int idx);
    void jobDone(int idx, const HikResult &r, int gen);

    QNetworkAccessManager *m_nam = nullptr;
    QList<HikRequest> m_requests;
    int m_next = 0;
    int m_done = 0;
    int m_active = 0;
    int m_conc = 8;
    int m_gen = 0;
    bool m_running = false;
};

// ======================================================================
// core/onvif.h
// ======================================================================
class QNetworkAccessManager;
class QTimer;
class QUdpSocket;

// Найденное ONVIF-устройство.
struct OnvifDevice {
    QString address;    // IPv4 камеры
    QString xaddr;      // http://ip:port/onvif/device_service
    QString name;       // из scopes (если камера сообщила)
    QString hardware;   // модель из scopes
};

struct OnvifRequest {
    QString xaddr;
    QString user;
    QString password;
};

// Результат опроса камеры: адреса потоков уже с логином и паролем.
struct OnvifStreams {
    bool ok = false;
    QString error;
    QString mainUrl;   // профиль с максимальным разрешением (для записи)
    QString subUrl;    // профиль с минимальным разрешением (для мозаики), может быть пустым

    // Для PTZ (может отсутствовать, если у камеры нет поворотного механизма):
    QString ptzXAddr;       // адрес PTZ-сервиса камеры
    QString ptzToken;       // токен профиля, которым управляем
    bool hasPtz = false;
};

// ---------------------------------------------------------------------------
// Поиск по WS-Discovery (multicast 239.255.255.250:3702).
// Работает только в той же подсети (L2), что и компьютер.
class OnvifDiscovery : public QObject {
    Q_OBJECT
public:
    explicit OnvifDiscovery(QObject *parent = nullptr);
    void start(int timeoutMs = 4000);
    void stop();

signals:
    void deviceFound(const OnvifDevice &dev);
    void finished();

private:
    void sendProbes();
    void onReadyRead();

    QUdpSocket *m_socket = nullptr;
    QTimer *m_timer = nullptr;
    QSet<QString> m_seen;
    bool m_running = false;
};

// ---------------------------------------------------------------------------
// Сканирование диапазона IP: на каждый адрес уходит GetSystemDateAndTime
// (разрешён без авторизации). Находит камеры в других подсетях и VLAN,
// куда multicast не доходит.
class OnvifScanner : public QObject {
    Q_OBJECT
public:
    explicit OnvifScanner(QObject *parent = nullptr);
    void start(quint32 firstIp, quint32 lastIp, quint16 port = 80, int concurrency = 64);
    void stop();

signals:
    void deviceFound(const OnvifDevice &dev);
    void progress(int done, int total);
    void finished();

private:
    void pump();

    QNetworkAccessManager *m_nam = nullptr;
    qint64 m_next = 0;
    qint64 m_last = -1;
    quint16 m_port = 80;
    int m_conc = 64;
    int m_active = 0;
    int m_done = 0;
    int m_total = 0;
    int m_gen = 0;
    bool m_running = false;
};

// ---------------------------------------------------------------------------
// Пакетное получение RTSP-адресов: для каждой камеры по цепочке
// GetSystemDateAndTime -> GetCapabilities -> GetProfiles -> GetStreamUri.
// Одновременно опрашивается не более `concurrency` камер.
class OnvifResolver : public QObject {
    Q_OBJECT
public:
    explicit OnvifResolver(QObject *parent = nullptr);
    void start(const QList<OnvifRequest> &requests, int concurrency = 8);
    void cancel();

signals:
    void resolved(int index, const OnvifStreams &streams);   // index — позиция в requests
    void progress(int done, int total);
    void finished();

private:
    void pump();
    void runJob(int idx);
    void jobDone(int idx, const OnvifStreams &s, int gen);

    QNetworkAccessManager *m_nam = nullptr;
    QList<OnvifRequest> m_requests;
    int m_next = 0;
    int m_done = 0;
    int m_active = 0;
    int m_conc = 8;
    int m_gen = 0;
    bool m_running = false;
};

// ======================================================================
// app/lan_search_dialog.h
// ======================================================================
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;

// Поиск камер и регистраторов разных производителей в локальной сети:
//   SADP (Hikvision/HiWatch) + ONVIF WS-Discovery (все бренды) + проверка типичных TCP-портов диапазона IP.
// Для найденных устройств подбираются адреса потоков; если нужен пароль, программа его спрашивает.
class LanSearchDialog : public QDialog {
    Q_OBJECT
public:
    explicit LanSearchDialog(CameraManager *mgr, QWidget *parent = nullptr);

private slots:
    void startSearch();
    void startScan();
    void importSelected();
    void applyFilter();
    void setAllChecked(bool on);

private:
    struct Candidate {
        QString address;
        quint16 httpPort = 80;
        bool httpPortConfirmed = false;
        QString model;
        QString vendor;
    };

    void addCandidate(const QString &ip, quint16 httpPort, const QString &model, const QString &name);
    void scanNextPort();
    void setState(int row, const QString &text);
    void setBusy(bool busy);
    void runRound();
    void onRoundFinished();
    void finishImport();
    void addEntries(int row, const AddResult &res);

    CameraManager *m_mgr;
    HikDiscovery *m_sadp = nullptr;
    OnvifDiscovery *m_onvif = nullptr;
    PortScanner *m_scan = nullptr;
    DeviceAdder *m_adder = nullptr;

    QComboBox *m_filter = nullptr;
    QLineEdit *m_from = nullptr;
    QLineEdit *m_to = nullptr;
    QSpinBox *m_rtsp = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_pass = nullptr;
    QLineEdit *m_group = nullptr;
    QSpinBox *m_channels = nullptr;
    QTableWidget *m_table = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_btnFind = nullptr;
    QPushButton *m_btnScan = nullptr;
    QPushButton *m_btnImport = nullptr;

    QList<Candidate> m_candidates;     // строка таблицы i == m_candidates[i]
    QSet<QString> m_existing;          // адреса, уже добавленные в программу
    int m_pendingSearches = 0;
    quint32 m_scanFirstIp = 0;
    quint32 m_scanLastIp = 0;
    QList<quint16> m_scanPorts;
    int m_scanPortIndex = 0;
    int m_scanDoneBase = 0;
    int m_scanTotal = 0;

    // состояние добавления (камеры опрашиваются «раундами»: после запроса пароля — новый раунд)
    QList<int> m_roundRows;
    QHash<int, QString> m_rowUser;
    QHash<int, QString> m_rowPass;
    QList<int> m_authRetry;
    QList<CameraEntry> m_entries;
    QList<int> m_addedRows;
    int m_failed = 0;
    int m_devicesOk = 0;
    QString m_commonUser;
    QString m_commonPass;
    QString m_groupName;
    quint16 m_rtspPort = 554;
    int m_fallbackChannels = 0;
};

// ======================================================================
// app/live_view_page.h
// ======================================================================
class CameraManager;
class CameraTree;
class QLineEdit;
class QPropertyAnimation;
class QToolButton;
class QTreeWidgetItem;
class VideoGrid;

// Мониторинг: слева список камер и PTZ, справа мозаика. Внизу панель: раскладка (до 64 ячеек),
// шаблоны, архив, запуск камер.
class LiveViewPage : public QWidget {
    Q_OBJECT
public:
    explicit LiveViewPage(CameraManager *mgr, QWidget *parent = nullptr);
    VideoGrid *grid() const { return m_grid; }
    void openArchiveForSelectedCamera();

signals:
    void addCameraRequested();
    void message(const QString &text);

private slots:
    void rebuildTree();
    void refreshStates();
    void applyFilter(const QString &text);
    void setPanelVisible(bool on);

private:
    void openArchive(const QString &cameraId);
    void rebuildTemplatesMenu();

    CameraManager *m_mgr;
    CameraTree *m_tree = nullptr;
    QLineEdit *m_search = nullptr;
    VideoGrid *m_grid = nullptr;
    QWidget *m_leftPanel = nullptr;
    QToolButton *m_layoutBtn = nullptr;
    QToolButton *m_tplBtn = nullptr;
    QPointer<QPropertyAnimation> m_panelAnim;
    QHash<QString, QTreeWidgetItem *> m_items;
};

// ======================================================================
// app/login_dialog.h
// ======================================================================
class QWidget;

// Вход в программу. При каждом запуске/переключении сеанса показывается выбор активной
// учётной записи; пароль проверяется, если он задан. Возвращает false при отмене входа.
bool authenticate(QWidget *parent = nullptr);

// Смена пароля текущего пользователя (пустой пароль — вход без пароля).
void changeOwnPassword(QWidget *parent);

// ======================================================================
// app/main_window.h
// ======================================================================
class CameraManager;
class DevicesPage;
class EventsPage;
class LiveViewPage;
class QLabel;
class QStackedWidget;
class SettingsPage;
class SideNav;
class TopBar;
class CameraStatusDialog;

// Проверка встроенного пароля разработчика. В исходнике хранится только PBKDF2-хэш,
// исходный пароль не записывается ни в настройки, ни в файлы программы.

// Окно PSVMS: слева постоянно раскрытое меню (Мониторинг, Устройства, События, Настройки, Язык),
// сверху панель со счётчиками камер и кнопкой облака Hik-Connect, внизу строка состояния.
namespace {
QIcon makeAppIcon();
QIcon makeCellIcon(const QString &kind, const QColor &color = QColor(255, 224, 104));
void openYandexAssistant(QWidget *parent);
QWidget *buildAiHomePanel(QWidget *parent);
}

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(CameraManager *mgr, QWidget *parent = nullptr);
    bool startFullscreen() const;

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void resetInactivityTimer();
    void showLanguageMenu();
    void toggleMainFullscreen();
    void leaveMainFullscreen();
    void applyLanguage(const QString &code);
    void updateLanguageItem();
    QString pageTitle(int page) const;
    void toggleVoiceControl();
    void handleVoiceCommand(const QString &command);
    void stopVoiceControl();

    CameraManager *m_mgr;
    SideNav *m_nav = nullptr;
    TopBar *m_top = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_aiHomePanel = nullptr;
    LiveViewPage *m_live = nullptr;
    DevicesPage *m_devices = nullptr;
    EventsPage *m_events = nullptr;
    SettingsPage *m_settings = nullptr;
    QPointer<CameraStatusDialog> m_cameraStatusDialog;
    QTimer *m_inactivityTimer = nullptr;
    QLabel *m_clock = nullptr;
    QLabel *m_sysLabel = nullptr;
    QPushButton *m_voiceButton = nullptr;
    QProcess *m_voiceProcess = nullptr;
    QByteArray m_voiceOutputBuffer;
    int m_langItem = -1;
    int m_settingsItem = -1;
    int m_fullscreenItem = -1;
    QRect m_preFullscreenGeometry;
    bool m_preFullscreenMaximized = false;
    bool m_hasPreFullscreenState = false;
};

// ======================================================================
// app/onvif_import_dialog.h
// ======================================================================
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;

// Массовое добавление камер по ONVIF:
//   1. найти устройства (multicast в своей подсети или сканирование диапазона IP),
//   2. указать общий логин и пароль,
//   3. отметить нужные и нажать «Добавить»: программа сама получит RTSP-адреса
//      основного потока (для записи) и подпотока (для мозаики).
class OnvifImportDialog : public QDialog {
    Q_OBJECT
public:
    explicit OnvifImportDialog(CameraManager *mgr, QWidget *parent = nullptr);

private slots:
    void startDiscovery();
    void startScan();
    void addDevice(const OnvifDevice &dev);
    void importSelected();
    void setAllChecked(bool on);

private:
    QString displayName(int row) const;
    void setBusy(bool busy);

    CameraManager *m_mgr;
    OnvifDiscovery *m_discovery = nullptr;
    OnvifScanner *m_scanner = nullptr;
    OnvifResolver *m_resolver = nullptr;

    QLineEdit *m_from = nullptr;
    QLineEdit *m_to = nullptr;
    QSpinBox *m_port = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_pass = nullptr;
    QLineEdit *m_group = nullptr;
    QTableWidget *m_table = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_btnDiscover = nullptr;
    QPushButton *m_btnScan = nullptr;
    QPushButton *m_btnImport = nullptr;

    QList<OnvifDevice> m_devices;        // строка таблицы i == m_devices[i]
    QSet<QString> m_existingHosts;       // уже добавленные камеры (по IP)
    QList<int> m_importRows;             // строки, ушедшие в опрос (по порядку запросов)
    QList<CameraEntry> m_pending;        // успешно опрошенные камеры
    QList<int> m_pendingRows;
    int m_failed = 0;
};

// ======================================================================
// core/recording_index.h
// ======================================================================
struct SegmentInfo {
    qint64 id = -1;
    QString cameraId;
    QString path;
    qint64 startMs = 0;   // UTC, мс от эпохи
    qint64 endMs = 0;     // 0, если сегмент ещё пишется
    qint64 sizeBytes = 0;
};

// Индекс сегментов архива в SQLite.
//
// ВАЖНО: соединение QSqlDatabase нельзя передавать между потоками, поэтому
// каждый объект RecordingIndex нужно создавать и вызывать open() в том потоке,
// где он будет использоваться (в CameraWorker::run() и отдельно в GUI-потоке).
// Несколько соединений к одному файлу работают штатно (включён режим WAL).
class RecordingIndex {
public:
    RecordingIndex() = default;
    ~RecordingIndex();
    RecordingIndex(const RecordingIndex &) = delete;
    RecordingIndex &operator=(const RecordingIndex &) = delete;

    bool open(const QString &dbPath);   // открывает соединение и создаёт схему
    void close();
    QString lastError() const { return m_lastError; }

    // Запись
    qint64 beginSegment(const QString &cameraId, const QString &path, qint64 startMs);
    bool endSegment(qint64 id, qint64 endMs, qint64 sizeBytes);
    // После сбоя/краша остаются сегменты без end_ms: восстановить по файлу или удалить.
    void closeDanglingSegments(const QString &cameraId);

    // Чтение (для архива и таймлайна)
    QList<SegmentInfo> segmentsInRange(const QString &cameraId, qint64 fromMs, qint64 toMs);
    SegmentInfo segmentAt(const QString &cameraId, qint64 ms);   // id == -1, если записи нет

private:
    QString m_conn;
    QString m_lastError;
};

// ======================================================================
// app/playback_page.h
// ======================================================================
class CameraManager;
class QAudioOutput;
class QCalendarWidget;
class QComboBox;
class QLabel;
class QListWidget;
class QMediaPlayer;
class QPushButton;
class QSlider;
class QVideoWidget;

// Архив: выбор камеры и даты, список сегментов из индекса SQLite, воспроизведение.
class PlaybackPage : public QWidget {
    Q_OBJECT
public:
    explicit PlaybackPage(CameraManager *mgr, QWidget *parent = nullptr);
    void selectCamera(const QString &id);   // выбрать камеру в списке

protected:
    void showEvent(QShowEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void reloadCameras();
    void reloadSegments();
    void playCurrent();
    void togglePlay();

private:
    CameraManager *m_mgr;
    RecordingIndex m_index;   // соединение с БД живёт в GUI-потоке

    QComboBox *m_camera = nullptr;
    QCalendarWidget *m_calendar = nullptr;
    QListWidget *m_list = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_archiveCount = nullptr;
    QVideoWidget *m_video = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QPushButton *m_playBtn = nullptr;
    QSlider *m_slider = nullptr;
    QLabel *m_time = nullptr;
    QVBoxLayout *m_videoLayout = nullptr;
    QPointer<QDialog> m_videoFullscreen;
};

// ======================================================================
// app/playback_window.h
// ======================================================================
class CameraManager;

// Архив камеры в отдельном окне (из панели «Мониторинг» или меню камеры)
class PlaybackWindow : public QDialog {
public:
    PlaybackWindow(CameraManager *mgr, const QString &cameraId, QWidget *parent = nullptr);
};

// ======================================================================
// app/ptz_panel.h
// ======================================================================
class CameraManager;
class QNetworkAccessManager;
class QSlider;
class QToolButton;

// Панель управления поворотной камерой (PTZ) по ONVIF. Активна только для камеры, у которой PTZ
// определился при импорте по ONVIF (core/onvif.h, OnvifStreams::hasPtz); для остальных камер кнопки
// выключены. Направление — удерживая кнопку, камера движется; отпустили — останавливается.
class PtzPanel : public QWidget {
    Q_OBJECT
public:
    explicit PtzPanel(CameraManager *mgr, QWidget *parent = nullptr);

public slots:
    void setCamera(const QString &cameraId);   // пустая строка — камера не выбрана
    void refreshControls() { updateEnabled(); }

private:
    void updateEnabled();

    CameraManager *m_mgr;
    QNetworkAccessManager *m_nam;
    QString m_cameraId;
    QToolButton *m_dirs[9] = {};
    QToolButton *m_zoomIn = nullptr;
    QToolButton *m_zoomOut = nullptr;
    QSlider *m_speed = nullptr;
    QWidget *m_note = nullptr;
};

// ======================================================================
// app/rtsp_quick_dialog.h
// ======================================================================
class QCheckBox;
class QLineEdit;

// Простое добавление камеры: только название и адрес RTSP-потока, без мастеров и поиска.
class RtspQuickDialog : public QDialog {
public:
    explicit RtspQuickDialog(QWidget *parent = nullptr);

    CameraEntry entry() const;
    void accept() override;

private:
    QLineEdit *m_name = nullptr;
    QLineEdit *m_url = nullptr;
    QLineEdit *m_group = nullptr;
    QCheckBox *m_record = nullptr;
    QCheckBox *m_cameraScheduleEnabled = nullptr;
    QTimeEdit *m_cameraScheduleFrom = nullptr;
    QTimeEdit *m_cameraScheduleTo = nullptr;
    QList<QCheckBox *> m_cameraScheduleDays;
};

// ======================================================================
// app/settings.h
// ======================================================================
// Сообщает окнам, что настройки изменились (частота кадров, формат времени и т. д.)
class CfgNotifier : public QObject {
    Q_OBJECT
signals:
    void changed();
};

namespace Cfg {

// Настройки просмотра (кэшируются: читаются на каждый кадр)
struct View {
    bool lowPower = false;      // режим для слабого компьютера
    int maxFps = 30;            // целевой предел кадров отрисовки по умолчанию
    bool smooth = true;         // сглаживание картинки при масштабировании
    bool timestamp = true;      // дата и время на камерах
    QString timeFormat = QStringLiteral("dd.MM.yyyy HH:mm:ss");
    bool animations = true;     // плавные анимации интерфейса
    bool showInfo = true;       // разрешение и качество связи поверх картинки
    bool showFps = true;        // отдельный индикатор входящего FPS в ячейке камеры
};

const View &view();
void reload();                  // перечитать настройки и оповестить окна
CfgNotifier *notifier();

bool autostartCameras();        // запускать камеры при старте программы
QString videoDir();             // папка для записей
QString screenshotDir();        // папка для снимков экрана

} // namespace Cfg

// ======================================================================
// app/settings_page.h
// ======================================================================
class CameraManager;
class QLabel;
class QTableWidget;

// Настройки программы: общие, видео и папки, производительность, пользователи, резервная копия.
class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(CameraManager *mgr, QWidget *parent = nullptr);

signals:
    void languageChosen(const QString &code);   // "ru" или "en"
    void storageChanged();
    void proSessionActivated();
    void proSessionDeactivated();

private slots:
    void rebuildUsers();

private:
    QWidget *buildGeneralTab();
    QWidget *buildVideoTab();
    QWidget *buildPerformanceTab();
    QWidget *buildNetworkTab();
    QWidget *buildAiTab();
    QWidget *buildUsersTab();
    QWidget *buildBackupTab();
    QWidget *buildLicenseTab();
    QWidget *buildAboutTab();
    QString currentUserId() const;

    CameraManager *m_mgr;
    QTableWidget *m_users = nullptr;
    QLabel *m_licenseStatus = nullptr;
};

// ======================================================================
// app/side_nav.h
// ======================================================================
class QButtonGroup;
class QFrame;
class QLabel;
class QPropertyAnimation;
class QPushButton;
class QVBoxLayout;

// Боковое меню: иконка и название пункта, подсветка-«пилюля» скользит к выбранному пункту,
// меню постоянно отображается в полном виде. Пункты бывают двух видов: страницы (подсвечиваются)
// и действия (например, выбор языка), внизу — учётная запись пользователя.
class SideNav : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int navWidth READ navWidth WRITE setNavWidth)
public:
    explicit SideNav(QWidget *parent = nullptr);

    int addPage(const QString &icon, const QString &title);      // возвращает номер пункта
    int addAction(const QString &icon, const QString &title);
    int addBottomAction(const QString &icon, const QString &title);   // компактный пункт внизу (язык, выход)
    void restorePageOrder();
    void setItemText(int index, const QString &icon, const QString &title);
    void setItemVisible(int index, bool visible);
    QWidget *itemButton(int index) const;
    void setCurrent(int pageId, bool animate = true);
    int current() const { return (m_current >= 0 && m_current < m_items.size()) ? m_items.at(m_current).pageId : -1; }

    void setUser(const QString &name, const QString &role);
    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool on, bool animate = true);

    int navWidth() const { return width(); }
    void setNavWidth(int w);

signals:
    void currentChanged(int index);       // выбрана страница
    void actionTriggered(int index);      // нажат пункт-действие
    void changePasswordRequested();
    void logoutRequested();

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    QPixmap m_wallpaper;
    QPixmap m_wallpaperScaled;
    QSize m_wallpaperHostSize;
    struct Item {
        QString icon;
        QString title;
        QPushButton *button = nullptr;
        bool isPage = true;
        int pageId = -1; // неизменяемый ID страницы, не зависит от визуального порядка пунктов
    };

    int addItem(const QString &icon, const QString &title, bool isPage);
    void updatePill(bool animate);
    void applyTexts();
    void showUserMenu();
    void reorderItem(int from, int to);
    int itemAtGlobalY(int globalY) const;

    QPointer<QPushButton> m_dragButton;
    QPoint m_dragStartPos;
    bool m_dragging = false;
    QFrame *m_pill = nullptr;
    QLabel *m_brand = nullptr;
    QLabel *m_brandIcon = nullptr;
    QPushButton *m_userBtn = nullptr;
    QToolButton *m_toggleBtn = nullptr;
    QVBoxLayout *m_itemsLayout = nullptr;
    QVBoxLayout *m_bottomLayout = nullptr;
    QButtonGroup *m_group = nullptr;
    QList<Item> m_items;
    int m_current = -1; // текущая позиция элемента в визуальном списке m_items
    int m_nextPageId = 0;
    bool m_expanded = true;
    QString m_userName;
    QString m_userRole;
    QPointer<QPropertyAnimation> m_pillAnim;
    QPointer<QPropertyAnimation> m_widthAnim;
    QHash<QWidget *, QPointer<QPropertyAnimation>> m_positionAnimations;
};

// ======================================================================
// app/splash.h
// ======================================================================
class QSplashScreen;

// Заставка «PSVMS» при запуске программы
QSplashScreen *createSplash();
void finishSplash(QSplashScreen *splash);   // плавно скрывает и удаляет заставку

// ======================================================================
// app/status_dialog.h
// ======================================================================
class CameraManager;
class QLabel;
class QTableWidget;

// Окно «Состояние камер»: сколько камер в сети и не в сети, сколько пишется, список с адресами.
class CameraStatusDialog : public QDialog {
public:
    CameraStatusDialog(CameraManager *mgr, QWidget *parent = nullptr);
    void showOfflineOnly();
    void showAllCameras();

private:
    void rebuild();

    CameraManager *m_mgr;
    QLabel *m_cards[4] = {nullptr, nullptr, nullptr, nullptr};
    QTableWidget *m_table = nullptr;
    bool m_offlineOnly = false;
};

// ======================================================================
// app/theme.h
// ======================================================================
// Дизайн в духе HikCentral Professional: плоские тёмно-серые панели, прямые углы,
// боковое меню с подсветкой-«пилюлей», заголовки панелей,
// нижняя панель инструментов под видео. Акцент: ярко-жёлтый #63e6ee.
inline QString appStyleSheet()
{
    // PSVms : HikCentral Professional Dark Pro typography & button architecture
    return QStringLiteral(R"PSVMS(
* { font-family:"Segoe UI", "Bahnschrift", "Arial", sans-serif; font-size:10pt; }
QWidget { color:#ECF0F5; background:transparent; }
QLabel, QAbstractButton { min-width:0; }
QMainWindow, QWidget#centralWidget, QStackedWidget { background:#0D0F12; }

/* Left navigation rail: flat industrial console surface + left amber active bar */
QWidget#sideNav { background:#12151B; border-right:1px solid #2A303C; }
QWidget#sideNav QLabel, QWidget#sideNav QFrame { background:transparent; }
QLabel#navBrand { color:#F5B700; font-family:"Bahnschrift", "Segoe UI", sans-serif; font-size:22px; font-weight:700; letter-spacing:1.4px; }
QLabel#navUser { color:#9AA4B5; font-family:"Segoe UI", sans-serif; font-size:9.5pt; font-weight:600; }
QToolButton#navToggle {
 color:#D6DCE6; background:#1B2029; border:1px solid #2E3543; border-radius:4px;
 min-height:32px; padding:4px 8px; font-weight:700;
}
QToolButton#navToggle:hover { background:#252B37; border-color:#F5B700; color:#F5B700; }

/* Navigation items: crisp 4px radius, flat slate surface, left amber accent stripe on active */
QPushButton#navItem, QPushButton#navBottomItem, QPushButton#navLang, QPushButton#navUserBtn {
 color:#C8D0DC;
 background:#171B22;
 border:1px solid #272D39;
 border-left:3px solid transparent;
 border-radius:4px;
 min-height:42px;
 padding:7px 12px;
 text-align:left;
 font-family:"Segoe UI", sans-serif;
 font-size:10pt;
 font-weight:600;
 letter-spacing:0.2px;
}
QPushButton#navItem:hover, QPushButton#navBottomItem:hover, QPushButton#navLang:hover, QPushButton#navUserBtn:hover {
 color:#FFFFFF;
 background:#202530;
 border-color:#3A4354;
 border-left:3px solid #C99600;
}
QPushButton#navItem:checked {
 color:#F5B700;
 font-weight:700;
 background:#23221A;
 border:1px solid #5C4911;
 border-left:4px solid #F5B700;
}
QPushButton#navItem:pressed, QPushButton#navBottomItem:pressed {
 background:#F5B700;
 color:#111317;
 border-color:#F5B700;
}
QPushButton#navItem[dragging="true"] { border:1px dashed #F5B700; background:#241F12; }
QPushButton#navBottomItem[prominent="true"] {
 color:#D6DCE6; background:#191E26; border:1px solid #2E3645; border-left:3px solid #3A4354; min-height:40px;
}
QPushButton#navBottomItem[prominent="true"]:hover {
 color:#FFFFFF; background:#232935; border-color:#F5B700; border-left:3px solid #F5B700;
}
QPushButton#navItem[collapsed="true"],
QPushButton#navBottomItem[collapsed="true"],
QPushButton#navUserBtn[collapsed="true"] {
 min-width:42px; max-width:42px; padding:0px; margin:0px;
 text-align:center;
}

/* Top bar & live view action bar */
QWidget#topBar, QFrame#topBar { background:#14181F; border-bottom:1px solid #2A303C; }
QWidget#videoToolbar QToolButton, QWidget#videoToolbar QPushButton {
 color:#DCE2EB;
 background:#1A1F28;
 border:1px solid #2F3746;
 border-radius:4px;
 min-height:30px;
 padding:4px 11px;
 font-family:"Segoe UI", sans-serif;
 font-size:9.5pt;
 font-weight:600;
 letter-spacing:0.2px;
}
QWidget#videoToolbar QToolButton:hover, QWidget#videoToolbar QPushButton:hover {
 color:#FFFFFF;
 background:#242B37;
 border-color:#F5B700;
}
QWidget#videoToolbar QToolButton:pressed, QWidget#videoToolbar QToolButton:checked,
QWidget#videoToolbar QPushButton:pressed, QWidget#videoToolbar QPushButton:checked {
 color:#111317;
 background:#F5B700;
 border-color:#F5B700;
 font-weight:700;
}
QLabel#pageTitle {
 color:#ECF0F5;
 font-family:"Bahnschrift", "Segoe UI", sans-serif;
 font-size:11pt;
 font-weight:700;
 letter-spacing:0.5px;
}
QFrame, QGroupBox { border-color:#2A303C; }

/* Camera tiles: HikCentral Pro crisp slate border + signal amber selection */
QFrame#videoCell {
 background:#101318;
 border:1px solid #292F3B;
 border-radius:4px;
}
QFrame#videoCell[cellHover="true"] {
 border:1px solid #7A5F14;
 background:#14181F;
}
QFrame#videoCell[selected="true"] {
 border:2px solid #F5B700;
 background:#161A22;
}
QFrame#videoCell[dropTarget="true"] {
 border:1px dashed #F5B700;
 background:#221D11;
}
QWidget#cellBar {
 background:#181D26;
 border:0;
 border-bottom:1px solid #292F3B;
 border-top-left-radius:3px;
 border-top-right-radius:3px;
}
QLabel#cellTitle {
 color:#E4EAF2;
 background:transparent;
 border:0;
 padding:0 8px;
 font-family:"Segoe UI", sans-serif;
 font-size:9.5pt;
 font-weight:600;
 letter-spacing:0.15px;
}
QLabel#cellLiveBadge {
 color:#111317;
 background:#F5B700;
 border:1px solid #F5B700;
 border-radius:3px;
 padding:1px 7px;
 margin:0 5px;
 font-family:"Bahnschrift", "Segoe UI", sans-serif;
 font-size:8.5pt;
 font-weight:700;
 letter-spacing:0.6px;
 min-width:68px;
 max-height:22px;
}
QLabel#cellLiveBadge[paused="true"] {
 color:#F5B700;
 background:#262113;
 border-color:#9C7611;
}
QToolButton#cellBtn {
 color:#C8D0DC;
 background:transparent;
 border:1px solid transparent;
 border-radius:3px;
 padding:2px;
 min-width:25px;
 min-height:24px;
}
QToolButton#cellBtn:hover {
 color:#111317;
 background:#F5B700;
 border:1px solid #F5B700;
}
QWidget#cellIdle {
 background:#11141A;
 border:0;
 border-radius:3px;
}
QFrame#videoCell {
 background:#101318;
}
QLabel#placeholder {
 color:#8D97A8;
 background:transparent;
 border:0;
 font-size:9.5pt;
 font-weight:500;
}
QToolButton#cellPlay {
 color:#111317;
 background:#F5B700;
 border:2px solid #FFD54F;
 border-radius:32px;
 font-weight:700;
}
QToolButton#cellPlay:hover {
 background:#FFD54F;
 border:2px solid #FFFFFF;
}
QLabel#cellFlash {
 background:rgba(245,183,0,50);
 border:2px solid #F5B700;
 border-radius:4px;
}

/* Containers, standard buttons & form controls */
QGroupBox {
 border:1px solid #2A303C;
 border-radius:4px;
 margin-top:12px;
 padding:12px;
 font-family:"Segoe UI", sans-serif;
 font-size:9.5pt;
 font-weight:700;
 letter-spacing:0.25px;
 color:#F5B700;
 background:#141820;
}
QGroupBox::title { subcontrol-origin:margin; left:12px; padding:0 6px; }

QPushButton, QToolButton {
 color:#E4EAF2;
 background:#1D222C;
 border:1px solid #303847;
 border-radius:4px;
 padding:6px 14px;
 min-height:32px;
 font-family:"Segoe UI", sans-serif;
 font-size:9.5pt;
 font-weight:600;
 letter-spacing:0.2px;
 text-align:center;
}
QPushButton:hover, QToolButton:hover {
 background:#262D3A;
 border-color:#F5B700;
 color:#FFFFFF;
}
QPushButton:pressed, QToolButton:pressed {
 background:#F5B700;
 color:#111317;
 border-color:#F5B700;
 font-weight:700;
}
QPushButton:disabled, QToolButton:disabled {
 color:#5C6575;
 background:#14171D;
 border-color:#232832;
}

QLineEdit, QTextEdit, QPlainTextEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateEdit, QTimeEdit {
 color:#ECF0F5;
 background:#11141A;
 border:1px solid #2E3645;
 border-radius:4px;
 padding:6px 10px;
 min-height:32px;
 font-family:"Segoe UI", sans-serif;
 font-size:9.5pt;
 selection-background-color:#F5B700;
 selection-color:#111317;
}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QDateEdit:focus, QTimeEdit:focus {
 border:1px solid #F5B700;
 background:#151921;
}
QComboBox QAbstractItemView {
 background:#171B23;
 color:#ECF0F5;
 selection-background-color:#F5B700;
 selection-color:#111317;
 border:1px solid #353E4F;
}

/* Data grids & tables */
QTableWidget, QTableView, QTreeWidget, QTreeView, QListWidget, QListView {
 background:#11141A;
 alternate-background-color:#151921;
 color:#E2E8F0;
 border:1px solid #2A303C;
 border-radius:4px;
 gridline-color:#202631;
 font-family:"Segoe UI", sans-serif;
 font-size:9.5pt;
}
QHeaderView::section {
 color:#D0D7E2;
 background:#191E28;
 border:0;
 border-right:1px solid #272E3B;
 border-bottom:2px solid #F5B700;
 padding:7px 10px;
 font-family:"Segoe UI", sans-serif;
 font-size:9.5pt;
 font-weight:700;
 letter-spacing:0.2px;
}
QTableWidget::item, QTableView::item, QTreeWidget::item, QListWidget::item { padding:6px 8px; }
QTableWidget::item:selected, QTreeWidget::item:selected, QListWidget::item:selected {
 background:#2E2611;
 color:#FFD54F;
}

/* Tabs, scrollbars, checkboxes, progress */
QTabWidget::pane { border:1px solid #2A303C; border-radius:4px; background:#141820; top:-1px; }
QTabBar::tab {
 background:#181D26;
 color:#9AA4B5;
 border:1px solid #2A303C;
 padding:7px 14px;
 margin-right:2px;
 border-top-left-radius:4px;
 border-top-right-radius:4px;
 font-family:"Segoe UI", sans-serif;
 font-size:9.5pt;
 font-weight:600;
}
QTabBar::tab:selected {
 color:#111317;
 font-weight:700;
 background:#F5B700;
 border-color:#F5B700;
}
QScrollBar:vertical { background:#101318; width:10px; margin:1px; border-radius:4px; }
QScrollBar::handle:vertical { background:#2E3645; min-height:26px; border-radius:4px; }
QScrollBar::handle:vertical:hover { background:#F5B700; }
QScrollBar:horizontal { background:#101318; height:10px; margin:1px; border-radius:4px; }
QScrollBar::handle:horizontal { background:#2E3645; min-width:26px; border-radius:4px; }
QCheckBox, QRadioButton { spacing:8px; color:#DCE2EB; font-family:"Segoe UI", sans-serif; font-size:9.5pt; font-weight:500; }
QCheckBox::indicator, QRadioButton::indicator { width:16px; height:16px; }
QCheckBox::indicator:checked { background:#F5B700; border:1px solid #F5B700; border-radius:3px; }
QProgressBar { background:#171B23; border:1px solid #2E3645; border-radius:4px; text-align:center; color:#ECF0F5; min-height:16px; font-size:9pt; font-weight:600; }
QProgressBar::chunk { background:#F5B700; border-radius:3px; }
QToolTip { color:#ECF0F5; background:#1B2029; border:1px solid #F5B700; padding:6px 8px; font-family:"Segoe UI", sans-serif; font-size:9pt; }
QMenu { background:#171B23; color:#E4EAF2; border:1px solid #323B4B; padding:4px; font-family:"Segoe UI", sans-serif; font-size:9.5pt; }
QMenu::item { padding:6px 22px 6px 10px; border-radius:3px; font-weight:500; }
QMenu::item:selected { background:#F5B700; color:#111317; font-weight:700; }

/* Status chips & volume */
QLabel#chipOnline { color:#4ADE80; font-family:"Segoe UI", sans-serif; font-size:9pt; font-weight:700; background:#12241B; border:1px solid #237A4B; border-radius:4px; padding:4px 10px; }
QLabel#chipOffline { color:#F5B700; font-family:"Segoe UI", sans-serif; font-size:9pt; font-weight:700; background:#241E11; border:1px solid #9C7611; border-radius:4px; padding:4px 10px; }
QPushButton#cameraStatusBtn { color:#E4EAF2; font-family:"Segoe UI", sans-serif; font-size:9.5pt; font-weight:600; min-width:100px; padding:4px 10px; border:1px solid #353E4F; border-radius:4px; background:#1A1F28; }
QPushButton#cameraStatusBtn:hover { background:#F5B700; color:#111317; border-color:#F5B700; font-weight:700; }
QSlider#cellVolume { background:transparent; }
QSlider#cellVolume::groove:vertical { background:#12151B; border:1px solid #2E3645; width:6px; border-radius:3px; }
QSlider#cellVolume::sub-page:vertical { background:#F5B700; border-radius:3px; }
QSlider#cellVolume::add-page:vertical { background:#1A1F28; border-radius:3px; }
QSlider#cellVolume::handle:vertical { background:#F5B700; border:1px solid #FFFFFF; width:14px; height:14px; margin:-4px -4px; border-radius:7px; }
QStatusBar { background:#0D0F12; color:#8D97A8; border-top:1px solid #262C38; padding:2px 6px; font-family:"Segoe UI", sans-serif; font-size:9pt; }
)PSVMS");
}

// app/top_bar.h

// ======================================================================
// app/top_bar.h
// ======================================================================
class CameraManager;
class QLabel;
class QPushButton;

// Верхняя панель страницы: название раздела, счётчики камер, полноэкранный режим,
// переключатель простого режима и кнопка облака. Настройки доступны через левое меню.
class TopBar : public QWidget {
    Q_OBJECT
public:
    explicit TopBar(CameraManager *mgr, QWidget *parent = nullptr);
    void setTitle(const QString &title);
signals:
    void statusRequested();
    void fullscreenRequested();
    void cloudRequested();
    void simpleModeToggled(bool simple);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateCounts();
    void updateModeButton();
    void updateStatusVisibility();
    void applyControlPositions();
    void saveControlPositions();
    void registerDraggable(QWidget *widget, const QPoint &defaultPos);

    CameraManager *m_mgr;
    QLabel *m_title = nullptr;
    QLabel *m_online = nullptr;
    QLabel *m_offline = nullptr;
    QPushButton *m_status = nullptr;
    QPushButton *m_fullscreen = nullptr;
    QPushButton *m_cloud = nullptr;
    QPushButton *m_mode = nullptr;
    QList<QWidget *> m_draggableControls;
    QHash<QWidget *, QPoint> m_defaultControlPositions;
    QHash<QWidget *, QPropertyAnimation *> m_positionAnimations;
    QWidget *m_dragCandidate = nullptr;
    QWidget *m_draggingControl = nullptr;
    QPoint m_dragOffset;
    QPoint m_dragPressGlobal;
    bool m_dragMoved = false;
    bool m_simple = false;
};

// ======================================================================
// app/ui_helpers.h
// ======================================================================
// Стандартные кнопки Qt всегда английские (OK/Cancel); подписываем их на выбранном языке.
inline void localizeButtons(QDialogButtonBox *box)
{
    if (QPushButton *ok = box->button(QDialogButtonBox::Ok))
        ok->setText(T("ОК"));
    if (QPushButton *cancel = box->button(QDialogButtonBox::Cancel))
        cancel->setText(T("Отмена"));
}

// Поле пароля со звёздочками и значком-глазком, который временно показывает пароль открытым текстом
// (пока зажата кнопка — отпустили, снова звёздочки). Возвращает контейнер для размещения в layout;
// сам QLineEdit отдаётся через edit (создавать его заранее не нужно).
inline QIcon makePasswordEyeIcon(bool open)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor accent(open ? "#e8c45b" : "#63e6ee");
    p.setPen(QPen(accent, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 24));
    QPainterPath eye;
    eye.moveTo(3.5, 16);
    eye.cubicTo(9, 7, 23, 7, 28.5, 16);
    eye.cubicTo(23, 25, 9, 25, 3.5, 16);
    p.drawPath(eye);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(12, 11, 8, 10));
    p.setBrush(accent);
    p.drawEllipse(QRectF(14.5, 14, 3, 4));
    if (open) {
        p.setPen(QPen(QColor("#e8c45b"), 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(5, 27, 27, 5);
    }
    return QIcon(pm);
}

// Поле пароля: глаз находится в отдельной фиксированной ячейке и не выходит за границы строки.
inline QWidget *passwordFieldWithEye(QWidget *parent, QLineEdit **edit)
{
    auto *box = new QWidget(parent);
    box->setObjectName(QStringLiteral("passwordFieldRow"));
    box->setMinimumWidth(0);
    auto *lay = new QHBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(7);

    auto *field = new QLineEdit(box);
    field->setObjectName(QStringLiteral("passwordInput"));
    field->setEchoMode(QLineEdit::Password);
    field->setMinimumWidth(0);
    field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    field->setMinimumHeight(38);

    auto *eye = new QPushButton(box);
    eye->setObjectName(QStringLiteral("eyeBtn"));
    eye->setIcon(makePasswordEyeIcon(false));
    eye->setIconSize(QSize(23, 23));
    eye->setCursor(Qt::PointingHandCursor);
    eye->setFocusPolicy(Qt::NoFocus);
    eye->setToolTip(T("Удерживайте, чтобы показать пароль"));
    eye->setFixedSize(40, 38);
    eye->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    eye->setStyleSheet(QStringLiteral(
        "QPushButton#eyeBtn{color:#63e6ee;background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #19465e,stop:1 #0b2639);"
        "border:1px solid #287f9e;border-radius:9px;padding:0px;}"
        "QPushButton#eyeBtn:hover{background:#205b73;border-color:#8af7ff;}"
        "QPushButton#eyeBtn:pressed{background:#806b2b;border-color:#e8c45b;}"));

    QObject::connect(eye, &QPushButton::pressed, field, [field, eye] {
        field->setEchoMode(QLineEdit::Normal);
        eye->setIcon(makePasswordEyeIcon(true));
        eye->setToolTip(T("Пароль виден"));
    });
    QObject::connect(eye, &QPushButton::released, field, [field, eye] {
        field->setEchoMode(QLineEdit::Password);
        eye->setIcon(makePasswordEyeIcon(false));
        eye->setToolTip(T("Удерживайте, чтобы показать пароль"));
    });

    lay->addWidget(field, 1);
    lay->addWidget(eye, 0, Qt::AlignVCenter);
    if (edit)
        *edit = field;
    return box;
}

// ======================================================================
// app/video_cell.h
// ======================================================================
class QAudioOutput;
class QCamera;
class QEnterEvent;
class QGraphicsOpacityEffect;
class QLabel;
class QLineEdit;
class QMediaCaptureSession;
class QMediaPlayer;
class QPropertyAnimation;
class QPushButton;
class QStackedLayout;
class QTimer;
class QToolButton;
class VideoSurface;

// Одна ячейка мозаики. Заголовок: название камеры (за него ячейку можно перетащить), звук, закрытие ✕.
// Остановленная ячейка показывает «▶». Двойной щелчок — на весь экран и обратно, колесо мыши —
// увеличение. Меню камеры по правой кнопке отключено: настройки — через «Устройства».
class VideoCell : public QFrame {
    Q_OBJECT
public:
    explicit VideoCell(QWidget *parent = nullptr);

    bool isEmpty() const { return m_cameraId.isEmpty(); }
    bool isStarted() const { return m_started; }
    bool isMuted() const { return m_muted; }
    QString cameraId() const { return m_cameraId; }
    QUrl url() const { return m_url; }
    VideoSurface *takeSurfaceForFullscreen();
    void restoreSurfaceFromFullscreen(VideoSurface *surface);

    void setCamera(const QString &id, const QString &title, const QUrl &url, bool autoplay,
                   bool showTime, int brightness, bool audio, bool fillCell = false);
    void setViewOptions(const QString &title, bool showTime, int brightness);
    void setFillCell(bool on);
    void startPlayback();
    void stopPlayback();                 // остановить показ: ячейка остаётся за камерой, показывается «▶»
    void clear(bool notify = true);
    void setSelected(bool on);
    void setDropTarget(bool on);
    void refreshCellArtwork(); // повторное центрирование после смены режима интерфейса
    void refreshVideoGeometry(); // принудительно синхронизировать вложенную видеоповерхность с актуальной геометрией ячейки
    void setMuted(bool muted);
    QImage snapshot() const;
    void resetZoom();
    void flash();                        // короткая вспышка после перестановки

signals:
    void clicked(VideoCell *cell);
    void doubleClicked(VideoCell *cell);
    void cameraDropped(VideoCell *cell, const QString &cameraId);
    void groupDropped(VideoCell *cell, const QStringList &cameraIds);
    void cellDropped(VideoCell *cell, quintptr sourceCell);
    void contextMenuRequested(VideoCell *cell, const QPoint &globalPos);
    void addRequested(VideoCell *cell);
    void assignmentChanged();
    // Камера отказала во входе; cell сам показал окошко с полями и подставил введённые данные —
    // здесь только просьба сохранить их в настройках камеры (новый m_url уже установлен).
    void credentialsEntered(VideoCell *cell, const QUrl &newUrl);

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void fitHintFont();   // размер шрифта подсказки «Перетащите камеру сюда» под размер ячейки

private:
    void restart();
    void scheduleReconnect();
    bool isLocalCapture() const;   // адрес вида qcam:... — локальная карта видеозахвата, не RTSP
    void updateIdlePage();
    void updateCellArtwork(); // векторная иконка камеры сохраняет размер и центрируется в ячейке
    void updateFrameGlow();
    void startDrag();
    QPixmap dragPixmap() const;
    void showAuthPrompt();    // окошко логина/пароля прямо в ячейке — камера отказала во входе
    void hideAuthPrompt();
    void submitAuthPrompt();

    QString m_cameraId;
    QString m_titleText;
    QUrl m_url;
    bool m_started = false;
    bool m_muted = true;
    bool m_hover = false;
    QPoint m_dragStart;

    QLabel *m_title = nullptr;
    QLabel *m_liveBadge = nullptr;
    QToolButton *m_audioBtn = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QToolButton *m_closeBtn = nullptr;
    QWidget *m_bar = nullptr;
    QWidget *m_idle = nullptr;
    QToolButton *m_plus = nullptr;
    QToolButton *m_play = nullptr;
    QLabel *m_logo = nullptr;
    QLabel *m_hint = nullptr;
    QGraphicsOpacityEffect *m_plusFx = nullptr;
    QPropertyAnimation *m_plusAnim = nullptr;
    QStackedLayout *m_stack = nullptr;
    VideoSurface *m_surface = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOut = nullptr;
    QCamera *m_camera = nullptr;                    // для аналоговых камер через карту видеозахвата
    QMediaCaptureSession *m_capture = nullptr;
    QTimer *m_retry = nullptr;
    QTimer *m_connectTimeout = nullptr; // контроль времени до первого декодированного кадра
    qint64 m_connectStartedMs = 0;
    bool m_firstFrameReceived = false;
    int m_retryDelayMs = 3000; // экспоненциальная пауза переподключения: 3–30 секунд
    QLabel *m_flash = nullptr;
    QGraphicsOpacityEffect *m_flashFx = nullptr;
    QGraphicsDropShadowEffect *m_frameGlow = nullptr;

    // окошко логина/пароля прямо в ячейке (создаётся лениво, только когда реально понадобилось)
    QWidget *m_authBox = nullptr;
    QLabel *m_authHost = nullptr;
    QLineEdit *m_authUser = nullptr;
    QLineEdit *m_authPass = nullptr;
    QPushButton *m_authBtn = nullptr;
    bool m_authShown = false;
};

// ======================================================================
// app/video_grid.h
// ======================================================================
class CameraManager;
class QGridLayout;
class QTimer;
class VideoCell;
struct CameraEntry;

struct GridLayoutDef {
    int count;   // число ячеек
    int cols;    // число столбцов
};

// Мозаика с раскладками от 1 до 64 ячеек. Ячейки можно перетаскивать друг на друга (камеры меняются
// местами), добавлять камеры из списка и кнопкой «+». Расположение камер сохраняется как шаблон
// автоматически; кроме того, можно хранить именованные шаблоны.
class VideoGrid : public QWidget {
    Q_OBJECT
public:
    explicit VideoGrid(CameraManager *mgr, QWidget *parent = nullptr);

    static QList<GridLayoutDef> layouts();
    static QString layoutLabel(int count);       // например, «3 × 2»
    int layoutCount() const { return m_count; }
    QStringList cameraIds() const;                // по ячейкам; пустая строка — свободная ячейка
    QString selectedCameraId() const;
    void flushLayout();                           // сохранить расположение камер сразу (при выходе)

    QStringList templateNames() const;
    void saveTemplate(const QString &name);
    void applyTemplateNamed(const QString &name);
    void removeTemplate(const QString &name);

public slots:
    void setLayoutCount(int n);
    void openCamera(const QString &cameraId);
    void clearAll();
    void startAllCells();
    void stopAllCells();
    void refreshCells();
    void restoreLayout();                         // последнее расположение камер
    void closeSelectedCell();                      // горячая клавиша Delete

signals:
    void message(const QString &text);
    void archiveRequested(const QString &cameraId);
    void addCameraRequested();
    void layoutCountChanged(int count);
    void selectionChanged(const QString &cameraId);   // выделенная ячейка сменилась (может быть пустой)

private:
    void ensureCells(int n);
    void applyLayout();
    void select(VideoCell *cell);
    void placeCamera(VideoCell *cell, const QString &cameraId, bool autoplay);
    QUrl urlFor(const CameraEntry &e, const VideoCell *cell) const;
    void applyTemplateData(int count, const QStringList &ids, bool autoplay);
    void saveSoon();
    void saveLayoutNow();
    void swapCells(VideoCell *a, VideoCell *b);
    void showCellMenu(VideoCell *cell, const QPoint &globalPos);
    void showAddMenu(VideoCell *cell, const QPoint &globalPos);
    void editCamera(const QString &cameraId);
    void saveSnapshot(VideoCell *cell);
    void openFullscreen(VideoCell *cell);

    CameraManager *m_mgr;
    QGridLayout *m_grid = nullptr;
    QList<VideoCell *> m_cells;
    VideoCell *m_selected = nullptr;
    QTimer *m_saveTimer = nullptr;
    int m_count = 4;
    bool m_restoring = false;
};

// ======================================================================
// app/video_surface.h
// ======================================================================
class QTimer;
class QVideoFrame;
class QVideoSink;

// Поверхность вывода видео. Сама рисует кадры (QVideoSink), поэтому умеет то, чего нет у QVideoWidget:
// ограничение частоты кадров для слабых компьютеров, увеличение колесом мыши, дату и время поверх
// картинки, яркость, снимок кадра.
class VideoSurface : public QWidget {
    Q_OBJECT
public:
    explicit VideoSurface(QWidget *parent = nullptr);

    QVideoSink *sink() const { return m_sink; }
    QImage currentFrame() const { return m_frame; }
    bool hasFrame() const { return !m_frame.isNull(); }

    void setShowTime(bool on);
    void setBrightness(int percent);          // от -50 до 50
    void setFillCell(bool on);                 // заполнить всю ячейку, обрезая края кадра
    void setStatusText(const QString &text);  // «Подключение…», «Нет сигнала»
    void setCameraRunning(bool running);      // фирменный индикатор запущенной камеры
    void setCameraKey(const QString &cameraId); // ключ для сохранения масштаба между запусками
    void resetZoom();
    void clearFrame();

    // качество связи: 0 — нет сигнала (красный), 1 — подключение (жёлтый), 2 — стабильно (зелёный)
    enum class LinkQuality { None, Connecting, Good };
    void setLinkQuality(LinkQuality q);

signals:
    void clicked();
    void doubleClicked();
    void contextMenuRequested(const QPoint &globalPos);

protected:
    void paintEvent(QPaintEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    void onFrame(const QVideoFrame &frame);
    QRectF targetRect() const;
    void updateInterval();

    QVideoSink *m_sink = nullptr;
    QImage m_frame;
    QRectF m_view{0.0, 0.0, 1.0, 1.0};   // видимая часть кадра в долях (0..1)
    QString m_status;
    bool m_showTime = true;
    bool m_fillCell = false;
    bool m_cameraRunning = false;
    QString m_cameraKey;
    int m_brightness = 0;
    qint64 m_lastFrameMs = 0;
    int m_minIntervalMs = 66;

    // измерение FPS: считаем кадры за последнюю секунду
    qint64 m_fpsWindowStartMs = 0;
    int m_fpsFrameCount = 0;
    int m_fps = 0;
    LinkQuality m_quality = LinkQuality::None;
    bool m_panning = false;
    QPoint m_panStart;
    QRectF m_panView;
    QTimer *m_clock = nullptr;
};

// Всплывающий предпросмотр камеры при наведении на её название в таблицах.
// Поток запускается только на время наведения и останавливается при уходе курсора.
class CameraHoverPreview final : public QObject {
public:
    CameraHoverPreview(QTableWidget *table, CameraManager *manager, QObject *parent = nullptr);
    ~CameraHoverPreview() override;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void schedule(const QString &cameraId, const QPoint &globalPos);
    void showPreview();
    void hidePreview();
    QPointer<QTableWidget> m_table;
    CameraManager *m_manager = nullptr;
    QTimer *m_delay = nullptr;
    QWidget *m_popup = nullptr;
    VideoSurface *m_video = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QLabel *m_info = nullptr;
    QString m_pendingId;
    QString m_currentId;
    QPoint m_globalPos;
};

// ======================================================================
// core/app_paths.h
// ======================================================================
// Данные хранятся в data/<machine-id> рядом с EXE. При переносе папки на другой ПК
// создаётся отдельный профиль; камеры и настройки не переезжают вместе с флешкой.
// Папка может быть недоступна для записи (например, программа установлена в Program Files) —
// тогда используется запасной вариант в %LOCALAPPDATA%\PSVMS.
namespace AppPaths {

QString dataDir();            // <папка программы>/data/<machine-id> (или запасной путь)
void initSettingsLocation(const QString &applicationDir);  // вызывается до любого QSettings

} // namespace AppPaths

// ======================================================================
// core/backup.h
// ======================================================================
// Резервная копия камер, настроек, шаблонов раскладки и расположения камер в окне.
// Пользователи и пароли входа в копию не попадают. ВНИМАНИЕ: адреса камер содержат пароли камер.
namespace Backup {

bool exportTo(const QString &path, QString *error);
bool importFrom(const QString &path, QString *error);   // после восстановления нужен перезапуск программы

} // namespace Backup

// ======================================================================
// core/icsee.h
// ======================================================================
// Камеры iCSee построены на чипах Xiongmai (XM, они же XMeye/Sofia).
// RTSP-адрес у них особый: логин и пароль передаются прямо в пути.
//   stream = 0 — основной поток, stream = 1 — дополнительный (меньше разрешение)
//   channel — номер канала (у камеры обычно 1, у регистраторов 1..N)
inline QString icseeRtspUrl(const QString &host, quint16 port, const QString &user,
                            const QString &pass, int channel, int stream)
{
    return QStringLiteral("rtsp://%1:%2/user=%3&password=%4&channel=%5&stream=%6.sdp?real_stream")
        .arg(host)
        .arg(port)
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(user)),
             QString::fromUtf8(QUrl::toPercentEncoding(pass)))
        .arg(channel)
        .arg(stream);
}

// ======================================================================
// core/onvif_ptz.h
// ======================================================================
class QNetworkAccessManager;

// Параметры для управления поворотной камерой: адрес PTZ-сервиса и токен профиля —
// их возвращает OnvifResolver при добавлении камеры через импорт ONVIF (core/onvif.h).
struct PtzTarget {
    QString xAddr;
    QString token;
    QString user;
    QString password;

    bool valid() const { return !xAddr.isEmpty() && !token.isEmpty(); }
};

// Непрерывное движение, пока не придёт Stop: x/y/zoom — от -1.0 до 1.0 (скорость и направление).
// nam можно передавать временный — используется и забывается, ответ камеры не разбирается
// (команда либо выполнится, либо нет; повторная отправка безопасна).
void ptzContinuousMove(QNetworkAccessManager *nam, const PtzTarget &target, double x, double y, double zoom);
void ptzStop(QNetworkAccessManager *nam, const PtzTarget &target);

// ======================================================================
// core/rtsp_probe.h
// ======================================================================
// Подбор рабочего RTSP-адреса камеры. Новые и старые модели Hikvision / HiWatch,
// а также Polyvision и OEM-камеры (Dahua, Xiongmai) используют разные пути, поэтому
// программа по очереди отправляет RTSP DESCRIBE (с Basic/Digest-авторизацией) и ищет путь,
// на который камера отвечает «200 OK».
class RtspProbe : public QObject {
    Q_OBJECT
public:
    explicit RtspProbe(QObject *parent = nullptr);

    void start(const QString &host, quint16 port, const QString &user, const QString &pass);
    void cancel();

    static int templateCount();
    // Готовый адрес канала по шаблону; для подпотока — пустая строка, если шаблон его не знает
    static QString buildUrl(int templateIndex, const QString &host, quint16 port, const QString &user,
                            const QString &pass, int channel, bool sub);

signals:
    // error: "auth" — неверный логин или пароль, "none" — рабочий путь не найден или нет ответа
    void finished(bool ok, int templateIndex, const QString &error);

private:
    void tryNext();
    void request(int tmpl, const QByteArray &authHeader, bool authed);
    void onResponse(int tmpl, bool authed, int code, const QString &www);
    QByteArray buildAuthHeader(const QString &www) const;

    QString m_host;
    QString m_user;
    QString m_pass;
    QString m_lastUri;
    quint16 m_port = 554;
    int m_index = 0;
    int m_gen = 0;
};

// ======================================================================
// core/secure_store.h
// ======================================================================
// Защищённое хранение учётных данных камер. На Windows используется DPAPI
// (CryptProtectData/CryptUnprotectData) с привязкой к текущей учётной записи Windows.
// Старый формат enc:v1 поддерживается только для миграции; новые записи — enc:v2.
// DPAPI защищает секреты на диске, но не от вредоносного ПО, уже работающего
// от имени вошедшего пользователя. Для максимальной защиты дополнительно нужны
// BitLocker, отдельная непривилегированная учётная запись и подписанные релизы.
namespace SecureStore {

QString encrypt(const QString &plain);     // пустая строка остаётся пустой (не шифруем «ничего»)
QString decrypt(const QString &cipherB64);  // DPAPI для enc:v2
QString decryptLegacy(const QString &cipherB64); // только миграция enc:v1

} // namespace SecureStore

// ======================================================================
// core/stream_presets.h
// ======================================================================
struct StreamOption {
    QString label;   // например: «Подпоток (102)»
    QString url;
};

// Предлагает потоки камеры, подставляя номер потока в адрес по шаблонам производителей:
//   Hikvision / HiWatch:  .../Streaming/Channels/101, 102, 103  (основной, подпоток, третий)
//   Dahua и OEM:          ...&subtype=0, 1, 2
//   iCSee (Xiongmai):     ...&stream=0, 1
// Для остальных адресов (например, полученных по ONVIF) шаблона нет, список пуст.
QList<StreamOption> streamOptionsFor(const QString &url);

// ======================================================================
// core/sys_stats.h
// ======================================================================
// Загрузка процессора (%) и памяти (%, занято/всего в МБ). На Windows — через системные счётчики,
// на других платформах («сборка для разработки» вне Windows) значения недоступны (-1).
struct SysStats {
    double cpuPercent = -1.0;
    double memPercent = -1.0;
    qint64 memUsedMb = 0;
    qint64 memTotalMb = 0;
};

SysStats readSysStats();

// ======================================================================
// core/users.h
// ======================================================================
struct UserInfo {
    QString id;
    QString name;
    QString role;                 // "admin" или "operator"
    bool hasPassword = false;
    bool enabled = true;

    bool valid() const { return !id.isEmpty(); }
    bool isDeveloper() const { return role == QLatin1String("developer"); }
    bool isGuest() const { return role == QLatin1String("guest"); }
    bool isAdmin() const { return role == QLatin1String("admin") || isDeveloper(); }
};

// Учётные записи программы: Гость и Administrator без пароля;
// Разработчик обязан задать пароль при первом входе. В настройках хранятся
// только соль и PBKDF2-хэш пароля.
namespace Users {

inline constexpr int kMinPassword = 4;

QList<UserInfo> all();
UserInfo find(const QString &id);
void ensureDefault();                        // создаёт гостя, Administrator и Разработчика при необходимости
bool needsLoginDialog();                     // выбор пользователя показывается при каждом запуске

QString add(const QString &name, const QString &role, const QString &password, QString *error);
bool remove(const QString &id, QString *error);
bool setPassword(const QString &id, const QString &password, QString *error);   // пустой — без пароля
bool setRole(const QString &id, const QString &role, QString *error);   // "admin" или "operator"
bool setEnabled(const QString &id, bool enabled, QString *error);
bool verify(const QString &id, const QString &password);

// «Запомнить меня»: на компьютере хранится случайный ключ, а не пароль
QString issueRememberToken(const QString &id);
bool consumeRememberedLogin(QString *userId);
void forgetRemembered();

void setCurrent(const QString &id);
UserInfo current();
QString lastUserId();
void setLastUserId(const QString &id);

} // namespace Users

namespace SecurityAudit { void write(const QString &event, const QString &details = QString()); }

// ======================================================================
// ======================================================================
// Реализации сгруппированы по подсистемам в modules/*.inc.
// Пока они включаются в одну единицу трансляции: так сохраняется видимость
// существующих внутренних функций и анонимных namespace без риска ODR.
// Следующий этап — вынос модулей в отдельные .cpp/.h с явными интерфейсами.
// ======================================================================
#include "modules/app_dialogs.inc"
#include "modules/app_pages.inc"
#include "modules/app_shell.inc"
#include "modules/app_playback_settings.inc"
#include "modules/video.inc"
#include "modules/core_services.inc"
#include "modules/application_entry.inc"

// Код moc для классов с Q_OBJECT (генерируется AUTOMOC).
#include "psvms.moc"
