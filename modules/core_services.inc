// PSVMS module: Сервисы ядра: пути, резервные копии, RTSP, ONVIF и пользователи
// This file is included by psvms.cpp to preserve the original single-TU build
// while the code is being separated safely.

// core/app_paths.cpp
// ======================================================================
namespace {

QString computeDataDir(const QString &applicationDir)
{
    // Профиль привязан к компьютеру. Это предотвращает автоматический перенос
    // списка камер и локальных учётных данных вместе с USB-копией PSVMS на другой ПК.
    QByteArray machineId = QSysInfo::machineUniqueId();
    if (machineId.isEmpty())
        machineId = QSysInfo::machineHostName().toUtf8();
    if (machineId.isEmpty())
        machineId = qgetenv("COMPUTERNAME");
    if (machineId.isEmpty())
        machineId = qgetenv("HOSTNAME");
    const QString machineKey = QString::fromLatin1(
        QCryptographicHash::hash(machineId, QCryptographicHash::Sha256).toHex().left(16));

    const QString preferred = QDir(applicationDir).filePath(
        QStringLiteral("data/") + machineKey);
    QDir().mkpath(preferred);
    // Проверяем, что в папку программы реально можно писать (например, Program Files).
    QFile probe(preferred + QStringLiteral("/.write_test"));
    if (probe.open(QIODevice::WriteOnly)) {
        probe.close();
        probe.remove();
        return preferred;
    }
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/") + machineKey;
    QDir().mkpath(fallback);
    return fallback;
}

} // namespace

namespace {
QString g_psvmsDataDir;
}

QString AppPaths::dataDir()
{
    if (g_psvmsDataDir.isEmpty())
        g_psvmsDataDir = computeDataDir(QCoreApplication::applicationDirPath());
    return g_psvmsDataDir;
}

void AppPaths::initSettingsLocation(const QString &applicationDir)
{
    // Профиль строго привязан к компьютеру: при переносе папки PSVMS на другой ПК
    // будет создан новый каталог data/<machine-id>, старые камеры с флешки не загрузятся.
    // При этом на одном и том же ПК камеры и настройки сохраняются между запусками.
    if (g_psvmsDataDir.isEmpty())
        g_psvmsDataDir = computeDataDir(applicationDir);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, g_psvmsDataDir);
    QSettings::setDefaultFormat(QSettings::IniFormat);
}

// ======================================================================
// core/backup.cpp
// ======================================================================
namespace {

bool isBackupKey(const QString &key)
{
    if (key == QLatin1String("ui/geometry"))
        return false;
    static const char *const prefixes[] = {"cameras/", "layout/", "templates/", "general/",
                                           "storage/", "perf/", "view/", "ui/", "cloud/"};
    for (const char *p : prefixes)
        if (key.startsWith(QLatin1String(p)))
            return true;
    return false;
}

QVariant backupValueFromJson(const QJsonValue &v)
{
    if (v.isArray()) {
        QStringList list;
        const QJsonArray arr = v.toArray();
        for (const QJsonValue &x : arr)
            list << x.toVariant().toString();
        return list;
    }
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (d == double(qint64(d)))
            return qint64(d);
    }
    return v.toVariant();
}

} // namespace

bool Backup::exportTo(const QString &path, QString *error)
{
    QSettings s;
    QJsonObject keys;
    const QStringList all = s.allKeys();
    for (const QString &k : all) {
        if (isBackupKey(k))
            keys.insert(k, QJsonValue::fromVariant(s.value(k)));
    }
    QJsonObject root;
    root.insert(QStringLiteral("app"), QStringLiteral("PSVMS"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
    root.insert(QStringLiteral("settings"), keys);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = T("Не удалось записать файл: %1").arg(f.errorString());
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

bool Backup::importFrom(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = T("Не удалось прочитать файл: %1").arg(f.errorString());
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()
        || doc.object().value(QStringLiteral("app")).toString() != QLatin1String("PSVMS")) {
        if (error) *error = T("Файл повреждён или не является резервной копией программы.");
        return false;
    }
    const QJsonObject keys = doc.object().value(QStringLiteral("settings")).toObject();

    QSettings s;
    static const char *const groups[] = {"cameras", "layout", "templates", "general",
                                         "storage", "perf", "view", "cloud"};
    for (const char *g : groups)
        s.remove(QString::fromLatin1(g));
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        if (isBackupKey(it.key()))
            s.setValue(it.key(), backupValueFromJson(it.value()));
    }
    s.sync();
    return true;
}

// ======================================================================
// core/camera_worker.cpp
// ======================================================================
namespace {

QString avErr(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

// Пишет один mkv-сегмент: копирует видео (и аудио, если mkv его поддерживает)
// из входного потока, сдвигая метки времени так, чтобы сегмент начинался с 0.
class SegmentWriter {
public:
    ~SegmentWriter() { close(); }

    bool isOpen() const { return m_oc != nullptr; }

    bool open(const QString &path, const AVFormatContext *in, QString *err)
    {
        m_path = path;
        const QByteArray fn = path.toUtf8();   // FFmpeg на Windows ждёт UTF-8 (в пути может быть кириллица)

        int r = avformat_alloc_output_context2(&m_oc, nullptr, "matroska", fn.constData());
        if (r < 0 || !m_oc)
            return fail(err, QStringLiteral("alloc_output_context2"), r < 0 ? r : AVERROR(ENOMEM));

        m_map.assign(in->nb_streams, -1);
        m_lastDts.assign(in->nb_streams, INT64_MIN);

        for (unsigned i = 0; i < in->nb_streams; ++i) {
            const AVCodecParameters *par = in->streams[i]->codecpar;
            if (par->codec_type != AVMEDIA_TYPE_VIDEO && par->codec_type != AVMEDIA_TYPE_AUDIO)
                continue;
            // экзотический аудиокодек, который mkv не принимает: пишем только видео
            if (par->codec_type == AVMEDIA_TYPE_AUDIO
                && avformat_query_codec(m_oc->oformat, par->codec_id, FF_COMPLIANCE_NORMAL) != 1)
                continue;

            AVStream *os = avformat_new_stream(m_oc, nullptr);
            if (!os)
                return fail(err, QStringLiteral("new_stream"), AVERROR(ENOMEM));
            r = avcodec_parameters_copy(os->codecpar, par);
            if (r < 0)
                return fail(err, QStringLiteral("parameters_copy"), r);
            os->codecpar->codec_tag = 0;
            m_map[i] = os->index;
        }

        r = avio_open(&m_oc->pb, fn.constData(), AVIO_FLAG_WRITE);
        if (r < 0)
            return fail(err, QStringLiteral("avio_open"), r);
        r = avformat_write_header(m_oc, nullptr);
        if (r < 0)
            return fail(err, QStringLiteral("write_header"), r);
        m_headerWritten = true;
        return true;
    }

    // 0 — пакет записан или пропущен, <0 — ошибка записи.
    // Пакет после вызова больше не нужен (av_packet_unref вызывать можно).
    int write(AVPacket *pkt, const AVStream *in)
    {
        const int idx = pkt->stream_index;
        if (idx < 0 || idx >= int(m_map.size()) || m_map[idx] < 0)
            return 0;
        const int o = m_map[idx];
        AVStream *os = m_oc->streams[o];

        int64_t dts = pkt->dts, pts = pkt->pts;
        if (dts == AV_NOPTS_VALUE) dts = pts;
        if (pts == AV_NOPTS_VALUE) pts = dts;
        if (dts == AV_NOPTS_VALUE) return 0;

        // Точка отсчёта сегмента — dts первого записанного пакета (ключевой кадр видео).
        if (m_baseTs == AV_NOPTS_VALUE) {
            m_baseTs = dts;
            m_baseTb = in->time_base;
        }
        const int64_t off = av_rescale_q(m_baseTs, m_baseTb, in->time_base);
        dts -= off;
        pts -= off;
        if (dts < 0)
            return 0;   // аудио, пришедшее раньше первого ключевого кадра

        dts = av_rescale_q(dts, in->time_base, os->time_base);
        pts = av_rescale_q(pts, in->time_base, os->time_base);
        const int64_t dur = pkt->duration > 0
            ? av_rescale_q(pkt->duration, in->time_base, os->time_base) : 0;

        // mkv требует неубывающих dts; RTSP-камеры иногда присылают повторы
        if (dts <= m_lastDts[o]) dts = m_lastDts[o] + 1;
        if (pts < dts) pts = dts;
        m_lastDts[o] = dts;

        pkt->pts = pts;
        pkt->dts = dts;
        pkt->duration = dur;
        pkt->stream_index = o;
        pkt->pos = -1;
        return av_interleaved_write_frame(m_oc, pkt);
    }

    // Закрывает файл, возвращает его размер в байтах.
    qint64 close()
    {
        if (!m_oc)
            return 0;
        if (m_headerWritten)
            av_write_trailer(m_oc);
        if (m_oc->pb)
            avio_closep(&m_oc->pb);
        avformat_free_context(m_oc);
        m_oc = nullptr;
        m_headerWritten = false;
        m_baseTs = AV_NOPTS_VALUE;
        return QFileInfo(m_path).size();
    }

private:
    bool fail(QString *err, const QString &what, int code)
    {
        if (err)
            *err = what + QStringLiteral(": ") + avErr(code);
        if (m_oc) {
            if (m_oc->pb)
                avio_closep(&m_oc->pb);
            avformat_free_context(m_oc);
            m_oc = nullptr;
        }
        m_headerWritten = false;
        return false;
    }

    AVFormatContext *m_oc = nullptr;
    QString m_path;
    bool m_headerWritten = false;
    std::vector<int> m_map;          // индекс входного потока -> индекс выходного (или -1)
    std::vector<int64_t> m_lastDts;  // по выходным потокам
    int64_t m_baseTs = AV_NOPTS_VALUE;
    AVRational m_baseTb = {1, 1};
};

} // namespace

// ---------------------------------------------------------------------------

CameraWorker::CameraWorker(CameraConfig cfg, QObject *parent)
    : QThread(parent), m_cfg(std::move(cfg))
{
}

CameraWorker::~CameraWorker()
{
    stop();
    wait();
}

void CameraWorker::startRecording()
{
    m_stop = false;
    start();
}

void CameraWorker::stop()
{
    m_stop = true;
}

int CameraWorker::interruptCallback(void *opaque)
{
    auto *self = static_cast<CameraWorker *>(opaque);
    if (self->m_stop)
        return 1;
    const qint64 deadline = self->m_deadlineMs.load();
    return (deadline > 0 && QDateTime::currentMSecsSinceEpoch() > deadline) ? 1 : 0;
}

void CameraWorker::armDeadline(int ms)
{
    m_deadlineMs = QDateTime::currentMSecsSinceEpoch() + ms;
}

void CameraWorker::sleepInterruptible(int ms)
{
    for (int waited = 0; waited < ms && !m_stop; waited += 100)
        msleep(100);
}

QString CameraWorker::segmentPath(qint64 startMs) const
{
    const QDateTime t = QDateTime::fromMSecsSinceEpoch(startMs);
    const QString dir = QStringLiteral("%1/%2/%3")
                            .arg(m_cfg.storageRoot, m_cfg.id, t.toString(QStringLiteral("yyyy/MM/dd")));
    QDir().mkpath(dir);
    return QStringLiteral("%1/%2_%3.mkv")
        .arg(dir, m_cfg.id, t.toString(QStringLiteral("yyyyMMdd_HHmmss")));
}

void CameraWorker::run()
{
    // Соединение с БД должно жить в том же потоке, где используется.
    RecordingIndex index;
    if (!index.open(m_cfg.dbPath)) {
        emit errorOccurred(m_cfg.id, T("Не удалось открыть БД: %1").arg(index.lastError()));
        emit stateChanged(m_cfg.id, State::Stopped);
        return;
    }
    index.closeDanglingSegments(m_cfg.id);

    int backoffMs = 1000;
    while (!m_stop) {
        emit stateChanged(m_cfg.id, State::Connecting);

        bool gotData = false;
        const qint64 sessionStartedMs = QDateTime::currentMSecsSinceEpoch();
        recordSession(index, gotData);
        if (m_stop)
            break;

        emit stateChanged(m_cfg.id, State::Reconnecting);
        // Не сбрасываем задержку только из-за одного полученного пакета:
        // нестабильные камеры иначе вызывают частые циклы подключения.
        const qint64 sessionDurationMs = QDateTime::currentMSecsSinceEpoch() - sessionStartedMs;
        if (gotData && sessionDurationMs >= 30000)
            backoffMs = 1000;
        else
            backoffMs = qMin(backoffMs * 2, 30000);
        sleepInterruptible(backoffMs);
    }
    emit stateChanged(m_cfg.id, State::Stopped);
}

// Одна сессия: подключились -> читаем пакеты -> пишем сегменты -> вышли при обрыве или stop().
void CameraWorker::recordSession(RecordingIndex &index, bool &gotData)
{
    AVFormatContext *ic = avformat_alloc_context();
    if (!ic) {
        emit errorOccurred(m_cfg.id, T("Не хватает памяти"));
        return;
    }
    ic->interrupt_callback.callback = &CameraWorker::interruptCallback;
    ic->interrupt_callback.opaque = this;

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    const QByteArray ioTimeout = QByteArray::number(qint64(m_cfg.ioTimeoutMs) * 1000);
    // Разные сборки FFmpeg используют разные имена таймаута RTSP/I/O.
    av_dict_set(&opts, "timeout", ioTimeout.constData(), 0);
    av_dict_set(&opts, "stimeout", ioTimeout.constData(), 0);
    av_dict_set(&opts, "rw_timeout", ioTimeout.constData(), 0);

    armDeadline(m_cfg.ioTimeoutMs);
    int r = avformat_open_input(&ic, m_cfg.url.toUtf8().constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (r < 0) {   // при ошибке ic уже освобождён
        emit errorOccurred(m_cfg.id, T("Не удалось подключиться: %1").arg(avErr(r)));
        return;
    }
    auto closeInput = qScopeGuard([&] { avformat_close_input(&ic); });

    armDeadline(m_cfg.ioTimeoutMs * 2);
    r = avformat_find_stream_info(ic, nullptr);
    if (r < 0) {
        emit errorOccurred(m_cfg.id, T("Не удалось получить параметры потока: %1").arg(avErr(r)));
        return;
    }
    const int videoIdx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIdx < 0) {
        emit errorOccurred(m_cfg.id, T("В потоке нет видео"));
        return;
    }

    AVPacket *pkt = av_packet_alloc();
    auto freePkt = qScopeGuard([&] { av_packet_free(&pkt); });

    SegmentWriter writer;
    QString segPath;
    qint64 segId = -1, segStartMs = 0, lastPktMs = 0;
    bool announcedRecording = false;

    auto finishSegment = [&] {
        if (!writer.isOpen())
            return;
        const qint64 size = writer.close();
        if (segId >= 0)
            index.endSegment(segId, lastPktMs, size);
        emit segmentFinished(m_cfg.id, segPath, segStartMs, lastPktMs, size);
        segId = -1;
    };
    // Закрыть сегмент при любом выходе из функции (обрыв, ошибка, stop)
    auto closeSegment = qScopeGuard(finishSegment);

    const qint64 segmentMs = qint64(m_cfg.segmentSeconds) * 1000;

    while (!m_stop) {
        armDeadline(m_cfg.ioTimeoutMs);
        r = av_read_frame(ic, pkt);
        if (r < 0) {
            if (!m_stop)
                emit errorOccurred(m_cfg.id, T("Поток прерван: %1").arg(avErr(r)));
            break;
        }
        gotData = true;

        const bool isKey = pkt->stream_index == videoIdx && (pkt->flags & AV_PKT_FLAG_KEY);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        // Ротация только на ключевом кадре, чтобы каждый сегмент проигрывался сам по себе
        if (writer.isOpen() && isKey && now - segStartMs >= segmentMs)
            finishSegment();

        if (!writer.isOpen()) {
            if (!isKey) {                       // ждём ключевой кадр
                av_packet_unref(pkt);
                continue;
            }
            // Политика хранения и автоочистка старых закрытых сегментов (активный сегмент не трогаем)
            purgeClosedSegmentsByPolicy(index.connectionName(), m_cfg.storageRoot, m_cfg.minimumFreeBytes);

            // Защита архива от заполнения диска: не начинаем новый сегмент,
            // если накопитель недоступен или свободного места ниже заданного порога.
            QStorageInfo storage(m_cfg.storageRoot);
            storage.refresh();
            if (!storage.isValid() || !storage.isReady()) {
                emit errorOccurred(m_cfg.id, T("Накопитель архива недоступен. Запись остановлена до восстановления диска."));
                av_packet_unref(pkt);
                break;
            }
            if (storage.bytesAvailable() < m_cfg.minimumFreeBytes) {
                const qint64 freeMb = storage.bytesAvailable() / (1024LL * 1024LL);
                const qint64 requiredMb = m_cfg.minimumFreeBytes / (1024LL * 1024LL);
                emit errorOccurred(m_cfg.id, T("Недостаточно свободного места: %1 МБ. Порог остановки записи: %2 МБ.")
                    .arg(freeMb).arg(requiredMb));
                av_packet_unref(pkt);
                break;
            }
            segStartMs = now;
            segPath = segmentPath(segStartMs);
            QString err;
            if (!writer.open(segPath, ic, &err)) {
                emit errorOccurred(m_cfg.id, T("Не удалось создать сегмент: %1").arg(err));
                av_packet_unref(pkt);
                break;
            }
            segId = index.beginSegment(m_cfg.id, segPath, segStartMs);
            if (segId < 0)
                emit errorOccurred(m_cfg.id, T("Ошибка индекса: %1").arg(index.lastError()));
            emit segmentStarted(m_cfg.id, segPath, segStartMs);
            if (!announcedRecording) {
                announcedRecording = true;
                emit stateChanged(m_cfg.id, State::Recording);
            }
        }

        lastPktMs = now;
        // Сюда же позже: копия пакета в декодер для живого просмотра и аналитики.
        const int wr = writer.write(pkt, ic->streams[pkt->stream_index]);
        av_packet_unref(pkt);
        if (wr < 0) {
            emit errorOccurred(m_cfg.id, T("Ошибка записи (диск?): %1").arg(avErr(wr)));
            break;
        }
    }
}

// ======================================================================
// core/device_adder.cpp
// ======================================================================
DeviceAdder::DeviceAdder(QObject *parent) : QObject(parent) {}

void DeviceAdder::start(const QList<AddRequest> &requests, int concurrency)
{
    ++m_gen;
    m_reqs = requests;
    m_next = 0;
    m_done = 0;
    m_active = 0;
    m_conc = qMax(1, concurrency);
    m_running = true;
    if (requests.isEmpty()) {
        m_running = false;
        emit finished();
        return;
    }
    pump();
}

void DeviceAdder::cancel()
{
    m_running = false;
    ++m_gen;
}

void DeviceAdder::pump()
{
    while (m_running && m_active < m_conc && m_next < m_reqs.size()) {
        ++m_active;
        runOne(m_next++);
    }
}

void DeviceAdder::finishOne(int idx, const AddResult &r, int gen)
{
    if (gen != m_gen)
        return;
    --m_active;
    ++m_done;
    emit resolved(idx, r);
    emit progress(m_done, int(m_reqs.size()));
    if (m_done >= m_reqs.size()) {
        m_running = false;
        emit finished();
    } else {
        pump();
    }
}

void DeviceAdder::runOne(int idx)
{
    const AddRequest rq = m_reqs.at(idx);
    const int gen = m_gen;

    auto *hr = new HikResolver(this);
    connect(hr, &HikResolver::resolved, this, [this, idx, rq, gen, hr](int, const HikResult &r) {
        hr->deleteLater();
        if (gen != m_gen)
            return;
        if (r.ok) {
            const HikDevice &d = r.device;
            AddResult res;
            res.ok = true;
            res.method = QStringLiteral("ISAPI");
            res.model = d.model;
            res.name = d.name;
            res.type = d.type;
            res.recorder = d.isRecorder();
            for (const HikChannel &c : d.channels) {
                AddedChannel ch;
                ch.number = c.number;
                ch.name = c.name;
                ch.mainUrl = hikRtspUrl(rq.address, rq.rtspPort, rq.user, rq.password, c.number, 1);
                ch.subUrl = hikRtspUrl(rq.address, rq.rtspPort, rq.user, rq.password, c.number, 2);
                res.channels.append(ch);
            }
            finishOne(idx, res, gen);
        } else if (r.authFailed) {
            AddResult res;
            res.authFailed = true;
            res.error = r.error;
            finishOne(idx, res, gen);
        } else {
            probeRtsp(idx, rq, r.error, gen);   // старая прошивка или другой бренд
        }
    });

    HikRequest hq;
    hq.address = rq.address;
    hq.httpPort = rq.httpPort;
    hq.user = rq.user;
    hq.password = rq.password;
    hq.fallbackChannels = rq.channels;
    hr->start({hq}, 1);
}

void DeviceAdder::probeRtsp(int idx, const AddRequest &rq, const QString &prevError, int gen)
{
    auto *probe = new RtspProbe(this);
    connect(probe, &RtspProbe::finished, this,
            [this, idx, rq, prevError, gen, probe](bool ok, int tmpl, const QString &err) {
        probe->deleteLater();
        if (gen != m_gen)
            return;
        AddResult res;
        if (!ok) {
            res.authFailed = err == QLatin1String("auth");
            res.error = res.authFailed ? T("Неверный логин или пароль")
                                       : (prevError.isEmpty() ? T("Не удалось определить адрес потока камеры") : prevError);
            finishOne(idx, res, gen);
            return;
        }
        res.ok = true;
        res.method = QStringLiteral("RTSP");
        const int n = qMax(1, rq.channels);
        res.recorder = n > 1;
        for (int ch = 1; ch <= n; ++ch) {
            AddedChannel c;
            c.number = ch;
            c.mainUrl = RtspProbe::buildUrl(tmpl, rq.address, rq.rtspPort, rq.user, rq.password, ch, false);
            c.subUrl = RtspProbe::buildUrl(tmpl, rq.address, rq.rtspPort, rq.user, rq.password, ch, true);
            res.channels.append(c);
        }
        finishOne(idx, res, gen);
    });
    probe->start(rq.address, rq.rtspPort, rq.user, rq.password);
}

// ======================================================================
// core/hikvision.cpp
// ======================================================================
namespace {

bool isName_hik(const QXmlStreamReader &x, const char *n)
{
    return x.name() == QLatin1String(n);
}

struct HttpReply {
    bool ok = false;
    bool authFailed = false;
    QByteArray body;
    QString error;
};

// GET с HTTP Digest: логин и пароль берутся из свойств ответа (разные для каждого устройства)
void httpGet(QNetworkAccessManager *nam, QObject *ctx, const HikRequest &req, const QString &path,
             std::function<void(const HttpReply &)> cb)
{
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(req.address);
    url.setPort(req.httpPort);
    url.setPath(path);

    if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0 && !req.password.isEmpty())
        SecurityAudit::write(QStringLiteral("camera_http_credentials_warning"), req.address);
    QNetworkRequest nr(url);
    if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
        nr.setSslConfiguration(ssl);
    }
    nr.setTransferTimeout(5000);
    QNetworkReply *reply = nam->get(nr);
    reply->setProperty("hik_user", req.user);
    reply->setProperty("hik_pass", req.password);

    QObject::connect(reply, &QNetworkReply::finished, ctx, [reply, cb] {
        HttpReply h;
        h.body = reply->readAll();
        h.ok = reply->error() == QNetworkReply::NoError;
        h.authFailed = reply->error() == QNetworkReply::AuthenticationRequiredError;
        if (!h.ok) {
            switch (reply->error()) {
            case QNetworkReply::AuthenticationRequiredError:
                h.error = T("Неверный логин или пароль (или устройство не активировано)");
                break;
            case QNetworkReply::TimeoutError:
                h.error = T("Не отвечает (таймаут)");
                break;
            case QNetworkReply::ConnectionRefusedError:
                h.error = T("Порт закрыт");
                break;
            case QNetworkReply::ContentNotFoundError:
                h.error = T("Это не Hikvision/HiWatch (нет ISAPI)");
                break;
            default:
                h.error = reply->errorString();
            }
        }
        reply->deleteLater();
        cb(h);
    });
}

void parseDeviceInfo(const QByteArray &xml, HikDevice *dev)
{
    QXmlStreamReader x(xml);
    while (!x.atEnd()) {
        x.readNext();
        if (!x.isStartElement())
            continue;
        if (isName_hik(x, "deviceName")) dev->name = x.readElementText().trimmed();
        else if (isName_hik(x, "model")) dev->model = x.readElementText().trimmed();
        else if (isName_hik(x, "serialNumber")) dev->serial = x.readElementText().trimmed();
        else if (isName_hik(x, "deviceType")) dev->type = x.readElementText().trimmed();
    }
}

// Каналы из /ISAPI/Streaming/channels: id 101 и 102 — канал 1, 201 и 202 — канал 2 и т. д.
QList<HikChannel> parseChannels(const QByteArray &xml)
{
    QMap<int, QString> found;
    QXmlStreamReader x(xml);
    bool inCh = false;
    int id = 0;
    QString name;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            if (isName_hik(x, "StreamingChannel")) {
                inCh = true;
                id = 0;
                name.clear();
            } else if (inCh && id == 0 && isName_hik(x, "id")) {
                id = x.readElementText().toInt();
            } else if (inCh && name.isEmpty() && isName_hik(x, "channelName")) {
                name = x.readElementText().trimmed();
            }
        } else if (x.isEndElement() && isName_hik(x, "StreamingChannel")) {
            if (id >= 101 && (id % 100 == 1 || id % 100 == 2)) {
                const int n = id / 100;
                if (!found.contains(n) || found.value(n).isEmpty())
                    found[n] = name;
            }
            inCh = false;
        }
    }
    QList<HikChannel> out;
    for (auto it = found.cbegin(); it != found.cend(); ++it) {
        HikChannel c;
        c.number = it.key();
        c.name = it.value();
        out.append(c);
    }
    return out;
}

} // namespace

// ===========================================================================
// HikDiscovery (SADP)
// ===========================================================================

HikDiscovery::HikDiscovery(QObject *parent) : QObject(parent)
{
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &HikDiscovery::onReadyRead);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] {
        m_running = false;
        emit finished();
    });
}

void HikDiscovery::start(int timeoutMs)
{
    stop();
    m_seen.clear();

    // Устройства отвечают на multicast-группу, поэтому слушаем порт 37020 и входим в группу
    if (m_socket->state() != QAbstractSocket::BoundState
        && !m_socket->bind(QHostAddress::AnyIPv4, 37020,
                           QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit finished();
        return;
    }
    const QHostAddress group(QStringLiteral("239.255.255.250"));
    for (const QNetworkInterface &ni : QNetworkInterface::allInterfaces()) {
        const auto fl = ni.flags();
        if ((fl & QNetworkInterface::IsUp) && (fl & QNetworkInterface::IsRunning)
            && (fl & QNetworkInterface::CanMulticast) && !(fl & QNetworkInterface::IsLoopBack))
            m_socket->joinMulticastGroup(group, ni);
    }
    m_socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 4);

    m_running = true;
    sendProbes();
    QTimer::singleShot(1000, this, [this] { if (m_running) sendProbes(); });
    m_timer->start(timeoutMs);
}

void HikDiscovery::stop()
{
    m_running = false;
    m_timer->stop();
}

void HikDiscovery::sendProbes()
{
    const QByteArray probe =
        QByteArray("<?xml version=\"1.0\" encoding=\"utf-8\"?><Probe><Uuid>")
        + QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper().toUtf8()
        + QByteArray("</Uuid><Types>inquiry</Types></Probe>");

    const QHostAddress group(QStringLiteral("239.255.255.250"));
    for (const QNetworkInterface &ni : QNetworkInterface::allInterfaces()) {
        const auto fl = ni.flags();
        if (!(fl & QNetworkInterface::IsUp) || !(fl & QNetworkInterface::IsRunning)
            || (fl & QNetworkInterface::IsLoopBack) || !(fl & QNetworkInterface::CanMulticast))
            continue;
        m_socket->setMulticastInterface(ni);
        m_socket->writeDatagram(probe, group, 37020);
    }
}

void HikDiscovery::onReadyRead()
{
    while (m_socket->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(int(m_socket->pendingDatagramSize()));
        m_socket->readDatagram(data.data(), data.size());
        if (!m_running || !data.contains("ProbeMatch"))   // свои же запросы пропускаем
            continue;

        HikDevice dev;
        QXmlStreamReader x(data);
        while (!x.atEnd()) {
            x.readNext();
            if (!x.isStartElement())
                continue;
            if (isName_hik(x, "IPv4Address")) dev.address = x.readElementText().trimmed();
            else if (isName_hik(x, "HttpPort")) dev.httpPort = quint16(x.readElementText().toUInt());
            else if (isName_hik(x, "DeviceDescription")) dev.model = x.readElementText().trimmed();
            else if (isName_hik(x, "DeviceSN")) dev.serial = x.readElementText().trimmed();
            else if (isName_hik(x, "Activated")) dev.activated = x.readElementText().trimmed() != QLatin1String("false");
        }
        if (dev.httpPort == 0)
            dev.httpPort = 80;
        if (dev.address.isEmpty() || m_seen.contains(dev.address))
            continue;
        m_seen.insert(dev.address);
        emit deviceFound(dev);
    }
}

// ===========================================================================
// HikResolver (ISAPI)
// ===========================================================================

HikResolver::HikResolver(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    connect(m_nam, &QNetworkAccessManager::authenticationRequired, this,
            [](QNetworkReply *reply, QAuthenticator *auth) {
                auth->setUser(reply->property("hik_user").toString());
                auth->setPassword(reply->property("hik_pass").toString());
            });
}

void HikResolver::start(const QList<HikRequest> &requests, int concurrency)
{
    ++m_gen;
    m_requests = requests;
    m_next = 0;
    m_done = 0;
    m_active = 0;
    m_conc = qMax(1, concurrency);
    m_running = true;
    if (requests.isEmpty()) {
        m_running = false;
        emit finished();
        return;
    }
    pump();
}

void HikResolver::cancel()
{
    m_running = false;
    ++m_gen;
}

void HikResolver::pump()
{
    while (m_running && m_active < m_conc && m_next < m_requests.size()) {
        ++m_active;
        runJob(m_next++);
    }
}

void HikResolver::jobDone(int idx, const HikResult &r, int gen)
{
    if (gen != m_gen)
        return;
    --m_active;
    ++m_done;
    emit resolved(idx, r);
    emit progress(m_done, int(m_requests.size()));
    if (m_done >= m_requests.size()) {
        m_running = false;
        emit finished();
    } else {
        pump();
    }
}

void HikResolver::runJob(int idx)
{
    const HikRequest req = m_requests.at(idx);
    const int gen = m_gen;

    auto fail = [this, idx, gen](const QString &msg, bool auth = false) {
        HikResult r;
        r.error = msg;
        r.authFailed = auth;
        jobDone(idx, r, gen);
    };

    // 1. Тип устройства и модель
    httpGet(m_nam, this, req, QStringLiteral("/ISAPI/System/deviceInfo"), [=](const HttpReply &r1) {
        if (gen != m_gen)
            return;
        if (!r1.ok) {
            fail(r1.error, r1.authFailed);
            return;
        }
        HikDevice dev;
        dev.address = req.address;
        dev.httpPort = req.httpPort;
        parseDeviceInfo(r1.body, &dev);
        if (dev.model.isEmpty() && dev.name.isEmpty()) {
            fail(T("Ответ не похож на Hikvision/HiWatch"));
            return;
        }

        // 2. Каналы (у регистратора — все, у камеры — один)
        httpGet(m_nam, this, req, QStringLiteral("/ISAPI/Streaming/channels"), [=](const HttpReply &r2) {
            if (gen != m_gen)
                return;
            HikDevice d = dev;
            if (r2.ok)
                d.channels = parseChannels(r2.body);
            if (d.channels.isEmpty()) {
                const int n = d.isRecorder() ? req.fallbackChannels : 1;
                for (int i = 1; i <= n; ++i) {
                    HikChannel c;
                    c.number = i;
                    d.channels.append(c);
                }
            }
            if (d.channels.isEmpty()) {
                fail(T("Не удалось получить каналы регистратора: укажите их число вручную"));
                return;
            }
            HikResult res;
            res.ok = true;
            res.device = d;
            jobDone(idx, res, gen);
        });
    });
}

QString guessVendor(const QString &model, const QString &name)
{
    const QString s = (model + QLatin1Char(' ') + name).trimmed();
    if (s.isEmpty())
        return QString();

    static const QRegularExpression polyvision(QStringLiteral("POLY\\s*VISION|\\bPV[CDRN]-"),
                                               QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hiwatch(QStringLiteral("HIWATCH|\\bHW[ICPDN]-|\\bDS-[ITNHD]\\d"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hikvision(QStringLiteral("HIKVISION|\\bI?DS-"),
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression dahua(QStringLiteral("DAHUA|\\bDH-|\\bIPC-"),
                                          QRegularExpression::CaseInsensitiveOption);
    if (polyvision.match(s).hasMatch())
        return QStringLiteral("Polyvision");
    if (hiwatch.match(s).hasMatch())
        return QStringLiteral("HiWatch");
    if (hikvision.match(s).hasMatch())
        return QStringLiteral("Hikvision");
    if (dahua.match(s).hasMatch())
        return QStringLiteral("Dahua");
    return QString();
}

// ======================================================================
// core/i18n.cpp
// ======================================================================
namespace {

I18n::Lang g_lang = I18n::Lang::Ru;
bool g_restartRequested = false;

// Русский текст (ключ) -> английский перевод.
// Список строится из вызовов T("...") в коде; нового текста здесь нет — добавьте строку, чтобы перевести.
const QHash<QString, QString> &englishDict()
{
    static const QHash<QString, QString> dict = {
        {QStringLiteral("    %1 МБ"), QStringLiteral("    %1 MB")},
        {QStringLiteral(" (исправьте логин/пароль и повторите)"), QStringLiteral(" (fix the login/password and try again)")},
        {QStringLiteral(" — исправьте логин/пароль и повторите"), QStringLiteral(" — fix the login/password and try again")},
        {QStringLiteral("%1 — %2, каналов: %3"), QStringLiteral("%1 — %2, channels: %3")},
        {QStringLiteral("1. Адреса камер"), QStringLiteral("1. Camera addresses")},
        {QStringLiteral("1. Поиск камер"), QStringLiteral("1. Find cameras")},
        {QStringLiteral("1. Поиск устройств"), QStringLiteral("1. Find devices")},
        {QStringLiteral("2. Доступ"), QStringLiteral("2. Access")},
        {QStringLiteral("2. Логин и пароль камер"), QStringLiteral("2. Camera login and password")},
        {QStringLiteral("2. Общие логин и пароль камер"), QStringLiteral("2. Shared camera login and password")},
        {QStringLiteral("3. Найденные адреса с открытым RTSP-портом"), QStringLiteral("3. Addresses with an open RTSP port")},
        {QStringLiteral("3. Найденные устройства"), QStringLiteral("3. Devices found")},
        {QStringLiteral("HTTP-порт"), QStringLiteral("HTTP port")},
        {QStringLiteral("MiniVMS — вход"), QStringLiteral("MiniVMS — sign in")},
        {QStringLiteral("MiniVMS — создание пароля"), QStringLiteral("MiniVMS — create password")},
        {QStringLiteral("PTZ-управление"), QStringLiteral("PTZ control")},
        {QStringLiteral("RTSP-порт"), QStringLiteral("RTSP port")},
        {QStringLiteral("RTSP-порт камеры, по умолчанию 554"), QStringLiteral("Camera RTSP port, 554 by default")},
        {QStringLiteral("RTSP-порт открыт"), QStringLiteral("RTSP port is open")},
        {QStringLiteral("rtsp://логин:пароль@192.168.1.64:554/Streaming/Channels/101"), QStringLiteral("rtsp://login:password@192.168.1.64:554/Streaming/Channels/101")},
        {QStringLiteral("Адрес"), QStringLiteral("Address")},
        {QStringLiteral("Архив"), QStringLiteral("Archive")},
        {QStringLiteral("Без группы"), QStringLiteral("No group")},
        {QStringLiteral("В потоке нет видео"), QStringLiteral("The stream has no video")},
        {QStringLiteral("В раскладке 1 и в развёрнутой ячейке всегда основной поток"), QStringLiteral("The 1×1 layout and a maximized cell always use the main stream")},
        {QStringLiteral("Введите пароль для входа в программу."), QStringLiteral("Enter the password to open the program.")},
        {QStringLiteral("Вести непрерывную запись в архив"), QStringLiteral("Record continuously to the archive")},
        {QStringLiteral("Во весь экран"), QStringLiteral("Full screen")},
        {QStringLiteral("Войти"), QStringLiteral("Sign in")},
        {QStringLiteral("Время"), QStringLiteral("Time")},
        {QStringLiteral("Выбрать все"), QStringLiteral("Select all")},
        {QStringLiteral("Выбрать поток…"), QStringLiteral("Choose stream…")},
        {QStringLiteral("Выход"), QStringLiteral("Exit")},
        {QStringLiteral(". Возможные причины: служба выключена, порт изменён, устройство недоступно или соединение блокирует firewall/маршрутизатор."), QStringLiteral(". Possible causes: the service is disabled, the port has changed, the device is unavailable, or the connection is blocked by a firewall/router.")},
        {QStringLiteral("rtsp://логин:пароль@192.168.1.64:554/Streaming/Channels/101"), QStringLiteral("rtsp://username:password@192.168.1.64:554/Streaming/Channels/101")},
        {QStringLiteral("UDP (проверка отправки)"), QStringLiteral("UDP (send test only)")},
        {QStringLiteral("Универсальные / ONVIF"), QStringLiteral("Generic / ONVIF")},
        {QStringLiteral("Hikvision / HiWatch"), QStringLiteral("Hikvision / HiWatch")},
        {QStringLiteral("Dahua"), QStringLiteral("Dahua")},
        {QStringLiteral("Uniview"), QStringLiteral("Uniview")},
        {QStringLiteral("Другой производитель"), QStringLiteral("Other manufacturer")},
        {QStringLiteral("ЗАГРУЗКА СИСТЕМЫ  •  ИНИЦИАЛИЗАЦИЯ МОДУЛЕЙ"), QStringLiteral("SYSTEM STARTUP  •  INITIALIZING MODULES")},
        {QStringLiteral("CPU: %1%"), QStringLiteral("CPU: %1%")},
        {QStringLiteral("CPU: недоступно"), QStringLiteral("CPU: unavailable")},
        {QStringLiteral("Pro версия"), QStringLiteral("Pro version")},
        {QStringLiteral("Демо-версия"), QStringLiteral("Demo version")},
        {QStringLiteral("вход"), QStringLiteral("sign in")},
        {QStringLiteral("Обнаружение в сети"), QStringLiteral("Network discovery")},
        {QStringLiteral("События"), QStringLiteral("Events")},
        {QStringLiteral("Администратор"), QStringLiteral("Administrator")},
        {QStringLiteral("Translation unavailable"), QStringLiteral("Translation unavailable")},
        {QStringLiteral("Вести непрерывную запись в архив"), QStringLiteral("Record continuously to local archive")},
        {QStringLiteral("Камеру вручную (с подробными настройками)…"), QStringLiteral("Add camera manually…")},
        {QStringLiteral("Быстрое добавление"), QStringLiteral("Quick add")},
        {QStringLiteral("Готово к добавлению"), QStringLiteral("Ready to add")},
        {QStringLiteral("Группа"), QStringLiteral("Group")},
        {QStringLiteral("Группа:"), QStringLiteral("Group:")},
        {QStringLiteral("Переименовать группу…"), QStringLiteral("Rename group…")},
        {QStringLiteral("Переименовать группу"), QStringLiteral("Rename group")},
        {QStringLiteral("Новое название группы «%1»:"), QStringLiteral("New name for group “%1”:")},
        {QStringLiteral("Переименовать камеру…"), QStringLiteral("Rename camera…")},
        {QStringLiteral("Переименовать камеру"), QStringLiteral("Rename camera")},
        {QStringLiteral("Новое название камеры:"), QStringLiteral("New camera name:")},
        {QStringLiteral("Удалить эту камеру / канал регистратора…"), QStringLiteral("Delete this camera / recorder channel…")},
        {QStringLiteral("Удаление устройства"), QStringLiteral("Delete device")},
        {QStringLiteral("Удалить «%1» из PSVMS?\nЗапись будет остановлена, устройство исчезнет из списка. Уже сохранённые файлы архива останутся на диске."), QStringLiteral("Remove “%1” from PSVMS?\nRecording will stop and the device will be removed from the list. Existing archive files will remain on disk.")},
        {QStringLiteral("Диапазон IP:"), QStringLiteral("IP range:")},
        {QStringLiteral("Для этого адреса шаблон потоков неизвестен: введите адрес вручную"), QStringLiteral("No stream template is known for this address: enter the address manually")},
        {QStringLiteral("Добавить"), QStringLiteral("Add")},
        {QStringLiteral("Добавить Hikvision / HiWatch"), QStringLiteral("Add Hikvision / HiWatch")},
        {QStringLiteral("Добавить Hikvision / HiWatch…"), QStringLiteral("Add Hikvision / HiWatch…")},
        {QStringLiteral("Добавить iCSee…"), QStringLiteral("Add iCSee…")},
        {QStringLiteral("Добавить выбранные"), QStringLiteral("Add selected")},
        {QStringLiteral("Добавить камеру"), QStringLiteral("Add camera")},
        {QStringLiteral("Добавить камеры iCSee"), QStringLiteral("Add iCSee cameras")},
        {QStringLiteral("Добавлена"), QStringLiteral("Added")},
        {QStringLiteral("Добавлено"), QStringLiteral("Added")},
        {QStringLiteral("Добавлено камер: %1, с ошибкой: %2%3"), QStringLiteral("Cameras added: %1, failed: %2%3")},
        {QStringLiteral("Добавлено камер: %1. Состояние записи смотрите на вкладке «Устройства» и в «Журнале»."), QStringLiteral("Cameras added: %1. Check the recording status on the “Devices” tab and in “Events”.")},
        {QStringLiteral("Добавлено каналов: %1 (устройств: %2), с ошибкой: %3%4"), QStringLiteral("Channels added: %1 (devices: %2), failed: %3%4")},
        {QStringLiteral("Закрыть"), QStringLiteral("Close")},
        {QStringLiteral("Закрыть все"), QStringLiteral("Close all")},
        {QStringLiteral("Закрыть камеру"), QStringLiteral("Close camera")},
        {QStringLiteral("Записи за день (двойной щелчок — воспроизвести):"), QStringLiteral("Recordings for the day (double-click to play):")},
        {QStringLiteral("Запись выключена"), QStringLiteral("Recording off")},
        {QStringLiteral("Запись идёт"), QStringLiteral("Recording")},
        {QStringLiteral("Запрос…"), QStringLiteral("Requesting…")},
        {QStringLiteral("Зум"), QStringLiteral("Zoom")},
        {QStringLiteral("Изменить"), QStringLiteral("Edit")},
        {QStringLiteral("Изменить камеру"), QStringLiteral("Edit camera")},
        {QStringLiteral("Изменить камеру…"), QStringLiteral("Edit camera…")},
        {QStringLiteral("Импорт ONVIF…"), QStringLiteral("Import ONVIF…")},
        {QStringLiteral("Импорт камер по ONVIF"), QStringLiteral("Import cameras via ONVIF")},
        {QStringLiteral("Имя / модель"), QStringLiteral("Name / model")},
        {QStringLiteral("КАМЕРЫ"), QStringLiteral("CAMERAS")},
        {QStringLiteral("Как у записи (без подпотока)"), QStringLiteral("Same as recording (no substream)")},
        {QStringLiteral("Камер: %1    Ведётся запись: %2    Архив: %3"), QStringLiteral("Cameras: %1    Recording: %2    Archive: %3")},
        {QStringLiteral("Камера"), QStringLiteral("Camera")},
        {QStringLiteral("Камера не вернула RTSP-адрес"), QStringLiteral("The camera did not return an RTSP address")},
        {QStringLiteral("Камера не вернула адрес media-сервиса (проверьте логин и пароль)"), QStringLiteral("The camera did not return a media service address (check the login and password)")},
        {QStringLiteral("Камера не отвечает (таймаут)"), QStringLiteral("The camera is not responding (timeout)")},
        {QStringLiteral("Камеры должны быть в вашей сети (по IP). Облачное подключение по серийному номеру не используется."), QStringLiteral("Cameras must be on your network (by IP). Cloud connection by serial number is not used.")},
        {QStringLiteral("Канал %1"), QStringLiteral("Channel %1")},
        {QStringLiteral("Каналов в регистраторе:"), QStringLiteral("Recorder channels:")},
        {QStringLiteral("Логин:"), QStringLiteral("Login:")},
        {QStringLiteral("Модель / тип"), QStringLiteral("Model / type")},
        {QStringLiteral("Мониторинг"), QStringLiteral("Monitoring")},
        {QStringLiteral("Нажмите «Найти в своей сети» или задайте диапазон IP."), QStringLiteral("Click “Find on my network” or set an IP range.")},
        {QStringLiteral("Название"), QStringLiteral("Name")},
        {QStringLiteral("Найдена"), QStringLiteral("Found")},
        {QStringLiteral("Найдено"), QStringLiteral("Found")},
        {QStringLiteral("Найдите устройства, введите логин и пароль и нажмите «Добавить выбранные»."), QStringLiteral("Find devices, enter the login and password, then click “Add selected”.")},
        {QStringLiteral("Найти в своей сети"), QStringLiteral("Find on my network")},
        {QStringLiteral("Найти в сети (SADP)"), QStringLiteral("Find on network (SADP)")},
        {QStringLiteral("Найти камеры"), QStringLiteral("Find cameras")},
        {QStringLiteral("Не отвечает (таймаут)"), QStringLiteral("Not responding (timeout)")},
        {QStringLiteral("Не удалось открыть БД: %1"), QStringLiteral("Could not open the database: %1")},
        {QStringLiteral("Не удалось подключиться: %1"), QStringLiteral("Could not connect: %1")},
        {QStringLiteral("Не удалось получить каналы регистратора: укажите их число вручную"), QStringLiteral("Could not get the recorder channels: enter their number manually")},
        {QStringLiteral("Не удалось получить параметры потока: %1"), QStringLiteral("Could not get stream parameters: %1")},
        {QStringLiteral("Не удалось создать сегмент: %1"), QStringLiteral("Could not create a segment: %1")},
        {QStringLiteral("Не удалось сохранить пароль."), QStringLiteral("Could not save the password.")},
        {QStringLiteral("Не хватает памяти"), QStringLiteral("Out of memory")},
        {QStringLiteral("Неверный логин или пароль"), QStringLiteral("Wrong login or password")},
        {QStringLiteral("Неверный логин или пароль (или устройство не активировано)"), QStringLiteral("Wrong login or password (or the device is not activated)")},
        {QStringLiteral("Неверный пароль."), QStringLiteral("Wrong password.")},
        {QStringLiteral("Неверный пароль. Вход временно заблокирован."), QStringLiteral("Wrong password. Sign-in is temporarily blocked.")},
        {QStringLiteral("Нет связи, переподключение…"), QStringLiteral("No connection, reconnecting…")},
        {QStringLiteral("Новые пароли не совпадают."), QStringLiteral("The new passwords do not match.")},
        {QStringLiteral("Новый пароль"), QStringLiteral("New password")},
        {QStringLiteral("Новый пароль слишком короткий: минимум %1 символов."), QStringLiteral("The new password is too short: at least %1 characters.")},
        {QStringLiteral("ОК"), QStringLiteral("OK")},
        {QStringLiteral("Обновить"), QStringLiteral("Refresh")},
        {QStringLiteral("Опрос камер: %1 из %2"), QStringLiteral("Querying cameras: %1 of %2")},
        {QStringLiteral("Опрос устройств: %1 из %2"), QStringLiteral("Querying devices: %1 of %2")},
        {QStringLiteral("Основной поток"), QStringLiteral("Main stream")},
        {QStringLiteral("Остановлено"), QStringLiteral("Stopped")},
        {QStringLiteral("Ответ не похож на Hikvision/HiWatch"), QStringLiteral("The response does not look like Hikvision/HiWatch")},
        {QStringLiteral("Отмена"), QStringLiteral("Cancel")},
        {QStringLiteral("Отметьте камеры для добавления."), QStringLiteral("Select the cameras to add.")},
        {QStringLiteral("Отметьте устройства для добавления."), QStringLiteral("Select the devices to add.")},
        {QStringLiteral("Очистить"), QStringLiteral("Clear")},
        {QStringLiteral("Ошибка записи (диск?): %1"), QStringLiteral("Write error (disk?): %1")},
        {QStringLiteral("Ошибка индекса: %1"), QStringLiteral("Index error: %1")},
        {QStringLiteral("Пароли не совпадают."), QStringLiteral("Passwords do not match.")},
        {QStringLiteral("Пароль"), QStringLiteral("Password")},
        {QStringLiteral("Пароль изменён."), QStringLiteral("Password changed.")},
        {QStringLiteral("Пароль слишком короткий: минимум %1 символов."), QStringLiteral("The password is too short: at least %1 characters.")},
        {QStringLiteral("Пароль:"), QStringLiteral("Password:")},
        {QStringLiteral("Первый запуск. Придумайте пароль для входа в программу (не короче 8 символов). Восстановить пароль невозможно: храните его в надёжном месте."), QStringLiteral("First launch. Create a password to sign in to the program (at least 8 characters). It cannot be recovered, so keep it somewhere safe.")},
        {QStringLiteral("Перетащите камеру сюда\nили дважды щёлкните её в списке"), QStringLiteral("Drag a camera here\nor double-click it in the list")},
        {QStringLiteral("Повторите новый"), QStringLiteral("Repeat new")},
        {QStringLiteral("Повторите пароль"), QStringLiteral("Repeat password")},
        {QStringLiteral("Подключение…"), QStringLiteral("Connecting…")},
        {QStringLiteral("Подпоток"), QStringLiteral("Substream")},
        {QStringLiteral("Подпоток для мозаики"), QStringLiteral("Substream for the grid")},
        {QStringLiteral("Поиск завершён. Найдено адресов: %1"), QStringLiteral("Search finished. Addresses found: %1")},
        {QStringLiteral("Поиск завершён. Найдено устройств: %1"), QStringLiteral("Search finished. Devices found: %1")},
        {QStringLiteral("Поиск завершён. Найдено устройств: %1. Если список пуст, используйте сканирование диапазона."), QStringLiteral("Search finished. Devices found: %1. If the list is empty, scan an IP range.")},
        {QStringLiteral("Поиск камер в сети…"), QStringLiteral("Looking for cameras on the network…")},
        {QStringLiteral("Поиск камеры…"), QStringLiteral("Search camera…")},
        {QStringLiteral("Поиск устройств в сети…"), QStringLiteral("Looking for devices on the network…")},
        {QStringLiteral("Показать или скрыть список камер"), QStringLiteral("Show or hide the camera list")},
        {QStringLiteral("Показывать основной поток вместо подпотока: выше качество, больше нагрузка"), QStringLiteral("Show the main stream instead of the substream: higher quality, more load")},
        {QStringLiteral("Получен 1 поток"), QStringLiteral("1 stream received")},
        {QStringLiteral("Получены основной поток и подпоток"), QStringLiteral("Main stream and substream received")},
        {QStringLiteral("Порт ONVIF: обычно 80; у камер Xiongmai/iCSee — 8899"), QStringLiteral("ONVIF port: usually 80; 8899 for Xiongmai/iCSee cameras")},
        {QStringLiteral("Порт закрыт"), QStringLiteral("Port closed")},
        {QStringLiteral("Поток для мозаики"), QStringLiteral("Stream for the grid")},
        {QStringLiteral("Поток записи"), QStringLiteral("Recording stream")},
        {QStringLiteral("Поток прерван: %1"), QStringLiteral("Stream interrupted: %1")},
        {QStringLiteral("Проверено %1 из %2, найдено: %3"), QStringLiteral("Checked %1 of %2, found: %3")},
        {QStringLiteral("Раскладка экрана"), QStringLiteral("Screen layout")},
        {QStringLiteral("Свернуть или развернуть меню"), QStringLiteral("Collapse or expand the menu")},
        {QStringLiteral("Сканирование завершено. Найдено адресов: %1"), QStringLiteral("Scan finished. Addresses found: %1")},
        {QStringLiteral("Сканирование завершено. Найдено устройств: %1"), QStringLiteral("Scan finished. Devices found: %1")},
        {QStringLiteral("Сканирование: %1 из %2, найдено: %3"), QStringLiteral("Scanning: %1 of %2, found: %3")},
        {QStringLiteral("Сканировать диапазон"), QStringLiteral("Scan range")},
        {QStringLiteral("Скорость"), QStringLiteral("Speed")},
        {QStringLiteral("Слишком большой диапазон: максимум %1 адресов за раз."), QStringLiteral("The range is too large: at most %1 addresses at a time.")},
        {QStringLiteral("Слишком много попыток. Подождите %1 с."), QStringLiteral("Too many attempts. Wait %1 s.")},
        {QStringLiteral("Смена пароля"), QStringLiteral("Change password")},
        {QStringLiteral("Сменить пароль…"), QStringLiteral("Change password…")},
        {QStringLiteral("Снять выбор"), QStringLiteral("Clear selection")},
        {QStringLiteral("Событие"), QStringLiteral("Event")},
        {QStringLiteral("События"), QStringLiteral("Events")},
        {QStringLiteral("Создать"), QStringLiteral("Create")},
        {QStringLiteral("Состояние"), QStringLiteral("Status")},
        {QStringLiteral("Текст"), QStringLiteral("Text")},
        {QStringLiteral("Текущий пароль"), QStringLiteral("Current password")},
        {QStringLiteral("Текущий пароль указан неверно."), QStringLiteral("The current password is wrong.")},
        {QStringLiteral("Текущий подпоток"), QStringLiteral("Current substream")},
        {QStringLiteral("Третий поток"), QStringLiteral("Third stream")},
        {QStringLiteral("У камеры нет видеопрофилей"), QStringLiteral("The camera has no video profiles")},
        {QStringLiteral("Удаление камеры"), QStringLiteral("Delete camera")},
        {QStringLiteral("Удалить"), QStringLiteral("Delete")},
        {QStringLiteral("Удалить камеру «%1»?\nУже записанные файлы архива останутся на диске."), QStringLiteral("Delete the camera “%1”?\nFiles already recorded to the archive will stay on disk.")},
        {QStringLiteral("Уже добавлена"), QStringLiteral("Already added")},
        {QStringLiteral("Уже добавлено"), QStringLiteral("Already added")},
        {QStringLiteral("Укажите корректный диапазон IPv4-адресов."), QStringLiteral("Enter a valid IPv4 address range.")},
        {QStringLiteral("Укажите логин камер."), QStringLiteral("Enter the camera login.")},
        {QStringLiteral("Укажите логин."), QStringLiteral("Enter the login.")},
        {QStringLiteral("Укажите название и корректный адрес потока, например rtsp://…"), QStringLiteral("Enter a name and a valid stream address, for example rtsp://…")},
        {QStringLiteral("Управление по ONVIF будет добавлено позже"), QStringLiteral("ONVIF control will be added later")},
        {QStringLiteral("Устройства"), QStringLiteral("Devices")},
        {QStringLiteral("Число каналов регистратора, если автоопределение не сработало"), QStringLiteral("Recorder channel count, if auto-detection failed")},
        {QStringLiteral("Это не Hikvision/HiWatch (нет ISAPI)"), QStringLiteral("This is not Hikvision/HiWatch (no ISAPI)")},
        {QStringLiteral("Язык интерфейса"), QStringLiteral("Interface language")},
        {QStringLiteral("Язык: "), QStringLiteral("Language: ")},
        {QStringLiteral("авто"), QStringLiteral("auto")},
        {QStringLiteral("для регистратора — по умолчанию его имя"), QStringLiteral("for a recorder, its name by default")},
        {QStringLiteral("идёт запись"), QStringLiteral("recording")},
        {QStringLiteral("камера"), QStringLiteral("camera")},
        {QStringLiteral("например, Двор"), QStringLiteral("for example, Yard")},
        {QStringLiteral("например, Склад"), QStringLiteral("for example, Warehouse")},
        {QStringLiteral("необязательно: поток меньшего разрешения для мозаики"), QStringLiteral("optional: a lower-resolution stream for the grid")},
        {QStringLiteral("порт"), QStringLiteral("port")},
        {QStringLiteral("регистратор"), QStringLiteral("recorder")},
        {QStringLiteral("у новых камер iCSee часто пустой"), QStringLiteral("new iCSee cameras often have an empty one")},
        {QStringLiteral("☰ Камеры"), QStringLiteral("☰ Cameras")},
        {QStringLiteral("2. Общие логин и пароль"), QStringLiteral("2. Shared login and password")},
        {QStringLiteral("Esc или двойной щелчок — выйти"), QStringLiteral("Esc or double-click to exit")},
        {QStringLiteral("Hik-Connect — облачный сервис Hikvision. Подключение к нему из сторонних программ возможно только через официальный SDK и ключи разработчика Hikvision (Open Platform), поэтому в этой версии получение камер из облака не работает."), QStringLiteral("Hik-Connect is Hikvision’s cloud service. Third-party programs can connect to it only through the official SDK and Hikvision Open Platform developer keys, so retrieving cameras from the cloud does not work in this version.")},
        {QStringLiteral("Hikvision / HiWatch по IP…"), QStringLiteral("Hikvision / HiWatch by IP…")},
        {QStringLiteral("IP-адрес"), QStringLiteral("IP address")},
        {QStringLiteral("e-mail или телефон"), QStringLiteral("e-mail or phone")},
        {QStringLiteral("Авто"), QStringLiteral("Auto")},
        {QStringLiteral("Авто (подпоток в мозаике)"), QStringLiteral("Auto (substream in the grid)")},
        {QStringLiteral("Авто: подпоток в мозаике, основной в раскладке 1 × 1"), QStringLiteral("Auto: substream in the grid, main stream in the 1 × 1 layout")},
        {QStringLiteral("Администратор"), QStringLiteral("Administrator")},
        {QStringLiteral("Администратор (все разделы)"), QStringLiteral("Administrator (all sections)")},
        {QStringLiteral("Азия"), QStringLiteral("Asia")},
        {QStringLiteral("Аккаунт"), QStringLiteral("Account")},
        {QStringLiteral("Америка"), QStringLiteral("Americas")},
        {QStringLiteral("Архив записей выбранной камеры"), QStringLiteral("Recording archive of the selected camera")},
        {QStringLiteral("Архив камеры…"), QStringLiteral("Camera archive…")},
        {QStringLiteral("Архив…"), QStringLiteral("Archive…")},
        {QStringLiteral("В резервную копию входят камеры, настройки программы, шаблоны и расположение камер в окне. Пользователи и пароли входа в копию не попадают.\n\nВнимание: адреса камер содержат их пароли. Храните файл копии в безопасном месте."), QStringLiteral("The backup includes cameras, program settings, templates and the camera layout in the window. Users and sign-in passwords are not included.\n\nWarning: camera addresses contain their passwords. Keep the backup file in a safe place.")},
        {QStringLiteral("В сети"), QStringLiteral("Online")},
        {QStringLiteral("Версия %1"), QStringLiteral("Version %1")},
        {QStringLiteral("Видео и папки"), QStringLiteral("Video and folders")},
        {QStringLiteral("Включать звук при показе"), QStringLiteral("Turn sound on when shown")},
        {QStringLiteral("Включить или выключить звук"), QStringLiteral("Turn sound on or off")},
        {QStringLiteral("Восстановить"), QStringLiteral("Restore")},
        {QStringLiteral("Восстановить из копии"), QStringLiteral("Restore from backup")},
        {QStringLiteral("Восстановить из копии…"), QStringLiteral("Restore from backup…")},
        {QStringLiteral("Восстановление"), QStringLiteral("Restore")},
        {QStringLiteral("Все производители"), QStringLiteral("All manufacturers")},
        {QStringLiteral("Всего камер"), QStringLiteral("Total cameras")},
        {QStringLiteral("Выберите папку"), QStringLiteral("Choose a folder")},
        {QStringLiteral("Выберите пользователя и введите пароль."), QStringLiteral("Choose a user and enter the password.")},
        {QStringLiteral("Выйти из учётной записи"), QStringLiteral("Sign out")},
        {QStringLiteral("Высокое (основной поток)"), QStringLiteral("High (main stream)")},
        {QStringLiteral("Высокое: всегда основной поток"), QStringLiteral("High: always the main stream")},
        {QStringLiteral("Дата и время на картинке"), QStringLiteral("Date and time on the picture")},
        {QStringLiteral("Для выделенных камер; если ничего не выделено — для всех"), QStringLiteral("For the selected cameras; if nothing is selected, for all")},
        {QStringLiteral("Добавить Hikvision / HiWatch по IP"), QStringLiteral("Add Hikvision / HiWatch by IP")},
        {QStringLiteral("Добавить новую камеру…"), QStringLiteral("Add a new camera…")},
        {QStringLiteral("Добавить пользователя…"), QStringLiteral("Add user…")},
        {QStringLiteral("Добавление отменено: нужен пароль."), QStringLiteral("Adding canceled: a password is required.")},
        {QStringLiteral("Добавлено каналов: %1 (%2). Способ определения: %3"), QStringLiteral("Channels added: %1 (%2). Detection method: %3")},
        {QStringLiteral("Добавлять камеры может только администратор"), QStringLiteral("Only an administrator can add cameras")},
        {QStringLiteral("Другой регион"), QStringLiteral("Other region")},
        {QStringLiteral("Европа"), QStringLiteral("Europe")},
        {QStringLiteral("Загрузка…"), QStringLiteral("Loading…")},
        {QStringLiteral("Записи ведутся сегментами по 5 минут. При смене папки новые записи пишутся в неё, старые остаются на прежнем месте. Скриншот делается из меню камеры (правая кнопка мыши)."), QStringLiteral("Recordings are made in 5-minute segments. When you change the folder, new recordings go to it and old ones stay where they were. Take a screenshot from the camera menu (right mouse button).")},
        {QStringLiteral("Запись"), QStringLiteral("Recording")},
        {QStringLiteral("Запомнить меня на этом компьютере"), QStringLiteral("Remember me on this computer")},
        {QStringLiteral("Запускать камеры автоматически при запуске программы"), QStringLiteral("Start cameras automatically when the program starts")},
        {QStringLiteral("Запустить запись"), QStringLiteral("Start recording")},
        {QStringLiteral("Запустить камеру"), QStringLiteral("Start camera")},
        {QStringLiteral("Заполнить всю ячейку (обрезать края)"), QStringLiteral("Fill the entire cell (crop edges)")},
        {QStringLiteral("Заполнить ячейки всей группы (обрезать края)"), QStringLiteral("Fill cells for the entire group (crop edges)")},
        {QStringLiteral("Применить заполнение ко всем камерам"), QStringLiteral("Apply fill mode to all cameras")},
        {QStringLiteral("Отключить заполнение у всех камер"), QStringLiteral("Disable fill mode for all cameras")},
        {QStringLiteral("Отключить заполнение для группы"), QStringLiteral("Disable fill mode for this group")},
        {QStringLiteral("Запустить показ"), QStringLiteral("Start viewing")},
        {QStringLiteral("Запустить показ камер в ячейках и запись"), QStringLiteral("Start showing cameras in the cells and recording")},
        {QStringLiteral("Звук"), QStringLiteral("Sound")},
        {QStringLiteral("Идёт запись"), QStringLiteral("Recording now")},
        {QStringLiteral("Импорт по ONVIF…"), QStringLiteral("Import via ONVIF…")},
        {QStringLiteral("Имя"), QStringLiteral("Name")},
        {QStringLiteral("Камер пока нет"), QStringLiteral("No cameras yet")},
        {QStringLiteral("Камеру вручную…"), QStringLiteral("Camera manually…")},
        {QStringLiteral("Камеры iCSee…"), QStringLiteral("iCSee cameras…")},
        {QStringLiteral("Каналов в регистраторе"), QStringLiteral("Recorder channels")},
        {QStringLiteral("Качество картинки"), QStringLiteral("Picture quality")},
        {QStringLiteral("Логин"), QStringLiteral("Login")},
        {QStringLiteral("Масштаб интерфейса (после перезапуска)"), QStringLiteral("Interface scale (after restart)")},
        {QStringLiteral("Модель / имя"), QStringLiteral("Model / name")},
        {QStringLiteral("На весь экран"), QStringLiteral("Full screen")},
        {QStringLiteral("Нажмите ▶, чтобы запустить"), QStringLiteral("Press ▶ to start")},
        {QStringLiteral("Название шаблона:"), QStringLiteral("Template name:")},
        {QStringLiteral("Настройки"), QStringLiteral("Settings")},
        {QStringLiteral("Настройки камеры"), QStringLiteral("Camera settings")},
        {QStringLiteral("Настройки камеры…"), QStringLiteral("Camera settings…")},
        {QStringLiteral("Не в сети"), QStringLiteral("Offline")},
        {QStringLiteral("Не запущена"), QStringLiteral("Not started")},
        {QStringLiteral("Не удалось добавить: %1"), QStringLiteral("Could not add: %1")},
        {QStringLiteral("Не удалось записать файл: %1"), QStringLiteral("Could not write the file: %1")},
        {QStringLiteral("Не удалось определить адрес потока камеры"), QStringLiteral("Could not determine the camera stream address")},
        {QStringLiteral("Не удалось прочитать файл: %1"), QStringLiteral("Could not read the file: %1")},
        {QStringLiteral("Не удалось сохранить снимок: %1"), QStringLiteral("Could not save the screenshot: %1")},
        {QStringLiteral("Нельзя удалить единственного пользователя."), QStringLiteral("The only user cannot be deleted.")},
        {QStringLiteral("Нельзя удалить пользователя, под которым выполнен вход."), QStringLiteral("You cannot delete the user you are signed in as.")},
        {QStringLiteral("Нельзя удалить последнего администратора."), QStringLiteral("The last administrator cannot be deleted.")},
        {QStringLiteral("Нет кадра для снимка"), QStringLiteral("No frame to capture")},
        {QStringLiteral("Нет видеопотока"), QStringLiteral("No video stream")},
        {QStringLiteral("Низкое (подпоток, экономия)"), QStringLiteral("Low (substream, saves resources)")},
        {QStringLiteral("Низкое: всегда подпоток (экономия ресурсов)"), QStringLiteral("Low: always the substream (saves resources)")},
        {QStringLiteral("Новый пароль для пользователя «%1». Оставьте пустым, чтобы убрать пароль."), QStringLiteral("New password for the user “%1”. Leave empty to remove the password.")},
        {QStringLiteral("Новый пользователь"), QStringLiteral("New user")},
        {QStringLiteral("Новый шаблон"), QStringLiteral("New template")},
        {QStringLiteral("Нужен пароль"), QStringLiteral("Password required")},
        {QStringLiteral("О программе"), QStringLiteral("About")},
        {QStringLiteral("Обзор…"), QStringLiteral("Browse…")},
        {QStringLiteral("Облако Hik-Connect"), QStringLiteral("Hik-Connect cloud")},
        {QStringLiteral("Общие"), QStringLiteral("General")},
        {QStringLiteral("Оператор"), QStringLiteral("Operator")},
        {QStringLiteral("Оператор (только просмотр)"), QStringLiteral("Operator (view only)")},
        {QStringLiteral("Оставьте новый пароль пустым, чтобы входить без пароля."), QStringLiteral("Leave the new password empty to sign in without a password.")},
        {QStringLiteral("Остановить запись"), QStringLiteral("Stop recording")},
        {QStringLiteral("Остановить показ"), QStringLiteral("Stop viewing")},
        {QStringLiteral("Остановить показ в ячейках (запись продолжается)"), QStringLiteral("Stop viewing in the cells (recording continues)")},
        {QStringLiteral("Открывать программу на весь экран"), QStringLiteral("Open the program in full screen")},
        {QStringLiteral("Открыть"), QStringLiteral("Open")},
        {QStringLiteral("Папка для записей видео"), QStringLiteral("Video recordings folder")},
        {QStringLiteral("Папка для скриншотов"), QStringLiteral("Screenshots folder")},
        {QStringLiteral("Пароль не обязателен: оставьте поля пустыми, чтобы пользователь входил без пароля."), QStringLiteral("A password is optional: leave the fields empty so the user signs in without a password.")},
        {QStringLiteral("Пароль пользователя"), QStringLiteral("User password")},
        {QStringLiteral("Пароль слишком короткий (минимум %1)."), QStringLiteral("The password is too short (at least %1).")},
        {QStringLiteral("Пароль…"), QStringLiteral("Password…")},
        {QStringLiteral("Перетащите заголовок камеры на другую ячейку: камеры поменяются местами; положение сохранится автоматически"), QStringLiteral("Drag by the title to swap cameras")},
        {QStringLiteral("Перетащите камеру сюда\nили нажмите «+»"), QStringLiteral("Drag a camera here\nor press “+”")},
        {QStringLiteral("Перетащите на другую ячейку"), QStringLiteral("Drop onto another cell")},
        {QStringLiteral("Подключиться"), QStringLiteral("Connect")},
        {QStringLiteral("Поиск в сети: Hikvision, HiWatch, Polyvision…"), QStringLiteral("Network search: Hikvision, HiWatch, Polyvision…")},
        {QStringLiteral("Поиск камер в сети: Hikvision, HiWatch, Polyvision"), QStringLiteral("Camera search on the network: Hikvision, HiWatch, Polyvision")},
        {QStringLiteral("Показывать дату и время на камерах"), QStringLiteral("Show date and time on cameras")},
        {QStringLiteral("Показывать дату и время на картинке"), QStringLiteral("Show date and time on the picture")},
        {QStringLiteral("Пользователи"), QStringLiteral("Users")},
        {QStringLiteral("Пользователи выбираются при входе. Администратор видит все разделы, оператор — только «Мониторинг» и «События». Пароль не обязателен."), QStringLiteral("Users are chosen at sign-in. An administrator sees all sections, an operator only “Monitoring” and “Events”. A password is optional.")},
        {QStringLiteral("Пользователь"), QStringLiteral("User")},
        {QStringLiteral("Пользователь с таким именем уже есть."), QStringLiteral("A user with this name already exists.")},
        {QStringLiteral("Порты"), QStringLiteral("Ports")},
        {QStringLiteral("Сеть и порты"), QStringLiteral("Network and ports")},
        {QStringLiteral("ИИ-помощник"), QStringLiteral("AI assistant")},
        {QStringLiteral("Включить ИИ-помощника Yandex в программе"), QStringLiteral("Enable Yandex AI assistant in the application")},
        {QStringLiteral("Открыть ИИ-чат"), QStringLiteral("Open AI chat")},
        {QStringLiteral("Сохранить параметры"), QStringLiteral("Save settings")},
        {QStringLiteral("Проверить порт"), QStringLiteral("Test port")},
        {QStringLiteral("Подставить порт службы"), QStringLiteral("Use service port")},
        {QStringLiteral("Порт доступен"), QStringLiteral("PORT IS REACHABLE")},
        {QStringLiteral("Порт недоступен"), QStringLiteral("PORT IS UNREACHABLE")},
        {QStringLiteral("RTSP — видеопоток"), QStringLiteral("RTSP — video stream")},
        {QStringLiteral("HTTP — веб-интерфейс"), QStringLiteral("HTTP — web interface")},
        {QStringLiteral("HTTPS — защищённый веб-интерфейс"), QStringLiteral("HTTPS — secure web interface")},
        {QStringLiteral("Пользовательский порт"), QStringLiteral("Custom port")},
        {QStringLiteral("Предел кадров в секунду на ячейку"), QStringLiteral("Frame rate limit per cell")},
        {QStringLiteral("Применён шаблон «%1»"), QStringLiteral("Template “%1” applied")},
        {QStringLiteral("Проверить и добавить"), QStringLiteral("Check and add")},
        {QStringLiteral("Проверить сейчас"), QStringLiteral("Check now")},
        {QStringLiteral("Проверка устройства %1…"), QStringLiteral("Checking device %1…")},
        {QStringLiteral("Производитель"), QStringLiteral("Manufacturer")},
        {QStringLiteral("Производительность"), QStringLiteral("Performance")},
        {QStringLiteral("Пропустить"), QStringLiteral("Skip")},
        {QStringLiteral("Пропущено: нужен пароль"), QStringLiteral("Skipped: password required")},
        {QStringLiteral("Работает с новыми и старыми моделями камер и регистраторов Hikvision и HiWatch. Если устройство потребует другой логин или пароль, программа спросит."), QStringLiteral("Works with new and old models of Hikvision and HiWatch cameras and recorders. If the device needs a different login or password, the program will ask.")},
        {QStringLiteral("Размер окна"), QStringLiteral("Window size")},
        {QStringLiteral("Расположение камер сохраняется автоматически. Здесь можно сохранить именованные шаблоны."), QStringLiteral("The camera layout is saved automatically. Here you can save named templates.")},
        {QStringLiteral("Регион"), QStringLiteral("Region")},
        {QStringLiteral("Анимация интерфейса (заставка, Demo, значки и переходы)"), QStringLiteral("Interface animations (splash, Demo, icons and transitions)")},
        {QStringLiteral("Режим для слабого компьютера"), QStringLiteral("Low-power mode")},
        {QStringLiteral("Режим для слабого компьютера: не более 8 кадров в секунду, без сглаживания и анимаций, реже проверяется связь с камерами. Ещё сильнее нагрузку снижает показ подпотока (у камеры: «Качество картинки → Низкое») и меньшее число ячеек в раскладке."), QStringLiteral("Low-power mode: no more than 8 frames per second, no smoothing or animations, camera connectivity is checked less often. The load is reduced even more by showing the substream (camera: “Picture quality → Low”) and fewer cells in the layout.")},
        {QStringLiteral("Резервная копия"), QStringLiteral("Backup")},
        {QStringLiteral("Резервная копия сохранена:\n%1"), QStringLiteral("Backup saved:\n%1")},
        {QStringLiteral("Роль"), QStringLiteral("Role")},
        {QStringLiteral("Россия и СНГ"), QStringLiteral("Russia and CIS")},
        {QStringLiteral("Сбросить масштаб"), QStringLiteral("Reset zoom")},
        {QStringLiteral("Связь"), QStringLiteral("Connection")},
        {QStringLiteral("Сглаживание картинки"), QStringLiteral("Picture smoothing")},
        {QStringLiteral("Система видеонаблюдения"), QStringLiteral("Video surveillance system")},
        {QStringLiteral("Снимок сохранён: %1"), QStringLiteral("Screenshot saved: %1")},
        {QStringLiteral("Снимок экрана"), QStringLiteral("Screenshot")},
        {QStringLiteral("Создать резервную копию…"), QStringLiteral("Create backup…")},
        {QStringLiteral("Состояние камер"), QStringLiteral("Camera status")},
        {QStringLiteral("Состояние"), QStringLiteral("Status")},
        {QStringLiteral("Удалить группу…"), QStringLiteral("Delete group…")},
        {QStringLiteral("Удалить группу"), QStringLiteral("Delete group")},
        {QStringLiteral("Удалить группу «%1»…"), QStringLiteral("Delete group “%1”…")},
        {QStringLiteral("Разрешить управление PTZ (поворот и зум камер)"), QStringLiteral("Enable PTZ control (camera pan and zoom)")},
        {QStringLiteral("Подключение к камере"), QStringLiteral("Connecting to camera")},
        {QStringLiteral("Потеря связи — выполняется переподключение"), QStringLiteral("Connection lost — reconnecting")},
        {QStringLiteral("Камера отключена / поток остановлен"), QStringLiteral("Camera disconnected / stream stopped")},
        {QStringLiteral("ОШИБКА: %1"), QStringLiteral("ERROR: %1")},
        {QStringLiteral("Камера добавлена"), QStringLiteral("Camera added")},
        {QStringLiteral("Параметры камеры изменены"), QStringLiteral("Camera settings changed")},
        {QStringLiteral("Камера удалена из списка"), QStringLiteral("Camera removed from list")},
        {QStringLiteral("Показывать счётчики «В сети» и «Не в сети» и кнопку «Состояние камер»"), QStringLiteral("Show the Online and Offline counters and the Camera status button")},
        {QStringLiteral("Открыть подробное состояние всех камер"), QStringLiteral("Open detailed status for all cameras")},
        {QStringLiteral("Сохранить текущее расположение как шаблон…"), QStringLiteral("Save the current layout as a template…")},
        {QStringLiteral("Статус: не подключено"), QStringLiteral("Status: not connected")},
        {QStringLiteral("Статус: недоступно в этой версии (нужны ключи Hikvision Open Platform). Добавляйте камеры по IP в локальной сети."), QStringLiteral("Status: unavailable in this version (Hikvision Open Platform keys are required). Add cameras by IP on the local network.")},
        {QStringLiteral("Текущие камеры и настройки будут заменены данными из копии, программа перезапустится. Продолжить?"), QStringLiteral("The current cameras and settings will be replaced with the data from the backup, and the program will restart. Continue?")},
        {QStringLiteral("Удаление пользователя"), QStringLiteral("Delete user")},
        {QStringLiteral("Удалить выбранные камеры (%1)?\nУже записанные файлы архива останутся на диске."), QStringLiteral("Delete the selected cameras (%1)?\nFiles already recorded to the archive will stay on disk.")},
        {QStringLiteral("Удалить выбранное"), QStringLiteral("Delete selected")},
        {QStringLiteral("Удалить камеру"), QStringLiteral("Delete camera")},
        {QStringLiteral("Удалить все камеры"), QStringLiteral("Delete all cameras")},
        {QStringLiteral("Удалить ВСЕ камеры и каналы регистраторов (%1) из PSVMS?\nВсе потоки и запись будут остановлены. Уже сохранённые файлы архива останутся на диске. Это действие нельзя отменить."), QStringLiteral("Delete ALL cameras and recorder channels (%1) from PSVMS?\nAll streams and recording will stop. Existing archive files will remain on disk. This action cannot be undone.")},
        {QStringLiteral("Удалить все"), QStringLiteral("Delete all")},
        {QStringLiteral("Удалить все камеры и каналы регистраторов из списка PSVMS"), QStringLiteral("Remove all cameras and recorder channels from the PSVMS list")},
        {QStringLiteral("Удалить пользователя «%1»?"), QStringLiteral("Delete the user “%1”?")},
        {QStringLiteral("Удалить шаблон"), QStringLiteral("Delete template")},
        {QStringLiteral("Укажите IP-адрес устройства."), QStringLiteral("Enter the device IP address.")},
        {QStringLiteral("Укажите имя пользователя."), QStringLiteral("Enter a user name.")},
        {QStringLiteral("Устройство %1 требует логин и пароль.\nВведите данные для входа."), QStringLiteral("Device %1 requires a login and password.\nEnter the sign-in details.")},
        {QStringLiteral("Файл повреждён или не является резервной копией программы."), QStringLiteral("The file is damaged or is not a backup of this program.")},
        {QStringLiteral("Формат даты и времени"), QStringLiteral("Date and time format")},
        {QStringLiteral("Шаблон «%1» сохранён"), QStringLiteral("Template “%1” saved")},
        {QStringLiteral("Шаблоны"), QStringLiteral("Templates")},
        {QStringLiteral("Язык"), QStringLiteral("Language")},
        {QStringLiteral("Яркость"), QStringLiteral("Brightness")},
        {QStringLiteral("администратор"), QStringLiteral("administrator")},
        {QStringLiteral("без пароля"), QStringLiteral("no password")},
        {QStringLiteral("вход"), QStringLiteral("sign in")},
        {QStringLiteral("задан"), QStringLiteral("set")},
        {QStringLiteral("необязательно"), QStringLiteral("optional")},
        {QStringLiteral("оператор"), QStringLiteral("operator")},
        {QStringLiteral("⏹ Остановить показ"), QStringLiteral("⏹ Stop viewing")},
        {QStringLiteral("▶ Запустить"), QStringLiteral("▶ Start")},
        {QStringLiteral("● В сети: %1"), QStringLiteral("● Online: %1")},
        {QStringLiteral("● Не в сети: %1"), QStringLiteral("● Offline: %1")},
        {QStringLiteral("Главная"), QStringLiteral("Home")},
        {QStringLiteral("Камеры"), QStringLiteral("Cameras")},
        {QStringLiteral("Активен"), QStringLiteral("Active")},
        {QStringLiteral("Активировать файл лицензии…"), QStringLiteral("Activate license file…")},
        {QStringLiteral("Адрес RTSP"), QStringLiteral("RTSP address")},
        {QStringLiteral("Автоматическая блокировка"), QStringLiteral("Automatic lock")},
        {QStringLiteral("Автоматический поиск (SADP + ONVIF)…"), QStringLiteral("Automatic discovery (SADP + ONVIF)…")},
        {QStringLiteral("Аналоговая камера (карта видеозахвата)"), QStringLiteral("Analog camera (video capture card)")},
        {QStringLiteral("Аналоговую камеру (карта видеозахвата)…"), QStringLiteral("Add analog camera (video capture card)…")},
        {QStringLiteral("Архив — полноэкранный просмотр"), QStringLiteral("Archive — full-screen playback")},
        {QStringLiteral("Блокировать PSVMS при бездействии"), QStringLiteral("Lock PSVMS when idle")},
        {QStringLiteral("Быстрое добавление по RTSP"), QStringLiteral("Quick add via RTSP")},
        {QStringLiteral("В демо-режиме доступны первые 10 подключённых камер. Остальные устройства не загружены."), QStringLiteral("Demo mode supports the first 10 connected cameras. Other devices were not loaded.")},
        {QStringLiteral("В демо-режиме одновременно доступны не более 10 камер. Лишние камеры удалены из списка подключений."), QStringLiteral("Demo mode supports up to 10 cameras at a time. Additional cameras were removed from the connection list.")},
        {QStringLiteral("В демо-версии можно подключить не более 10 камер. Для снятия ограничения активируйте Pro-версию в настройках."), QStringLiteral("Demo mode supports up to 10 cameras. Activate Pro in Settings to remove this limit.")},
        {QStringLiteral("В демо-версии можно добавить ещё %1 камер. Остальные устройства не добавлены. Для полного доступа активируйте Pro-версию в настройках."), QStringLiteral("You can add %1 more cameras in Demo mode. The remaining devices were not added. Activate Pro in Settings for full access.")},
        {QStringLiteral("В раскладке можно одновременно показать не более 64 камер"), QStringLiteral("The grid can display up to 64 cameras at once")},
        {QStringLiteral("Введите IP-адрес или имя узла."), QStringLiteral("Enter an IP address or host name.")},
        {QStringLiteral("Вернуть масштабирование к стандартным настройкам"), QStringLiteral("Restore default scaling")},
        {QStringLiteral("Вертикальная регулировка громкости"), QStringLiteral("Vertical volume control")},
        {QStringLiteral("Видео Matroska (*.mkv);;Все файлы (*.*)"), QStringLiteral("Matroska video (*.mkv);;All files (*.*)")},
        {QStringLiteral("Включить / отключить"), QStringLiteral("Enable / disable")},
        {QStringLiteral("Включить или выключить голосовое управление на русском языке"), QStringLiteral("Turn Russian voice control on or off")},
        {QStringLiteral("Включить пользователя"), QStringLiteral("Enable user")},
        {QStringLiteral("Временно отключить"), QStringLiteral("Disable temporarily")},
        {QStringLiteral("Время:"), QStringLiteral("Time:")},
        {QStringLiteral("Вчера"), QStringLiteral("Yesterday")},
        {QStringLiteral("Вы"), QStringLiteral("You")},
        {QStringLiteral("Выберите камеру и дату, затем откройте нужную запись."), QStringLiteral("Select a camera and date, then open a recording.")},
        {QStringLiteral("Выберите поворотную камеру, добавленную по ONVIF"), QStringLiteral("Select a PTZ camera added via ONVIF")},
        {QStringLiteral("Выберите файл лицензии PSVMS"), QStringLiteral("Select a PSVMS license file")},
        {QStringLiteral("Голосовое управление"), QStringLiteral("Voice control")},
        {QStringLiteral("Голосовое управление в этой версии доступно в Windows."), QStringLiteral("Voice control is available on Windows in this version.")},
        {QStringLiteral("Голосовое управление включено. Скажите, например: «покажи камеры» или «полный экран»"), QStringLiteral("Voice control is on. Try saying “show cameras” or “full screen”.")},
        {QStringLiteral("Голосовое управление выключено"), QStringLiteral("Voice control is off")},
        {QStringLiteral("Гость"), QStringLiteral("Guest")},
        {QStringLiteral("Групп пока нет: укажите группу хотя бы у одной камеры."), QStringLiteral("No groups yet. Assign at least one camera to a group.")},
        {QStringLiteral("Дважды щёлкните, чтобы воспроизвести запись"), QStringLiteral("Double-click to play the recording")},
        {QStringLiteral("Двойной щелчок — открыть или закрыть полноэкранный просмотр"), QStringLiteral("Double-click to enter or exit full-screen view")},
        {QStringLiteral("Для UDP-проверки укажите IP-адрес в числовом формате."), QStringLiteral("Enter a numeric IP address for the UDP test.")},
        {QStringLiteral("Для камер и регистраторов, адрес потока которых у вас уже есть."), QStringLiteral("For cameras and recorders whose stream URL you already know.")},
        {QStringLiteral("Для учётной записи «Разработчик» необходимо создать пароль. Он будет запрашиваться при каждом входе."), QStringLiteral("A password must be created for the Developer account. It will be requested at every sign-in.")},
        {QStringLiteral("Для учётной записи «Разработчик» пароль обязателен."), QStringLiteral("The Developer account requires a password.")},
        {QStringLiteral("Добавить IP-камеру, регистратор или устройство видеозахвата"), QStringLiteral("Add an IP camera, recorder, or video capture device")},
        {QStringLiteral("Добавить по IP / производителю…"), QStringLiteral("Add by IP / manufacturer…")},
        {QStringLiteral("Добавить по IP: Hikvision, HiWatch, Dahua"), QStringLiteral("Add by IP: Hikvision, HiWatch, Dahua")},
        {QStringLiteral("Добавьте камеру, чтобы просматривать архив."), QStringLiteral("Add a camera to view the archive.")},
        {QStringLiteral("Журнал входов и действий"), QStringLiteral("Sign-in and activity log")},
        {QStringLiteral("Журнал действий…"), QStringLiteral("Activity log…")},
        {QStringLiteral("Журнал пока пуст или недоступен."), QStringLiteral("The log is empty or unavailable.")},
        {QStringLiteral("За этот день записей нет.\nВыберите другую дату или нажмите «Обновить архив»."), QStringLiteral("No recordings for this day.\nChoose another date or click “Refresh archive”.")},
        {QStringLiteral("Записей: %1"), QStringLiteral("Recordings: %1")},
        {QStringLiteral("Записи за выбранный день"), QStringLiteral("Recordings for the selected day")},
        {QStringLiteral("Запись успешно экспортирована."), QStringLiteral("Recording exported successfully.")},
        {QStringLiteral("Запустить"), QStringLiteral("Start")},
        {QStringLiteral("Защита Administrator"), QStringLiteral("Administrator security")},
        {QStringLiteral("Здесь появятся ответы ИИ-помощника."), QStringLiteral("AI assistant replies will appear here.")},
        {QStringLiteral("Здравствуйте! Помогу с подключением камер, RTSP/ONVIF, портами и настройкой программы."), QStringLiteral("Hello! I can help with camera connections, RTSP/ONVIF, ports, and application settings.")},
        {QStringLiteral("ИИ-помощник PSVMS — Yandex"), QStringLiteral("PSVMS AI assistant — Yandex")},
        {QStringLiteral("ИИ-помощник отключён в настройках."), QStringLiteral("The AI assistant is disabled in Settings.")},
        {QStringLiteral("Идентификатор каталога Yandex Cloud (Folder ID)"), QStringLiteral("Yandex Cloud folder ID (Folder ID)")},
        {QStringLiteral("Изменения сохранены. Новое масштабирование применится после перезапуска программы."), QStringLiteral("Changes saved. The new scaling will take effect after restarting the application.")},
        {QStringLiteral("Изменять пароль учётной записи «Разработчик» может только сам разработчик."), QStringLiteral("Only the developer can change the Developer account password.")},
        {QStringLiteral("Импорт и совместимость"), QStringLiteral("Import and compatibility")},
        {QStringLiteral("Истекло время ожидания. Устройство не ответило на TCP-подключение за 4,5 секунды."), QStringLiteral("Timed out. The device did not respond to the TCP connection within 4.5 seconds.")},
        {QStringLiteral("Камера требует пароль"), QStringLiteral("Camera requires a password")},
        {QStringLiteral("Камеры не в сети"), QStringLiteral("Offline cameras")},
        {QStringLiteral("Лимит демо-версии — 10 камер. Активируйте Pro-версию в настройках, чтобы добавить больше."), QStringLiteral("Demo limit: 10 cameras. Activate Pro in Settings to add more.")},
        {QStringLiteral("Лицензии PSVMS (*.lic);;Все файлы (*.*)"), QStringLiteral("PSVMS licenses (*.lic);;All files (*.*)")},
        {QStringLiteral("Лицензия"), QStringLiteral("License")},
        {QStringLiteral("Лицензия активирована"), QStringLiteral("License activated")},
        {QStringLiteral("Лицензия не активирована"), QStringLiteral("License not activated")},
        {QStringLiteral("Масштаб интерфейса (пресет, после перезапуска)"), QStringLiteral("Interface scale (preset, after restart)")},
        {QStringLiteral("Масштабирование"), QStringLiteral("Scaling")},
        {QStringLiteral("Модель"), QStringLiteral("Model")},
        {QStringLiteral("Модель камеры или регистратора (необязательно)"), QStringLiteral("Camera or recorder model (optional)")},
        {QStringLiteral("Найдено: %1"), QStringLiteral("Found: %1")},
        {QStringLiteral("Напишите вопрос о камерах, регистраторах или настройке PSVMS…"), QStringLiteral("Ask a question about cameras, recorders, or PSVMS settings…")},
        {QStringLiteral("Настройки доступны только администратору"), QStringLiteral("Settings are available to administrators only")},
        {QStringLiteral("Настройки доступны только администратору."), QStringLiteral("Settings are available to administrators only.")},
        {QStringLiteral("Не удалось завершить экспорт: %1"), QStringLiteral("Could not complete the export: %1")},
        {QStringLiteral("Не удалось запустить распознавание речи Windows"), QStringLiteral("Could not start Windows speech recognition")},
        {QStringLiteral("Не удалось открыть запись: %1"), QStringLiteral("Could not open the recording: %1")},
        {QStringLiteral("Не удалось отправить UDP-пакет. Проверьте IP, маршрут и настройки сети."), QStringLiteral("Could not send the UDP packet. Check the IP address, route, and network settings.")},
        {QStringLiteral("Не удалось создать файл: %1"), QStringLiteral("Could not create the file: %1")},
        {QStringLiteral("Недостаточно прав."), QStringLiteral("Insufficient permissions.")},
        {QStringLiteral("Недостаточно прав: требуется администратор."), QStringLiteral("Insufficient permissions: administrator access is required.")},
        {QStringLiteral("Нельзя отключить последнего активного администратора."), QStringLiteral("The last active administrator cannot be disabled.")},
        {QStringLiteral("Нельзя отключить учётную запись, под которой выполнен вход."), QStringLiteral("You cannot disable the account currently signed in.")},
        {QStringLiteral("Нельзя понизить последнего администратора."), QStringLiteral("The last administrator cannot be demoted.")},
        {QStringLiteral("Нет активных учётных записей. Обратитесь к администратору."), QStringLiteral("No active accounts. Contact an administrator.")},
        {QStringLiteral("Нет выбранной камеры"), QStringLiteral("No camera selected")},
        {QStringLiteral("Нет изображения: %1"), QStringLiteral("No image: %1")},
        {QStringLiteral("Новый пользователь будет создан без пароля. При необходимости пароль можно задать позже."), QStringLiteral("The new user will be created without a password. You can set one later if needed.")},
        {QStringLiteral("Номер порта"), QStringLiteral("Port number")},
        {QStringLiteral("Обновить окна с камерами"), QStringLiteral("Refresh camera windows")},
        {QStringLiteral("Ожидание видеопотока…"), QStringLiteral("Waiting for video stream…")},
        {QStringLiteral("Ответы генерируются Yandex AI Studio. Не отправляйте пароли и секретные данные."), QStringLiteral("Responses are generated by Yandex AI Studio. Do not send passwords or other secrets.")},
        {QStringLiteral("Отключить / включить"), QStringLiteral("Disable / enable")},
        {QStringLiteral("Отключён"), QStringLiteral("Disabled")},
        {QStringLiteral("Открыть архив записей выбранной камеры"), QStringLiteral("Open the selected camera’s recording archive")},
        {QStringLiteral("Открыть настройки программы"), QStringLiteral("Open application settings")},
        {QStringLiteral("Отправить"), QStringLiteral("Send")},
        {QStringLiteral("Ошибка Yandex AI"), QStringLiteral("Yandex AI error")},
        {QStringLiteral("Ошибка чтения или записи при экспорте."), QStringLiteral("Read or write error during export.")},
        {QStringLiteral("ПОРТ ДОСТУПЕН"), QStringLiteral("PORT AVAILABLE")},
        {QStringLiteral("ПОРТ НЕДОСТУПЕН"), QStringLiteral("PORT UNAVAILABLE")},
        {QStringLiteral("Параметры сохранены."), QStringLiteral("Settings saved.")},
        {QStringLiteral("Пароль виден"), QStringLiteral("Password visible")},
        {QStringLiteral("Пароль камеры «%1» сохранён"), QStringLiteral("Password for camera “%1” saved")},
        {QStringLiteral("Пароль устройства"), QStringLiteral("Device password")},
        {QStringLiteral("Первый вход разработчика"), QStringLiteral("First developer sign-in")},
        {QStringLiteral("Переключить полноэкранный режим (F11)"), QStringLiteral("Toggle full-screen mode (F11)")},
        {QStringLiteral("Перетащите камеру сюда"), QStringLiteral("Drag a camera here")},
        {QStringLiteral("По RTSP-адресу…"), QStringLiteral("By RTSP URL…")},
        {QStringLiteral("Подключение к камере…"), QStringLiteral("Connecting to camera…")},
        {QStringLiteral("Подключить"), QStringLiteral("Connect")},
        {QStringLiteral("Подпись проверена. Полная версия PSVMS активирована на этом компьютере."), QStringLiteral("Signature verified. The full version of PSVMS is activated on this computer.")},
        {QStringLiteral("Поиск записи по времени…"), QStringLiteral("Search recordings by time…")},
        {QStringLiteral("Поиск камер в сети: Hikvision, HiWatch, Polyvision, Dahua"), QStringLiteral("Discover cameras on the network: Hikvision, HiWatch, Polyvision, Dahua")},
        {QStringLiteral("Поиск камер и регистраторов в вашей сети"), QStringLiteral("Search for cameras and recorders on your network")},
        {QStringLiteral("Поиск по названию, IP или группе…"), QStringLiteral("Search by name, IP, or group…")},
        {QStringLiteral("Показано камер: %1 из %2"), QStringLiteral("Showing %1 of %2 cameras")},
        {QStringLiteral("Показать записи за вчера"), QStringLiteral("Show yesterday’s recordings")},
        {QStringLiteral("Показать записи за сегодня"), QStringLiteral("Show today’s recordings")},
        {QStringLiteral("Показывать разрешение и качество связи поверх камеры"), QStringLiteral("Show resolution and connection quality over the camera view")},
        {QStringLiteral("Пользователь не найден."), QStringLiteral("User not found.")},
        {QStringLiteral("Предпросмотр камеры"), QStringLiteral("Camera preview")},
        {QStringLiteral("Применить изменения"), QStringLiteral("Apply changes")},
        {QStringLiteral("Программа запускается в гостевом режиме. Выберите Гостя или другую учётную запись."), QStringLiteral("The application is starting in guest mode. Select Guest or another account.")},
        {QStringLiteral("Пропущено повторных устройств: %1. Уже добавленные камеры и регистраторы не дублируются."), QStringLiteral("Skipped %1 duplicate devices. Existing cameras and recorders are not added again.")},
        {QStringLiteral("Простой режим"), QStringLiteral("Low-power mode")},
        {QStringLiteral("Простой режим снижает нагрузку: меньше кадров в секунду, без анимаций и сглаживания"), QStringLiteral("Low-power mode reduces system load: lower frame rate, no animations, and no smoothing")},
        {QStringLiteral("Протокол"), QStringLiteral("Protocol")},
        {QStringLiteral("Развернуть боковую панель"), QStringLiteral("Expand sidebar")},
        {QStringLiteral("Раздел событий недоступен в этой редакции"), QStringLiteral("The Events section is unavailable in this edition")},
        {QStringLiteral("Размер кнопок и элементов (после перезапуска)"), QStringLiteral("Button and control size (after restart)")},
        {QStringLiteral("Размер текста (после перезапуска)"), QStringLiteral("Text size (after restart)")},
        {QStringLiteral("Разработчик"), QStringLiteral("Developer")},
        {QStringLiteral("Разработчик: %1"), QStringLiteral("Developer: %1")},
        {QStringLiteral("Распознано: %1"), QStringLiteral("Recognized: %1")},
        {QStringLiteral("Расширенный режим"), QStringLiteral("Advanced mode")},
        {QStringLiteral("Рекомендуется установить пароль для Administrator. Установить пароль сейчас?"), QStringLiteral("It is recommended to set a password for Administrator. Set it now?")},
        {QStringLiteral("Роль пользователя"), QStringLiteral("User role")},
        {QStringLiteral("Роль системной учётной записи «Гость» нельзя изменить."), QStringLiteral("The system Guest account role cannot be changed.")},
        {QStringLiteral("Роль учётной записи «Разработчик» нельзя изменить."), QStringLiteral("The Developer account role cannot be changed.")},
        {QStringLiteral("Роль…"), QStringLiteral("Role…")},
        {QStringLiteral("Ручной размер интерфейса (после перезапуска)"), QStringLiteral("Custom interface size (after restart)")},
        {QStringLiteral("Сбросить пресет, ручной масштаб, размер текста и элементов до 100%"), QStringLiteral("Reset preset, custom scale, text size, and control size to 100%")},
        {QStringLiteral("Свернуть боковую панель"), QStringLiteral("Collapse sidebar")},
        {QStringLiteral("Связь / запись"), QStringLiteral("Connection / recording")},
        {QStringLiteral("Сеанс заблокирован из-за бездействия."), QStringLiteral("The session was locked due to inactivity.")},
        {QStringLiteral("Сегодня"), QStringLiteral("Today")},
        {QStringLiteral("Секретный API-ключ Yandex AI Studio"), QStringLiteral("Yandex AI Studio secret API key")},
        {QStringLiteral("Сервис вернул пустой ответ. Проверьте модель, каталог и права API-ключа."), QStringLiteral("The service returned an empty response. Check the model, folder, and API key permissions.")},
        {QStringLiteral("Системную учётную запись «Гость» нельзя удалить."), QStringLiteral("The system Guest account cannot be deleted.")},
        {QStringLiteral("Системные учётные записи «Гость» и «Разработчик» нельзя отключить."), QStringLiteral("The system Guest and Developer accounts cannot be disabled.")},
        {QStringLiteral("Скопировать ID компьютера"), QStringLiteral("Copy computer ID")},
        {QStringLiteral("Сменить пользователя…"), QStringLiteral("Switch user…")},
        {QStringLiteral("Сначала выберите запись в списке."), QStringLiteral("Select a recording from the list first.")},
        {QStringLiteral("Сначала укажите API-ключ и Folder ID."), QStringLiteral("Enter the API key and Folder ID first.")},
        {QStringLiteral("Состояние пользователя"), QStringLiteral("User status")},
        {QStringLiteral("Сохранить выбранные параметры масштабирования"), QStringLiteral("Save the selected scaling settings")},
        {QStringLiteral("Сохранить выбранный сегмент архива в отдельный MKV-файл"), QStringLiteral("Save the selected archive segment as a separate MKV file")},
        {QStringLiteral("Список камер пуст"), QStringLiteral("Camera list is empty")},
        {QStringLiteral("Срок лицензии истёк. Включён демонстрационный режим."), QStringLiteral("The license has expired. Demo mode is now enabled.")},
        {QStringLiteral("Тип службы"), QStringLiteral("Service type")},
        {QStringLiteral("Удалить лицензию"), QStringLiteral("Remove license")},
        {QStringLiteral("Удалить лицензию с этого компьютера? Программа вернётся в демо-режим."), QStringLiteral("Remove the license from this computer? The application will return to Demo mode.")},
        {QStringLiteral("Удерживайте, чтобы показать пароль"), QStringLiteral("Press and hold to show the password")},
        {QStringLiteral("Укажите IP-адрес и порт, затем нажмите «Проверить порт»."), QStringLiteral("Enter an IP address and port, then click “Test port”.")},
        {QStringLiteral("Укажите название и корректный адрес, начинающийся с rtsp://"), QStringLiteral("Enter a name and a valid address beginning with rtsp://")},
        {QStringLiteral("Укажите название камеры."), QStringLiteral("Enter a camera name.")},
        {QStringLiteral("Управление PTZ"), QStringLiteral("PTZ control")},
        {QStringLiteral("Управление PTZ доступно в Pro-версии"), QStringLiteral("PTZ control is available in the Pro version")},
        {QStringLiteral("Управление PTZ доступно в режиме разработчика"), QStringLiteral("PTZ control is available in Developer mode")},
        {QStringLiteral("Устройства захвата не найдены"), QStringLiteral("No capture devices found")},
        {QStringLiteral("Устройство"), QStringLiteral("Device")},
        {QStringLiteral("Устройство захвата не выбрано или не найдено."), QStringLiteral("No capture device selected or found.")},
        {QStringLiteral("Устройство уже добавлено"), QStringLiteral("Device already added")},
        {QStringLiteral("Устройство уже добавлено в видеосетку."), QStringLiteral("The device is already in the video grid.")},
        {QStringLiteral("Устройство уже добавлено в другую ячейку."), QStringLiteral("The device is already assigned to another cell.")},
        {QStringLiteral("Учётную запись «Разработчик» нельзя удалить."), QStringLiteral("The Developer account cannot be deleted.")},
        {QStringLiteral("Файл записи не найден на диске."), QStringLiteral("Recording file not found on disk.")},
        {QStringLiteral("Часть камер уже размещена в ячейках и была пропущена: %1."), QStringLiteral("Some cameras were already placed in cells and were skipped: %1.")},
        {QStringLiteral("Экспорт архива"), QStringLiteral("Archive export")},
        {QStringLiteral("Экспортировать запись"), QStringLiteral("Export recording")},
        {QStringLiteral("Эта учётная запись отключена."), QStringLiteral("This account is disabled.")},
        {QStringLiteral("Это устройство уже есть в списке камер или регистраторов."), QStringLiteral("This device is already in the camera or recorder list.")},
        {QStringLiteral("Этот раздел доступен только администратору."), QStringLiteral("This section is available to administrators only.")},
        {QStringLiteral("гость"), QStringLiteral("guest")},
        {QStringLiteral("разработчик"), QStringLiteral("developer")},
        {QStringLiteral("⇩  Экспортировать выбранную запись"), QStringLiteral("⇩  Export selected recording")},
        {QStringLiteral("● В сети"), QStringLiteral("● Online")},
        {QStringLiteral("● Не в сети"), QStringLiteral("● Offline")},
        {QStringLiteral("⛶  Во весь экран"), QStringLiteral("⛶  Full screen")},
        {QStringLiteral("⟳  Обновить архив"), QStringLiteral("⟳  Refresh archive")},
        {QStringLiteral("⟳ Обновить"), QStringLiteral("⟳ Refresh")},
        {QStringLiteral("＋  Добавить устройство"), QStringLiteral("＋  Add device")},
        {QStringLiteral("🎙 Голос"), QStringLiteral("🎙 Voice")},
        {QStringLiteral("🔴 Слушаю"), QStringLiteral("🔴 Listening")},
        {QStringLiteral("\nДемо-режим: доступно не более 10 камер."), QStringLiteral("\nDemo mode: up to 10 cameras are available.")},
        {QStringLiteral("\nПолный доступ к функциям программы включён."), QStringLiteral("\nFull access to all application features is enabled.")},
        {QStringLiteral("   ·   %1 МБ"), QStringLiteral("   ·   %1 MB")},
        {QStringLiteral(" мин."), QStringLiteral(" min.")},
        {QStringLiteral("..."), QStringLiteral("...")},
        {QStringLiteral("API-ключ"), QStringLiteral("API key")},
        {QStringLiteral("DEMO ВЕРСИЯ"), QStringLiteral("DEMO VERSION")},
        {QStringLiteral("Dahua — TCP Service"), QStringLiteral("Dahua — TCP service")},
        {QStringLiteral("Folder ID"), QStringLiteral("Folder ID")},
        {QStringLiteral("Hikvision — SDK / Server"), QStringLiteral("Hikvision — SDK / Server")},
        {QStringLiteral("ID компьютера"), QStringLiteral("Computer ID")},
        {QStringLiteral("ID компьютера скопирован. Отправьте его для выпуска лицензии."), QStringLiteral("Computer ID copied. Send it to request a license.")},
        {QStringLiteral("ID этого компьютера для выпуска лицензии:\n%1"), QStringLiteral("This computer’s ID for license issuance:\n%1")},
        {QStringLiteral("IP-адрес / имя узла"), QStringLiteral("IP address / host name")},
        {QStringLiteral("ONVIF — обнаружение/управление (часто HTTP)"), QStringLiteral("ONVIF — discovery/control (often HTTP)")},
        {QStringLiteral("PSVMS — DEMO ВЕРСИЯ"), QStringLiteral("PSVMS — DEMO VERSION")},
        {QStringLiteral("PSVMS — полная Pro-версия"), QStringLiteral("PSVMS — full Pro version")},
        {QStringLiteral("PTZ поддерживается"), QStringLiteral("PTZ supported")},
        {QStringLiteral("TCP-соединение установлено. Служба принимает подключения на этом порту."), QStringLiteral("TCP connection established. A service is accepting connections on this port.")},
        {QStringLiteral("UDP-пакет отправлен. Это НЕ подтверждает, что порт открыт: UDP не устанавливает соединение, а ответ зависит от службы устройства."), QStringLiteral("UDP packet sent. This does NOT confirm that the port is open: UDP does not establish a connection, and a reply depends on the device service.")},
        {QStringLiteral("АРХИВ ЗАПИСЕЙ"), QStringLiteral("RECORDING ARCHIVE")},
        {QStringLiteral("Для работы нужен API-ключ сервисного аккаунта Yandex AI Studio с доступом к генерации текста и Folder ID. Используется модель YandexGPT Pro 5.1. Запросы отправляются в облако Yandex; тарификация зависит от вашего аккаунта и условий Yandex Cloud. При выключении переключателя чат не открывается и запросы не отправляются."), QStringLiteral("Requires a Yandex AI Studio service-account API key with text-generation access and a Folder ID. Uses the YandexGPT Pro 5.1 model. Requests are sent to Yandex Cloud; charges depend on your account and Yandex Cloud terms. When disabled, the chat does not open and no requests are sent.")},
        {QStringLiteral("Dahua: часто HTTP 80/TCP, RTSP 554/TCP, TCP Service 37777/TCP. Порты зависят от модели и прошивки; уточните их в Network → Connection → Port или в руководстве устройства."), QStringLiteral("Dahua commonly uses HTTP 80/TCP, RTSP 554/TCP, and TCP service 37777/TCP. Ports vary by model and firmware; check Network → Connection → Port or the device manual.")},
        {QStringLiteral("Hikvision / HiWatch: обычно HTTP 80/TCP, HTTPS 443/TCP, RTSP 554/TCP, SDK/Server 8000/TCP. В некоторых моделях есть дополнительные порты 8443, 7681 и 7682. Проверьте меню Configuration → Network → Basic Settings → Port и документацию именно вашей модели."), QStringLiteral("Hikvision / HiWatch commonly uses HTTP 80/TCP, HTTPS 443/TCP, RTSP 554/TCP, and SDK/Server 8000/TCP. Some models also use ports 8443, 7681, and 7682. Check Configuration → Network → Basic Settings → Port and the documentation for your model.")},
        {QStringLiteral("Uniview: обычно используются HTTP/HTTPS для веб-доступа и RTSP 554/TCP для потока; порт управления зависит от модели и версии прошивки. Проверьте Network → Port в веб-интерфейсе."), QStringLiteral("Uniview commonly uses HTTP/HTTPS for web access and RTSP 554/TCP for video. The management port depends on the model and firmware. Check Network → Port in the web interface.")},
        {QStringLiteral("Подсказки по типовым портам:\n• RTSP: 554/TCP — видеопоток большинства IP-камер.\n• HTTP: 80/TCP; HTTPS: 443/TCP — веб-интерфейс.\n• Hikvision/HiWatch: 8000/TCP — SDK/Server, 554/TCP — RTSP.\n• Dahua: 37777/TCP — сервисный порт, 554/TCP — RTSP.\n• ONVIF часто использует HTTP-порт устройства (обычно 80 или заданный вручную).\nПорты могут быть изменены производителем, установщиком или администратором. Сверяйтесь с сетевыми настройками конкретной модели."), QStringLiteral("Typical port reference:\n• RTSP: 554/TCP — video stream for most IP cameras.\n• HTTP: 80/TCP; HTTPS: 443/TCP — web interface.\n• Hikvision/HiWatch: 8000/TCP — SDK/Server, 554/TCP — RTSP.\n• Dahua: 37777/TCP — service port, 554/TCP — RTSP.\n• ONVIF often uses the device HTTP port (usually 80 or a custom value).\nPorts may be changed by the manufacturer, installer, or administrator. Check the network settings for your specific model.")},
        {QStringLiteral("Типовые значения: RTSP 554/TCP, HTTP 80/TCP, HTTPS 443/TCP, ONVIF — часто HTTP-порт устройства. Порты управления зависят от производителя и модели; не применяйте значения Hikvision/Dahua к другому устройству без проверки документации."), QStringLiteral("Typical values: RTSP 554/TCP, HTTP 80/TCP, HTTPS 443/TCP. ONVIF often uses the device HTTP port. Management ports vary by manufacturer and model; do not apply Hikvision/Dahua values to other devices without checking the documentation.")},
    };
    return dict;
}

} // namespace

void I18n::init()
{
    const QString saved = QSettings().value(QStringLiteral("ui/lang")).toString();
    if (saved == QLatin1String("en"))
        g_lang = Lang::En;
    else if (saved == QLatin1String("ru"))
        g_lang = Lang::Ru;
    else
        g_lang = (QLocale::system().language() == QLocale::Russian) ? Lang::Ru : Lang::En;
    QLocale::setDefault(QLocale(g_lang == Lang::Ru ? QLocale::Russian : QLocale::English));
}

I18n::Lang I18n::language()
{
    return g_lang;
}

QString I18n::code()
{
    return g_lang == Lang::Ru ? QStringLiteral("ru") : QStringLiteral("en");
}

void I18n::setLanguage(Lang lang)
{
    g_lang = lang;
    QSettings().setValue(QStringLiteral("ui/lang"), code());
    QLocale::setDefault(QLocale(lang == Lang::Ru ? QLocale::Russian : QLocale::English));
}

void I18n::setLanguageByCode(const QString &c)
{
    setLanguage(c == QLatin1String("en") ? Lang::En : Lang::Ru);
}

void I18n::requestRestart()
{
    g_restartRequested = true;
    QCoreApplication::quit();
}

bool I18n::takeRestartRequest()
{
    const bool r = g_restartRequested;
    g_restartRequested = false;
    return r;
}

QString I18n::translate(const char *ru)
{
    const QString key = QString::fromUtf8(ru);
    if (g_lang == Lang::En) {
        const auto it = englishDict().constFind(key);
        if (it != englishDict().constEnd())
            return it.value();
        // Never leak Russian text into the English interface. This fallback also
        // covers dynamic event/error strings until a dedicated translation is added.
        static const QRegularExpression cyrillic(QStringLiteral("[\\x{0400}-\\x{04FF}]"));
        if (key.contains(cyrillic))
            return QStringLiteral("Translation unavailable");
    }
    return key;
}

// ======================================================================
// core/onvif.cpp
// ======================================================================
namespace {

constexpr int kTimeoutMs = 6000;

bool isName_onvif(const QXmlStreamReader &x, const char *n)
{
    return x.name() == QLatin1String(n);
}

// ---------- SOAP ----------

// Конверт SOAP 1.2; при заданном логине добавляется WS-Security UsernameToken (PasswordDigest).
// offsetSecs — разница часов камеры и компьютера: без неё многие камеры отклоняют подпись.
QByteArray soapEnvelope(const QByteArray &body, const QString &user = QString(),
                        const QString &pass = QString(), qint64 offsetSecs = 0)
{
    QByteArray header;
    if (!user.isEmpty()) {
        QByteArray nonce(16, '\0');
        for (char &c : nonce)
            c = char(QRandomGenerator::global()->bounded(256));
        const QByteArray created =
            QDateTime::currentDateTimeUtc().addSecs(offsetSecs).toString(Qt::ISODate).toUtf8();
        const QByteArray digest =
            QCryptographicHash::hash(nonce + created + pass.toUtf8(), QCryptographicHash::Sha1).toBase64();

        header = QByteArray(
            "<s:Header><wsse:Security s:mustUnderstand=\"1\" "
            "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
            "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
            "<wsse:UsernameToken><wsse:Username>")
            + user.toHtmlEscaped().toUtf8()
            + QByteArray("</wsse:Username><wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
                         "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">")
            + digest
            + QByteArray("</wsse:Password><wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
                         "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">")
            + nonce.toBase64()
            + QByteArray("</wsse:Nonce><wsu:Created>") + created
            + QByteArray("</wsu:Created></wsse:UsernameToken></wsse:Security></s:Header>");
    }

    return QByteArray(
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
               "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
               "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
               "xmlns:tt=\"http://www.onvif.org/ver10/schema\">")
           + header + QByteArray("<s:Body>") + body + QByteArray("</s:Body></s:Envelope>");
}

struct SoapReply {
    bool ok = false;
    QByteArray body;
    QString error;
};

QString faultText(const QByteArray &xml)
{
    QXmlStreamReader x(xml);
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement() && (isName_onvif(x, "Text") || isName_onvif(x, "faultstring")))
            return x.readElementText().trimmed();
    }
    return QString();
}

void postSoap(QNetworkAccessManager *nam, QObject *ctx, const QUrl &url, const QByteArray &body,
              int timeoutMs, std::function<void(const SoapReply &)> cb)
{
    if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0)
        SecurityAudit::write(QStringLiteral("onvif_plain_http_warning"), url.host());
    QNetworkRequest req(url);
    if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
        req.setSslConfiguration(ssl);
    }
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/soap+xml; charset=utf-8"));
    req.setTransferTimeout(timeoutMs);
    QNetworkReply *reply = nam->post(req, body);

    QObject::connect(reply, &QNetworkReply::finished, ctx, [reply, cb] {
        SoapReply r;
        r.body = reply->readAll();
        r.ok = reply->error() == QNetworkReply::NoError;
        if (!r.ok) {
            const QString fault = faultText(r.body);
            if (reply->error() == QNetworkReply::AuthenticationRequiredError
                || fault.contains(QLatin1String("authorized"), Qt::CaseInsensitive))
                r.error = T("Неверный логин или пароль");
            else if (reply->error() == QNetworkReply::TimeoutError)
                r.error = T("Камера не отвечает (таймаут)");
            else
                r.error = fault.isEmpty() ? reply->errorString() : fault;
        }
        reply->deleteLater();
        cb(r);
    });
}

// ---------- разбор ответов ----------

QString firstText(const QByteArray &xml, const char *name)
{
    QXmlStreamReader x(xml);
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement() && isName_onvif(x, name))
            return x.readElementText().trimmed();
    }
    return QString();
}

// Разница «время камеры − наше UTC» в секундах
qint64 parseTimeOffset(const QByteArray &xml)
{
    QXmlStreamReader x(xml);
    bool inUtc = false;
    int h = -1, mi = -1, s = -1, y = -1, mo = -1, d = -1;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            if (isName_onvif(x, "UTCDateTime")) {
                inUtc = true;
            } else if (inUtc) {
                if (isName_onvif(x, "Hour")) h = x.readElementText().toInt();
                else if (isName_onvif(x, "Minute")) mi = x.readElementText().toInt();
                else if (isName_onvif(x, "Second")) s = x.readElementText().toInt();
                else if (isName_onvif(x, "Year")) y = x.readElementText().toInt();
                else if (isName_onvif(x, "Month")) mo = x.readElementText().toInt();
                else if (isName_onvif(x, "Day")) d = x.readElementText().toInt();
            }
        } else if (x.isEndElement() && isName_onvif(x, "UTCDateTime")) {
            break;
        }
    }
    if (h < 0 || mi < 0 || s < 0 || y < 0 || mo < 0 || d < 0)
        return 0;
    QDateTime cam(QDate(y, mo, d), QTime(h, mi, s));
    cam.setTimeSpec(Qt::UTC);
    if (!cam.isValid())
        return 0;
    return QDateTime::currentDateTimeUtc().secsTo(cam);
}

// Адрес сервиса по имени категории (Media, PTZ, ...) из ответа GetCapabilities
QString capabilityXAddr(const QByteArray &xml, const char *category)
{
    QXmlStreamReader x(xml);
    bool inCategory = false;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            if (isName_onvif(x, category))
                inCategory = true;
            else if (inCategory && isName_onvif(x, "XAddr"))
                return x.readElementText().trimmed();
        } else if (x.isEndElement() && isName_onvif(x, category)) {
            inCategory = false;
        }
    }
    return QString();
}

QString mediaXAddr(const QByteArray &xml)
{
    return capabilityXAddr(xml, "Media");
}

struct Profile {
    QString token;
    int w = 0;
    int h = 0;
    qint64 area() const { return qint64(w) * h; }
};

QList<Profile> parseProfiles(const QByteArray &xml)
{
    QList<Profile> out;
    QXmlStreamReader x(xml);
    bool inEnc = false, inRes = false;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            if (isName_onvif(x, "Profiles")) {
                Profile p;
                p.token = x.attributes().value(QLatin1String("token")).toString();
                out.append(p);
            } else if (isName_onvif(x, "VideoEncoderConfiguration")) {
                inEnc = true;
            } else if (inEnc && isName_onvif(x, "Resolution")) {
                inRes = true;
            } else if (inEnc && inRes && !out.isEmpty()) {
                if (isName_onvif(x, "Width")) out.last().w = x.readElementText().toInt();
                else if (isName_onvif(x, "Height")) out.last().h = x.readElementText().toInt();
            }
        } else if (x.isEndElement()) {
            if (isName_onvif(x, "Resolution")) inRes = false;
            else if (isName_onvif(x, "VideoEncoderConfiguration")) inEnc = false;
        }
    }
    return out;
}

QByteArray streamUriBody(const QString &token)
{
    return QByteArray("<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>"
                      "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup>"
                      "<trt:ProfileToken>")
           + token.toHtmlEscaped().toUtf8() + QByteArray("</trt:ProfileToken></trt:GetStreamUri>");
}

QString withCredentials(const QString &uri, const OnvifRequest &req)
{
    QUrl u(uri);
    if (!u.isValid())
        return uri;
    if (u.userName().isEmpty() && !req.user.isEmpty()) {
        u.setUserName(req.user);
        u.setPassword(req.password);
    }
    return u.toString(QUrl::FullyEncoded);
}

OnvifDevice parseScopes(OnvifDevice dev, const QString &scopes)
{
    for (const QString &tok : scopes.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        int p = tok.indexOf(QLatin1String("/name/"));
        if (p >= 0) {
            dev.name = QUrl::fromPercentEncoding(tok.mid(p + 6).toUtf8());
            continue;
        }
        p = tok.indexOf(QLatin1String("/hardware/"));
        if (p >= 0)
            dev.hardware = QUrl::fromPercentEncoding(tok.mid(p + 10).toUtf8());
    }
    return dev;
}

} // namespace

// ===========================================================================
// OnvifDiscovery
// ===========================================================================

OnvifDiscovery::OnvifDiscovery(QObject *parent) : QObject(parent)
{
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &OnvifDiscovery::onReadyRead);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] {
        m_running = false;
        emit finished();
    });
}

void OnvifDiscovery::start(int timeoutMs)
{
    stop();
    m_seen.clear();

    if (m_socket->state() != QAbstractSocket::BoundState
        && !m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        emit finished();
        return;
    }
    m_socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 4);

    m_running = true;
    sendProbes();
    QTimer::singleShot(1000, this, [this] { if (m_running) sendProbes(); });   // повтор для надёжности
    m_timer->start(timeoutMs);
}

void OnvifDiscovery::stop()
{
    m_running = false;
    m_timer->stop();
}

void OnvifDiscovery::sendProbes()
{
    const QByteArray probe =
        QByteArray(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
            "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
            "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><e:Header>"
            "<w:MessageID>uuid:")
        + QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8()
        + QByteArray(
            "</w:MessageID><w:To e:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
            "<w:Action e:mustUnderstand=\"true\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
            "</e:Header><e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body>"
            "</e:Envelope>");

    const QHostAddress group(QStringLiteral("239.255.255.250"));
    for (const QNetworkInterface &ni : QNetworkInterface::allInterfaces()) {
        const auto fl = ni.flags();
        if (!(fl & QNetworkInterface::IsUp) || !(fl & QNetworkInterface::IsRunning)
            || (fl & QNetworkInterface::IsLoopBack) || !(fl & QNetworkInterface::CanMulticast))
            continue;
        m_socket->setMulticastInterface(ni);   // шлём через каждый сетевой адаптер
        m_socket->writeDatagram(probe, group, 3702);
    }
}

void OnvifDiscovery::onReadyRead()
{
    while (m_socket->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(int(m_socket->pendingDatagramSize()));
        m_socket->readDatagram(data.data(), data.size());
        if (!m_running)
            continue;

        QString xaddrs, scopes;
        QXmlStreamReader x(data);
        while (!x.atEnd()) {
            x.readNext();
            if (x.isStartElement()) {
                if (isName_onvif(x, "XAddrs")) xaddrs = x.readElementText();
                else if (isName_onvif(x, "Scopes")) scopes = x.readElementText();
            }
        }

        OnvifDevice dev;
        for (const QString &u : xaddrs.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            const QUrl url(u);
            if (url.scheme() == QLatin1String("http") && !url.host().contains(QLatin1Char(':'))) {
                dev.xaddr = u;
                dev.address = url.host();
                break;
            }
        }
        if (dev.xaddr.isEmpty() || m_seen.contains(dev.address))
            continue;
        m_seen.insert(dev.address);
        emit deviceFound(parseScopes(dev, scopes));
    }
}

// ===========================================================================
// OnvifScanner
// ===========================================================================

OnvifScanner::OnvifScanner(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void OnvifScanner::start(quint32 firstIp, quint32 lastIp, quint16 port, int concurrency)
{
    stop();
    ++m_gen;
    m_next = firstIp;
    m_last = lastIp;
    m_port = port;
    m_conc = qMax(1, concurrency);
    m_total = int(m_last - m_next + 1);
    m_done = 0;
    m_active = 0;
    m_running = true;
    pump();
}

void OnvifScanner::stop()
{
    m_running = false;
    ++m_gen;
}

void OnvifScanner::pump()
{
    while (m_running && m_active < m_conc && m_next <= m_last) {
        const QString host = QHostAddress(quint32(m_next++)).toString();
        const QUrl url(QStringLiteral("http://%1:%2/onvif/device_service").arg(host).arg(m_port));
        const int gen = m_gen;
        ++m_active;

        postSoap(m_nam, this, url, soapEnvelope("<tds:GetSystemDateAndTime/>"), 1500,
                 [this, gen, host, url](const SoapReply &r) {
            if (gen != m_gen)
                return;
            --m_active;
            ++m_done;
            // даже SOAP-ошибка с HTTP 4xx означает, что по адресу отвечает ONVIF-устройство
            if (r.body.contains("Envelope")) {
                OnvifDevice d;
                d.address = host;
                d.xaddr = url.toString();
                emit deviceFound(d);
            }
            emit progress(m_done, m_total);
            pump();
        });
    }
    if (m_running && m_next > m_last && m_active == 0) {
        m_running = false;
        emit finished();
    }
}

// ===========================================================================
// OnvifResolver
// ===========================================================================

OnvifResolver::OnvifResolver(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void OnvifResolver::start(const QList<OnvifRequest> &requests, int concurrency)
{
    ++m_gen;
    m_requests = requests;
    m_next = 0;
    m_done = 0;
    m_active = 0;
    m_conc = qMax(1, concurrency);
    m_running = true;
    if (requests.isEmpty()) {
        m_running = false;
        emit finished();
        return;
    }
    pump();
}

void OnvifResolver::cancel()
{
    m_running = false;
    ++m_gen;
}

void OnvifResolver::pump()
{
    while (m_running && m_active < m_conc && m_next < m_requests.size()) {
        ++m_active;
        runJob(m_next++);
    }
}

void OnvifResolver::jobDone(int idx, const OnvifStreams &s, int gen)
{
    if (gen != m_gen)
        return;
    --m_active;
    ++m_done;
    emit resolved(idx, s);
    emit progress(m_done, int(m_requests.size()));
    if (m_done >= m_requests.size()) {
        m_running = false;
        emit finished();
    } else {
        pump();
    }
}

void OnvifResolver::runJob(int idx)
{
    const OnvifRequest req = m_requests.at(idx);
    const int gen = m_gen;
    const QUrl devUrl(req.xaddr);

    auto fail = [this, idx, gen](const QString &msg) {
        OnvifStreams s;
        s.error = msg;
        jobDone(idx, s, gen);
    };

    // 1. Время камеры (без авторизации) — нужно для подписи WS-Security
    postSoap(m_nam, this, devUrl, soapEnvelope("<tds:GetSystemDateAndTime/>"), kTimeoutMs,
             [=](const SoapReply &r1) {
        if (gen != m_gen)
            return;
        const qint64 offset = r1.ok ? parseTimeOffset(r1.body) : 0;

        // 2. Адрес media-сервиса
        postSoap(m_nam, this, devUrl,
                 soapEnvelope("<tds:GetCapabilities><tds:Category>Media</tds:Category></tds:GetCapabilities>",
                              req.user, req.password, offset),
                 kTimeoutMs, [=](const SoapReply &r2) {
            if (gen != m_gen)
                return;
            if (!r2.ok) {
                fail(r2.error);
                return;
            }
            QUrl mediaUrl(mediaXAddr(r2.body));
            if (!mediaUrl.isValid() || mediaUrl.host().isEmpty()) {
                fail(T("Камера не вернула адрес media-сервиса (проверьте логин и пароль)"));
                return;
            }
            mediaUrl.setHost(devUrl.host());   // камеры за NAT иногда отдают внутренний адрес

            // 3. Профили видео
            postSoap(m_nam, this, mediaUrl,
                     soapEnvelope("<trt:GetProfiles/>", req.user, req.password, offset),
                     kTimeoutMs, [=](const SoapReply &r3) {
                if (gen != m_gen)
                    return;
                if (!r3.ok) {
                    fail(r3.error);
                    return;
                }
                QList<Profile> valid;
                const QList<Profile> all = parseProfiles(r3.body);
                for (const Profile &p : all)
                    if (!p.token.isEmpty() && p.area() > 0)
                        valid.append(p);
                if (valid.isEmpty() && !all.isEmpty() && !all.first().token.isEmpty())
                    valid.append(all.first());   // разрешение неизвестно — берём первый профиль
                if (valid.isEmpty()) {
                    fail(T("У камеры нет видеопрофилей"));
                    return;
                }

                const auto byArea = [](const Profile &a, const Profile &b) { return a.area() < b.area(); };
                const Profile mainP = *std::max_element(valid.begin(), valid.end(), byArea);
                Profile subP = *std::min_element(valid.begin(), valid.end(), byArea);
                if (subP.token == mainP.token)
                    subP.token.clear();   // профиль всего один

                // 4. RTSP-адрес основного потока
                postSoap(m_nam, this, mediaUrl,
                         soapEnvelope(streamUriBody(mainP.token), req.user, req.password, offset),
                         kTimeoutMs, [=](const SoapReply &r4) {
                    if (gen != m_gen)
                        return;
                    if (!r4.ok) {
                        fail(r4.error);
                        return;
                    }
                    const QString mainUri = firstText(r4.body, "Uri");
                    if (mainUri.isEmpty()) {
                        fail(T("Камера не вернула RTSP-адрес"));
                        return;
                    }
                    OnvifStreams s;
                    s.ok = true;
                    s.mainUrl = withCredentials(mainUri, req);

                    // 5. PTZ: есть ли у камеры поворотный механизм и адрес его сервиса
                    auto withPtz = [=](OnvifStreams out) {
                        postSoap(m_nam, this, devUrl,
                                 soapEnvelope("<tds:GetCapabilities><tds:Category>PTZ</tds:Category></tds:GetCapabilities>",
                                              req.user, req.password, offset),
                                 kTimeoutMs, [=](const SoapReply &r6) {
                            if (gen != m_gen)
                                return;
                            OnvifStreams final = out;
                            if (r6.ok) {
                                const QString ptz = capabilityXAddr(r6.body, "PTZ");
                                if (!ptz.isEmpty()) {
                                    final.ptzXAddr = ptz;
                                    final.ptzToken = mainP.token;
                                    final.hasPtz = true;
                                }
                            }
                            jobDone(idx, final, gen);
                        });
                    };

                    if (subP.token.isEmpty()) {
                        withPtz(s);
                        return;
                    }

                    // 6. RTSP-адрес дополнительного потока (для мозаики)
                    postSoap(m_nam, this, mediaUrl,
                             soapEnvelope(streamUriBody(subP.token), req.user, req.password, offset),
                             kTimeoutMs, [=](const SoapReply &r5) {
                        if (gen != m_gen)
                            return;
                        OnvifStreams out = s;
                        if (r5.ok) {
                            const QString subUri = firstText(r5.body, "Uri");
                            if (!subUri.isEmpty())
                                out.subUrl = withCredentials(subUri, req);
                        }
                        withPtz(out);   // без подпотока тоже успех
                    });
                });
            });
        });
    });
}

// ======================================================================
// core/onvif_ptz.cpp
// ======================================================================
namespace {

// Обычный (не digest) логин в заголовке PTZ-запроса — большинство камер его принимают;
// камеры со строгой WS-Security-аутентификацией для этого разговора пока не поддержаны.
QByteArray ptzEnvelope(const QString &body, const PtzTarget &t)
{
    QByteArray header;
    if (!t.user.isEmpty()) {
        header = QByteArray(
            "<s:Header><wsse:Security xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
            "oasis-200401-wss-wssecurity-secext-1.0.xsd\"><wsse:UsernameToken><wsse:Username>")
            + t.user.toHtmlEscaped().toUtf8()
            + "</wsse:Username><wsse:Password>" + t.password.toHtmlEscaped().toUtf8()
            + "</wsse:Password></wsse:UsernameToken></wsse:Security></s:Header>";
    }
    return QByteArray(
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
               "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" "
               "xmlns:tt=\"http://www.onvif.org/ver10/schema\">")
           + header + "<s:Body>" + body.toUtf8() + "</s:Body></s:Envelope>";
}

void send(QNetworkAccessManager *nam, const PtzTarget &t, const QString &body)
{
    const QUrl ptzUrl(t.xAddr);
    if (ptzUrl.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0 && !t.password.isEmpty())
        SecurityAudit::write(QStringLiteral("ptz_plain_http_warning"), ptzUrl.host());
    QNetworkRequest req(ptzUrl);
    if (ptzUrl.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
        req.setSslConfiguration(ssl);
    }
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/soap+xml; charset=utf-8"));
    req.setTransferTimeout(3000);
    QNetworkReply *reply = nam->post(req, ptzEnvelope(body, t));
    QObject::connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

} // namespace

void ptzContinuousMove(QNetworkAccessManager *nam, const PtzTarget &target, double x, double y, double zoom)
{
    if (!target.valid())
        return;
    const QString body = QStringLiteral(
        "<tptz:ContinuousMove><tptz:ProfileToken>%1</tptz:ProfileToken><tptz:Velocity>"
        "<tt:PanTilt x=\"%2\" y=\"%3\" xmlns:tt=\"http://www.onvif.org/ver10/schema\"/>"
        "<tt:Zoom x=\"%4\" xmlns:tt=\"http://www.onvif.org/ver10/schema\"/>"
        "</tptz:Velocity></tptz:ContinuousMove>")
        .arg(target.token.toHtmlEscaped())
        .arg(x, 0, 'f', 2).arg(y, 0, 'f', 2).arg(zoom, 0, 'f', 2);
    send(nam, target, body);
}

void ptzStop(QNetworkAccessManager *nam, const PtzTarget &target)
{
    if (!target.valid())
        return;
    const QString body = QStringLiteral(
        "<tptz:Stop><tptz:ProfileToken>%1</tptz:ProfileToken>"
        "<tptz:PanTilt>true</tptz:PanTilt><tptz:Zoom>true</tptz:Zoom></tptz:Stop>")
        .arg(target.token.toHtmlEscaped());
    send(nam, target, body);
}

void ptzGotoPreset(QNetworkAccessManager *nam, const PtzTarget &target, const QString &presetToken, double speedPan, double speedTilt, double speedZoom)
{
    if (!target.valid() || presetToken.trimmed().isEmpty())
        return;
    const QString body = QStringLiteral(
        "<tptz:GotoPreset><tptz:ProfileToken>%1</tptz:ProfileToken>"
        "<tptz:PresetToken>%2</tptz:PresetToken>"
        "<tptz:Speed><tt:PanTilt x=\"%3\" y=\"%4\" xmlns:tt=\"http://www.onvif.org/ver10/schema\"/>"
        "<tt:Zoom x=\"%5\" xmlns:tt=\"http://www.onvif.org/ver10/schema\"/></tptz:Speed>"
        "</tptz:GotoPreset>")
        .arg(target.token.toHtmlEscaped())
        .arg(presetToken.trimmed().toHtmlEscaped())
        .arg(speedPan, 0, 'f', 2)
        .arg(speedTilt, 0, 'f', 2)
        .arg(speedZoom, 0, 'f', 2);
    send(nam, target, body);
}

void ptzSetPreset(QNetworkAccessManager *nam, const PtzTarget &target, const QString &presetName, const QString &presetToken)
{
    if (!target.valid())
        return;
    QString tokenPart;
    if (!presetToken.trimmed().isEmpty())
        tokenPart = QStringLiteral("<tptz:PresetToken>%1</tptz:PresetToken>").arg(presetToken.trimmed().toHtmlEscaped());
    QString namePart;
    if (!presetName.trimmed().isEmpty())
        namePart = QStringLiteral("<tptz:PresetName>%1</tptz:PresetName>").arg(presetName.trimmed().toHtmlEscaped());
    const QString body = QStringLiteral(
        "<tptz:SetPreset><tptz:ProfileToken>%1</tptz:ProfileToken>%2%3</tptz:SetPreset>")
        .arg(target.token.toHtmlEscaped(), tokenPart, namePart);
    send(nam, target, body);
}

void ptzRemovePreset(QNetworkAccessManager *nam, const PtzTarget &target, const QString &presetToken)
{
    if (!target.valid() || presetToken.trimmed().isEmpty())
        return;
    const QString body = QStringLiteral(
        "<tptz:RemovePreset><tptz:ProfileToken>%1</tptz:ProfileToken>"
        "<tptz:PresetToken>%2</tptz:PresetToken></tptz:RemovePreset>")
        .arg(target.token.toHtmlEscaped())
        .arg(presetToken.trimmed().toHtmlEscaped());
    send(nam, target, body);
}


// ======================================================================
// core/portscan.cpp
// ======================================================================
PortScanner::PortScanner(QObject *parent) : QObject(parent) {}

void PortScanner::start(quint32 firstIp, quint32 lastIp, quint16 port, int concurrency, int timeoutMs)
{
    stop();
    ++m_gen;
    m_next = firstIp;
    m_last = lastIp;
    m_port = port;
    m_conc = qMax(1, concurrency);
    m_timeout = timeoutMs;
    m_total = int(m_last - m_next + 1);
    m_done = 0;
    m_active = 0;
    m_running = true;
    pump();
}

void PortScanner::stop()
{
    m_running = false;
    ++m_gen;
}

void PortScanner::pump()
{
    while (m_running && m_active < m_conc && m_next <= m_last) {
        const QString ip = QHostAddress(quint32(m_next++)).toString();
        const int gen = m_gen;
        ++m_active;

        auto *sock = new QTcpSocket(this);
        auto *timer = new QTimer(sock);      // удалится вместе с сокетом
        timer->setSingleShot(true);
        auto done = std::make_shared<bool>(false);

        auto finish = [this, sock, gen, ip, done](bool open) {
            if (*done)
                return;
            *done = true;
            sock->abort();
            sock->deleteLater();
            if (gen != m_gen)
                return;
            --m_active;
            ++m_done;
            if (open) {
                emit found(ip);
                emit foundPort(ip, m_port);
            }
            emit progress(m_done, m_total);
            pump();
        };

        connect(sock, &QTcpSocket::connected, this, [finish] { finish(true); });
        connect(sock, &QAbstractSocket::errorOccurred, this,
                [finish](QAbstractSocket::SocketError) { finish(false); });
        connect(timer, &QTimer::timeout, this, [finish] { finish(false); });

        timer->start(m_timeout);
        sock->connectToHost(ip, m_port);
    }

    if (m_running && m_next > m_last && m_active == 0) {
        m_running = false;
        emit finished();
    }
}

// ======================================================================
// core/recording_index.cpp
// ======================================================================
namespace {

SegmentInfo rowToSegment(const QSqlQuery &q)
{
    SegmentInfo s;
    s.id = q.value(0).toLongLong();
    s.cameraId = q.value(1).toString();
    s.path = q.value(2).toString();
    s.startMs = q.value(3).toLongLong();
    s.endMs = q.value(4).isNull() ? 0 : q.value(4).toLongLong();
    s.sizeBytes = q.value(5).toLongLong();
    return s;
}

const char *kSelectColumns = "SELECT id, camera_id, path, start_ms, end_ms, size_bytes FROM segments ";

} // namespace

RecordingIndex::~RecordingIndex() { close(); }

bool RecordingIndex::open(const QString &dbPath)
{
    close();
    m_conn = QStringLiteral("vms_idx_") + QUuid::createUuid().toString(QUuid::Id128);

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        m_lastError = db.lastError().text();
        return false;
    }

    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    // FULL снижает риск потери последних изменений индекса при внезапном отключении питания.
    q.exec(QStringLiteral("PRAGMA synchronous=FULL"));
    q.exec(QStringLiteral("PRAGMA busy_timeout=10000"));

    const bool ok =
        q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS segments ("
            " id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            " camera_id  TEXT    NOT NULL,"
            " path       TEXT    NOT NULL,"
            " start_ms   INTEGER NOT NULL,"
            " end_ms     INTEGER,"
            " size_bytes INTEGER NOT NULL DEFAULT 0)"))
        && q.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_segments_cam_time "
            "ON segments(camera_id, start_ms)"))
        && q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS downtime_log ("
            " id          INTEGER PRIMARY KEY AUTOINCREMENT,"
            " camera_id   TEXT    NOT NULL,"
            " start_ms    INTEGER NOT NULL,"
            " end_ms      INTEGER NOT NULL,"
            " duration_ms INTEGER NOT NULL,"
            " reason      TEXT    NOT NULL DEFAULT '')"))
        && q.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_downtime_cam_start "
            "ON downtime_log(camera_id, start_ms)"));
    if (!ok)
        m_lastError = q.lastError().text();
    return ok;
}

namespace {

void purgeClosedSegmentsByPolicy(const QString &connName, const QString &storageRoot, qint64 minFreeBytes)
{
    if (connName.isEmpty())
        return;
    QSettings s;
    const bool autoCleanup = s.value(QStringLiteral("storage/autoCleanup"), true).toBool();
    const int retentionDays = qBound(0, s.value(QStringLiteral("storage/retentionDays"), 30).toInt(), 3650);
    if (!autoCleanup)
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 cutoffMs = (retentionDays > 0)
        ? (nowMs - qint64(retentionDays) * 86400LL * 1000LL)
        : 0LL;

    struct SegRow {
        qint64 id;
        QString cameraId;
        QString path;
        qint64 endMs;
        qint64 sizeBytes;
    };

    QList<SegRow> candidates;
    {
        QSqlDatabase db = QSqlDatabase::database(connName, false);
        if (!db.isOpen())
            return;
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT id, camera_id, path, end_ms, size_bytes FROM segments "
            "WHERE end_ms IS NOT NULL AND end_ms > 0 "
            "ORDER BY end_ms ASC, id ASC"));
        if (!q.exec())
            return;
        while (q.next()) {
            candidates.append({
                q.value(0).toLongLong(),
                q.value(1).toString(),
                q.value(2).toString(),
                q.value(3).toLongLong(),
                q.value(4).toLongLong()
            });
        }
    }

    QStorageInfo storage(storageRoot);
    storage.refresh();
    qint64 availBytes = (storage.isValid() && storage.isReady()) ? storage.bytesAvailable() : -1LL;

    QList<qint64> deletedIds;
    for (const SegRow &row : std::as_const(candidates)) {
        const bool expiredByAge = (cutoffMs > 0 && row.endMs < cutoffMs);
        const bool lowDiskSpace = (availBytes >= 0 && minFreeBytes > 0 && availBytes < minFreeBytes);
        if (!expiredByAge && !lowDiskSpace)
            break;

        qint64 reclaimed = 0;
        if (!row.path.isEmpty()) {
            QFileInfo fi(row.path);
            if (fi.exists()) {
                reclaimed = qMax<qint64>(0, fi.size());
                QFile::remove(row.path);
            }
        }
        if (reclaimed <= 0 && row.sizeBytes > 0)
            reclaimed = row.sizeBytes;

        deletedIds.append(row.id);
        if (availBytes >= 0) {
            storage.refresh();
            if (storage.isValid() && storage.isReady())
                availBytes = storage.bytesAvailable();
            else
                availBytes += reclaimed;
        }
    }

    if (!deletedIds.isEmpty()) {
        QSqlDatabase db = QSqlDatabase::database(connName, false);
        if (!db.isOpen())
            return;
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM segments WHERE id = ?"));
        for (qint64 id : std::as_const(deletedIds)) {
            del.addBindValue(id);
            del.exec();
        }
    }
}

void recordDowntimeEvent(const QString &connName, const QString &cameraId, qint64 startMs, qint64 endMs, const QString &reason)
{
    if (connName.isEmpty() || cameraId.isEmpty() || endMs <= startMs)
        return;
    QSqlDatabase db = QSqlDatabase::database(connName, false);
    if (!db.isOpen())
        return;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO downtime_log(camera_id, start_ms, end_ms, duration_ms, reason) "
        "VALUES(?, ?, ?, ?, ?)"));
    q.addBindValue(cameraId);
    q.addBindValue(startMs);
    q.addBindValue(endMs);
    q.addBindValue(endMs - startMs);
    q.addBindValue(reason);
    q.exec();
}

} // namespace

void RecordingIndex::close()
{
    if (m_conn.isEmpty())
        return;
    {
        QSqlDatabase db = QSqlDatabase::database(m_conn, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_conn);
    m_conn.clear();
}

qint64 RecordingIndex::beginSegment(const QString &cameraId, const QString &path, qint64 startMs)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("INSERT INTO segments(camera_id, path, start_ms) VALUES(?, ?, ?)"));
    q.addBindValue(cameraId);
    q.addBindValue(path);
    q.addBindValue(startMs);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

bool RecordingIndex::endSegment(qint64 id, qint64 endMs, qint64 sizeBytes)
{
    if (id < 0)
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("UPDATE segments SET end_ms = ?, size_bytes = ? WHERE id = ?"));
    q.addBindValue(endMs);
    q.addBindValue(sizeBytes);
    q.addBindValue(id);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

void RecordingIndex::closeDanglingSegments(const QString &cameraId)
{
    struct Row { qint64 id; QString path; qint64 startMs; };
    QList<Row> rows;
    {
        QSqlQuery q(QSqlDatabase::database(m_conn));
        q.prepare(QStringLiteral(
            "SELECT id, path, start_ms FROM segments WHERE camera_id = ? AND end_ms IS NULL"));
        q.addBindValue(cameraId);
        if (!q.exec()) {
            m_lastError = q.lastError().text();
            return;
        }
        while (q.next())
            rows.append({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toLongLong()});
    }

    for (const Row &r : rows) {
        const QFileInfo fi(r.path);
        QSqlQuery q(QSqlDatabase::database(m_conn));
        if (fi.exists() && fi.size() > 0) {
            const qint64 endMs = qMax(fi.lastModified().toMSecsSinceEpoch(), r.startMs);
            q.prepare(QStringLiteral("UPDATE segments SET end_ms = ?, size_bytes = ? WHERE id = ?"));
            q.addBindValue(endMs);
            q.addBindValue(fi.size());
            q.addBindValue(r.id);
        } else {
            q.prepare(QStringLiteral("DELETE FROM segments WHERE id = ?"));
            q.addBindValue(r.id);
        }
        q.exec();
    }
}

QList<SegmentInfo> RecordingIndex::segmentsInRange(const QString &cameraId, qint64 fromMs, qint64 toMs)
{
    QList<SegmentInfo> out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QString::fromLatin1(kSelectColumns) +
              QStringLiteral("WHERE camera_id = ? AND start_ms < ? "
                             "AND (end_ms IS NULL OR end_ms > ?) ORDER BY start_ms"));
    q.addBindValue(cameraId);
    q.addBindValue(toMs);
    q.addBindValue(fromMs);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(rowToSegment(q));
    return out;
}

SegmentInfo RecordingIndex::segmentAt(const QString &cameraId, qint64 ms)
{
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QString::fromLatin1(kSelectColumns) +
              QStringLiteral("WHERE camera_id = ? AND start_ms <= ? "
                             "ORDER BY start_ms DESC LIMIT 1"));
    q.addBindValue(cameraId);
    q.addBindValue(ms);
    if (q.exec() && q.next()) {
        const SegmentInfo s = rowToSegment(q);
        if (s.endMs == 0 || ms <= s.endMs)
            return s;
    }
    return {};
}

namespace {

QString maskCredentialsInUrl(const QString &rawUrl)
{
    const QString trimmed = rawUrl.trimmed();
    if (trimmed.isEmpty())
        return QString();
    QUrl u(trimmed);
    if (u.isValid() && !u.scheme().isEmpty() && !u.host().isEmpty()) {
        if (!u.userInfo().isEmpty()) {
            const QString user = u.userName();
            u.setUserInfo(user.isEmpty() ? QStringLiteral("***") : (user + QStringLiteral(":***")));
        }
        return u.toString(QUrl::FullyEncoded);
    }
    const int schemeEnd = trimmed.indexOf(QStringLiteral("://"));
    const int atPos = (schemeEnd >= 0) ? trimmed.indexOf(QLatin1Char('@'), schemeEnd + 3) : trimmed.indexOf(QLatin1Char('@'));
    const int slashPos = (schemeEnd >= 0) ? trimmed.indexOf(QLatin1Char('/'), schemeEnd + 3) : trimmed.indexOf(QLatin1Char('/'));
    if (atPos > 0 && (slashPos < 0 || atPos < slashPos)) {
        const QString prefix = (schemeEnd >= 0) ? trimmed.left(schemeEnd + 3) : QString();
        const QString authPart = trimmed.mid(prefix.size(), atPos - prefix.size());
        const QString rest = trimmed.mid(atPos);
        const int colon = authPart.indexOf(QLatin1Char(':'));
        const QString maskedAuth = (colon >= 0)
            ? (authPart.left(colon) + QStringLiteral(":***"))
            : QStringLiteral("***");
        return prefix + maskedAuth + rest;
    }
    return trimmed;
}

} // namespace

namespace {

bool writeArchiveRangeManifest(const QString &cameraId,
                               const QString &outputPath,
                               qint64 fromMs,
                               qint64 toMs,
                               const QList<SegmentInfo> &segments,
                               QString *errorOut)
{
    if (outputPath.trimmed().isEmpty()) {
        if (errorOut) *errorOut = T("Не указан путь сохранения фрагмента архива.");
        return false;
    }
    if (toMs <= fromMs) {
        if (errorOut) *errorOut = T("Конец интервала должен быть позже начала.");
        return false;
    }
    QJsonArray segArray;
    for (const SegmentInfo &s : segments) {
        QJsonObject obj;
        obj.insert(QStringLiteral("path"), s.path);
        obj.insert(QStringLiteral("startMs"), s.startMs);
        obj.insert(QStringLiteral("endMs"), s.endMs);
        obj.insert(QStringLiteral("sizeBytes"), s.sizeBytes);
        segArray.append(obj);
    }
    QJsonObject root;
    root.insert(QStringLiteral("cameraId"), cameraId);
    root.insert(QStringLiteral("fromMs"), fromMs);
    root.insert(QStringLiteral("toMs"), toMs);
    root.insert(QStringLiteral("exportedAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    root.insert(QStringLiteral("segmentCount"), segments.size());
    root.insert(QStringLiteral("segments"), segArray);

    QFile f(outputPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = T("Не удалось записать файл экспорта: %1").arg(f.errorString());
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace

namespace {

QString saveScheduledSnapshot(const QString &screenshotDir, const QString &cameraId, const QImage &frame, QString *errorOut)
{
    if (frame.isNull()) {
        if (errorOut) *errorOut = T("Пустой кадр для снимка.");
        return QString();
    }
    const QString root = screenshotDir.trimmed().isEmpty() ? Cfg::screenshotDir() : screenshotDir.trimmed();
    const QString dateSubdir = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd"));
    const QString safeCam = cameraId.trimmed().isEmpty() ? QStringLiteral("camera") : cameraId.trimmed();
    const QDir dir(QDir(root).filePath(QStringLiteral("scheduled/") + dateSubdir));
    if (!QDir().mkpath(dir.absolutePath())) {
        if (errorOut) *errorOut = T("Не удалось создать папку снимков: %1").arg(dir.absolutePath());
        return QString();
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HHmmss_zzz"));
    const QString filePath = dir.filePath(QStringLiteral("%1_%2.jpg").arg(safeCam, stamp));
    if (!frame.save(filePath, "JPG", 88)) {
        if (errorOut) *errorOut = T("Не удалось сохранить снимок: %1").arg(filePath);
        return QString();
    }
    return filePath;
}

} // namespace

namespace {

bool canCurrentUserPerformAction(const QString &actionKey)
{
    const UserInfo u = Users::current();
    if (u.isAdmin())
        return true;
    if (actionKey == QLatin1String("ptz.control")
        || actionKey == QLatin1String("events.ack")
        || actionKey == QLatin1String("snapshot.save")) {
        return !u.id.isEmpty();
    }
    return false;
}

} // namespace

// ======================================================================
// core/rtsp_probe.cpp
// ======================================================================
namespace {

struct RtspTemplate {
    const char *mainPath;
    const char *subPath;
};

// %CH% — номер канала, %ID1% / %ID2% — канал*100+1 / +2, %U% и %P% — логин и пароль
const RtspTemplate kRtspTemplates[] = {
    {"/Streaming/Channels/%ID1%", "/Streaming/Channels/%ID2%"},                       // Hikvision, HiWatch (новые)
    {"/h264/ch%CH%/main/av_stream", "/h264/ch%CH%/sub/av_stream"},                    // Hikvision, HiWatch (старые)
    {"/ISAPI/Streaming/channels/%ID1%", "/ISAPI/Streaming/channels/%ID2%"},
    {"/PSIA/Streaming/channels/%CH%", ""},                                            // очень старые
    {"/mpeg4/ch%CH%/main/av_stream", "/mpeg4/ch%CH%/sub/av_stream"},
    {"/cam/realmonitor?channel=%CH%&subtype=0", "/cam/realmonitor?channel=%CH%&subtype=1"},   // Dahua и OEM
    {"/user=%U%&password=%P%&channel=%CH%&stream=0.sdp?real_stream",
     "/user=%U%&password=%P%&channel=%CH%&stream=1.sdp?real_stream"},                 // Xiongmai (iCSee, часть Polyvision)
    {"/stream1", "/stream2"},
    {"/live/main", "/live/sub"},
    {"/11", "/12"},
    {"/1/h264major", "/1/h264minor"},
};
constexpr int kRtspTemplateCount = int(sizeof(kRtspTemplates) / sizeof(kRtspTemplates[0]));

QString expandRtspPath(const char *fmt, int channel, const QString &user, const QString &pass)
{
    QString s = QString::fromLatin1(fmt);
    s.replace(QLatin1String("%ID1%"), QString::number(channel * 100 + 1));
    s.replace(QLatin1String("%ID2%"), QString::number(channel * 100 + 2));
    s.replace(QLatin1String("%CH%"), QString::number(channel));
    s.replace(QLatin1String("%U%"), QString::fromUtf8(QUrl::toPercentEncoding(user)));
    s.replace(QLatin1String("%P%"), QString::fromUtf8(QUrl::toPercentEncoding(pass)));
    return s;
}

QByteArray md5Hex(const QByteArray &d)
{
    return QCryptographicHash::hash(d, QCryptographicHash::Md5).toHex();
}

QString headerParam(const QString &www, const QString &name)
{
    const QRegularExpression re(name + QStringLiteral("=\"([^\"]*)\""), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(www);
    return m.hasMatch() ? m.captured(1) : QString();
}

} // namespace

RtspProbe::RtspProbe(QObject *parent) : QObject(parent) {}

int RtspProbe::templateCount()
{
    return kRtspTemplateCount;
}

QString RtspProbe::buildUrl(int tmpl, const QString &host, quint16 port, const QString &user,
                            const QString &pass, int channel, bool sub)
{
    if (tmpl < 0 || tmpl >= kRtspTemplateCount)
        return QString();
    const char *fmt = sub ? kRtspTemplates[tmpl].subPath : kRtspTemplates[tmpl].mainPath;
    if (!fmt || !*fmt)
        return QString();
    const bool credsInPath = QString::fromLatin1(fmt).contains(QLatin1String("%U%"));
    QString url = QStringLiteral("rtsp://");
    if (!credsInPath && !user.isEmpty())
        url += QString::fromUtf8(QUrl::toPercentEncoding(user)) + QLatin1Char(':')
               + QString::fromUtf8(QUrl::toPercentEncoding(pass)) + QLatin1Char('@');
    url += QStringLiteral("%1:%2").arg(host).arg(port) + expandRtspPath(fmt, channel, user, pass);
    return url;
}

void RtspProbe::start(const QString &host, quint16 port, const QString &user, const QString &pass)
{
    cancel();
    m_host = host;
    m_port = port ? port : 554;
    m_user = user;
    m_pass = pass;
    m_index = 0;
    tryNext();
}

void RtspProbe::cancel()
{
    ++m_gen;
}

void RtspProbe::tryNext()
{
    if (m_index >= kRtspTemplateCount) {
        emit finished(false, -1, QStringLiteral("none"));
        return;
    }
    request(m_index, QByteArray(), false);
}

void RtspProbe::request(int tmpl, const QByteArray &authHeader, bool authed)
{
    const int gen = m_gen;
    const QString path = expandRtspPath(kRtspTemplates[tmpl].mainPath, 1, m_user, m_pass);
    m_lastUri = QStringLiteral("rtsp://%1:%2").arg(m_host).arg(m_port) + path;

    QByteArray req = "DESCRIBE " + m_lastUri.toUtf8() + " RTSP/1.0\r\nCSeq: 2\r\n"
                     "User-Agent: PSVMS\r\nAccept: application/sdp\r\n";
    if (!authHeader.isEmpty())
        req += authHeader + "\r\n";
    req += "\r\n";

    auto *sock = new QTcpSocket(this);
    auto buf = std::make_shared<QByteArray>();
    auto done = std::make_shared<bool>(false);
    auto finish = [this, sock, tmpl, authed, gen, done](int code, const QString &www) {
        if (*done)
            return;
        *done = true;
        sock->abort();
        sock->deleteLater();
        if (gen != m_gen)
            return;
        onResponse(tmpl, authed, code, www);
    };

    connect(sock, &QTcpSocket::connected, this, [sock, req] { sock->write(req); });
    connect(sock, &QTcpSocket::readyRead, this, [sock, buf, finish] {
        buf->append(sock->readAll());
        const int end = buf->indexOf("\r\n\r\n");
        if (end < 0)
            return;
        const QList<QByteArray> lines = buf->left(end).split('\n');
        int code = -1;
        QString www;
        if (!lines.isEmpty()) {
            const QList<QByteArray> parts = lines.first().trimmed().split(' ');
            if (parts.size() >= 2)
                code = parts.at(1).toInt();
        }
        for (const QByteArray &ln : lines) {
            if (ln.trimmed().toLower().startsWith("www-authenticate:")) {
                if (www.isEmpty() || ln.contains("Digest"))
                    www = QString::fromLatin1(ln.mid(ln.indexOf(':') + 1).trimmed());
            }
        }
        finish(code, www);
    });
    connect(sock, &QAbstractSocket::errorOccurred, this,
            [finish](QAbstractSocket::SocketError) { finish(-1, QString()); });
    QTimer::singleShot(2500, sock, [finish] { finish(-1, QString()); });
    sock->connectToHost(m_host, m_port);
}

void RtspProbe::onResponse(int tmpl, bool authed, int code, const QString &www)
{
    if (code == 200) {
        emit finished(true, tmpl, QString());
        return;
    }
    if (code == 401) {
        if (authed || m_user.isEmpty()) {
            emit finished(false, -1, QStringLiteral("auth"));
            return;
        }
        const QByteArray header = buildAuthHeader(www);
        if (header.isEmpty()) {
            emit finished(false, -1, QStringLiteral("auth"));
            return;
        }
        request(tmpl, header, true);
        return;
    }
    if (code < 0) {                       // нет ответа: порт закрыт или камера недоступна
        emit finished(false, -1, QStringLiteral("none"));
        return;
    }
    ++m_index;                            // 404 и другие ответы: путь не подходит, пробуем следующий
    tryNext();
}

QByteArray RtspProbe::buildAuthHeader(const QString &www) const
{
    if (www.startsWith(QLatin1String("Basic"), Qt::CaseInsensitive)) {
        const QByteArray cred = (m_user + QLatin1Char(':') + m_pass).toUtf8().toBase64();
        return "Authorization: Basic " + cred;
    }
    if (!www.startsWith(QLatin1String("Digest"), Qt::CaseInsensitive))
        return QByteArray();

    const QString realm = headerParam(www, QStringLiteral("realm"));
    const QString nonce = headerParam(www, QStringLiteral("nonce"));
    const QString qop = headerParam(www, QStringLiteral("qop"));
    const QByteArray ha1 = md5Hex((m_user + QLatin1Char(':') + realm + QLatin1Char(':') + m_pass).toUtf8());
    const QByteArray ha2 = md5Hex("DESCRIBE:" + m_lastUri.toUtf8());

    QByteArray response, extra;
    if (qop.contains(QLatin1String("auth"))) {
        const QByteArray cnonce = QByteArray::number(QRandomGenerator::global()->generate(), 16);
        response = md5Hex(ha1 + ':' + nonce.toUtf8() + ":00000001:" + cnonce + ":auth:" + ha2);
        extra = ", qop=auth, nc=00000001, cnonce=\"" + cnonce + "\"";
    } else {
        response = md5Hex(ha1 + ':' + nonce.toUtf8() + ':' + ha2);
    }
    return "Authorization: Digest username=\"" + m_user.toUtf8() + "\", realm=\"" + realm.toUtf8()
           + "\", nonce=\"" + nonce.toUtf8() + "\", uri=\"" + m_lastUri.toUtf8() + "\", response=\""
           + response + "\"" + extra;
}

// ======================================================================
// core/secure_store.cpp
// ======================================================================
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

namespace {

constexpr int kKeyBytes = 32;

QString keyFilePath()
{
    return AppPaths::dataDir() + QStringLiteral("/.psvms_key");
}

void hideFile(const QString &path)
{
#ifdef Q_OS_WIN
    SetFileAttributesW(reinterpret_cast<const wchar_t *>(path.utf16()), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
#else
    Q_UNUSED(path)
#endif
}

// Ключ создаётся при первом обращении и живёт отдельным файлом рядом с настройками.
const QByteArray &encryptionKey()
{
    static const QByteArray key = [] {
        QDir().mkpath(AppPaths::dataDir());
        const QString path = keyFilePath();
        QFile f(path);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            const QByteArray data = QByteArray::fromBase64(f.readAll());
            if (data.size() == kKeyBytes)
                return data;
        }
        QByteArray fresh(kKeyBytes, '\0');
        for (char &c : fresh)
            c = char(QRandomGenerator::system()->bounded(256));
        QFile out(path);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(fresh.toBase64());
            out.close();
            hideFile(path);
        }
        return fresh;
    }();
    return key;
}

// Ключевой поток произвольной длины из SHA-256(ключ || номер блока)
QByteArray keystream(const QByteArray &key, int length)
{
    QByteArray out;
    out.reserve(length);
    quint32 counter = 0;
    while (out.size() < length) {
        QByteArray block = key;
        block.append(reinterpret_cast<const char *>(&counter), sizeof(counter));
        out += QCryptographicHash::hash(block, QCryptographicHash::Sha256);
        ++counter;
    }
    out.truncate(length);
    return out;
}

} // namespace

namespace {

QByteArray legacyDecryptBytes(const QByteArray &data)
{
    const QByteArray ks = keystream(encryptionKey(), data.size());
    QByteArray out(data.size(), '\0');
    for (int i = 0; i < data.size(); ++i)
        out[i] = data[i] ^ ks[i];
    return out;
}

#ifdef Q_OS_WIN
bool dpapiProtect(const QByteArray &input, QByteArray *output)
{
    if (!output || input.isEmpty()) return false;
    DATA_BLOB inBlob;
    inBlob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(input.constData()));
    inBlob.cbData = static_cast<DWORD>(input.size());
    DATA_BLOB outBlob{};
    static const wchar_t description[] = L"PSVMS camera credentials";
    if (!CryptProtectData(&inBlob, description, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &outBlob))
        return false;
    *output = QByteArray(reinterpret_cast<const char *>(outBlob.pbData), int(outBlob.cbData));
    SecureZeroMemory(outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}

bool dpapiUnprotect(const QByteArray &input, QByteArray *output)
{
    if (!output || input.isEmpty()) return false;
    DATA_BLOB inBlob;
    inBlob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(input.constData()));
    inBlob.cbData = static_cast<DWORD>(input.size());
    DATA_BLOB outBlob{};
    if (!CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &outBlob))
        return false;
    *output = QByteArray(reinterpret_cast<const char *>(outBlob.pbData), int(outBlob.cbData));
    SecureZeroMemory(outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}
#endif

} // namespace

QString SecureStore::encrypt(const QString &plain)
{
    if (plain.isEmpty()) return QString();
    const QByteArray data = plain.toUtf8();
#ifdef Q_OS_WIN
    QByteArray protectedData;
    if (!dpapiProtect(data, &protectedData)) {
        writePsvmsLog(QStringLiteral("SecureStore: DPAPI encryption failed; using legacy compatibility encryption."));
        const QByteArray ks = keystream(encryptionKey(), data.size());
        QByteArray out(data.size(), '\0');
        for (int i = 0; i < data.size(); ++i) out[i] = data[i] ^ ks[i];
        return QStringLiteral("legacy:v1:") + QString::fromLatin1(out.toBase64());
    }
    return QString::fromLatin1(protectedData.toBase64());
#else
    // На других платформах сохраняем совместимость, но не выдаём это за DPAPI.
    const QByteArray ks = keystream(encryptionKey(), data.size());
    QByteArray out(data.size(), '\0');
    for (int i = 0; i < data.size(); ++i) out[i] = data[i] ^ ks[i];
    return QStringLiteral("legacy:v1:") + QString::fromLatin1(out.toBase64());
#endif
}

QString SecureStore::decrypt(const QString &cipherB64)
{
    if (cipherB64.isEmpty()) return QString();
    const QByteArray data = QByteArray::fromBase64(cipherB64.toLatin1());
    if (data.isEmpty()) return QString();
#ifdef Q_OS_WIN
    QByteArray plain;
    if (!dpapiUnprotect(data, &plain)) {
        writePsvmsLog(QStringLiteral("SecureStore: DPAPI decryption failed; refusing to treat the value as plaintext."));
        return QString();
    }
    const QString result = QString::fromUtf8(plain);
    SecureZeroMemory(plain.data(), size_t(plain.size()));
    return result;
#else
    const QByteArray plain = legacyDecryptBytes(data);
    return QString::fromUtf8(plain);
#endif
}

QString SecureStore::decryptLegacy(const QString &cipherB64)
{
    if (cipherB64.isEmpty()) return QString();
    const QByteArray data = QByteArray::fromBase64(cipherB64.toLatin1());
    if (data.isEmpty()) return QString();
    const QByteArray plain = legacyDecryptBytes(data);
    return QString::fromUtf8(plain);
}

// ======================================================================
// core/stream_presets.cpp
// ======================================================================
namespace {

QString kindName(int index)   // 0 — основной, 1 — подпоток, 2 — третий
{
    switch (index) {
    case 0:  return T("Основной поток");
    case 1:  return T("Подпоток");
    default: return T("Третий поток");
    }
}

} // namespace

QList<StreamOption> streamOptionsFor(const QString &url)
{
    QList<StreamOption> out;
    if (url.isEmpty())
        return out;

    // Hikvision / HiWatch: номер = канал * 100 + поток (1 — основной, 2 — подпоток, 3 — третий)
    static const QRegularExpression hik(QStringLiteral("^(.*/Streaming/Channels/)(\\d+)(.*)$"),
                                        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m = hik.match(url);
    if (m.hasMatch()) {
        const int channel = m.captured(2).toInt() / 100;
        if (channel >= 1) {
            for (int s = 1; s <= 3; ++s) {
                const int id = channel * 100 + s;
                StreamOption o;
                o.label = kindName(s - 1) + QStringLiteral(" (%1)").arg(id);
                o.url = m.captured(1) + QString::number(id) + m.captured(3);
                out.append(o);
            }
            return out;
        }
    }

    // Старые Hikvision / HiWatch: .../h264/ch1/main/av_stream и .../sub/av_stream
    static const QRegularExpression oldHik(QStringLiteral("^(.*/(?:h264|mpeg4)/ch\\d+/)(main|sub)(/av_stream.*)$"));
    m = oldHik.match(url);
    if (m.hasMatch()) {
        const char *kinds[2] = {"main", "sub"};
        for (int s = 0; s <= 1; ++s) {
            StreamOption o;
            o.label = kindName(s) + QStringLiteral(" (%1)").arg(QLatin1String(kinds[s]));
            o.url = m.captured(1) + QLatin1String(kinds[s]) + m.captured(3);
            out.append(o);
        }
        return out;
    }

    // Dahua и совместимые: ...subtype=0 / 1 / 2
    static const QRegularExpression dahua(QStringLiteral("^(.*[?&]subtype=)(\\d)(.*)$"));
    m = dahua.match(url);
    if (m.hasMatch()) {
        for (int s = 0; s <= 2; ++s) {
            StreamOption o;
            o.label = kindName(s) + QStringLiteral(" (subtype=%1)").arg(s);
            o.url = m.captured(1) + QString::number(s) + m.captured(3);
            out.append(o);
        }
        return out;
    }

    // iCSee / Xiongmai: ...stream=0 (основной) или stream=1 (подпоток)
    static const QRegularExpression icsee(QStringLiteral("^(.*[?&/]stream=)(\\d)(.*)$"));
    m = icsee.match(url);
    if (m.hasMatch()) {
        for (int s = 0; s <= 1; ++s) {
            StreamOption o;
            o.label = kindName(s) + QStringLiteral(" (stream=%1)").arg(s);
            o.url = m.captured(1) + QString::number(s) + m.captured(3);
            out.append(o);
        }
        return out;
    }

    // Reolink: ...h264Preview_01_main / _sub
    static const QRegularExpression reolink(QStringLiteral("^(.*h264Preview_\\d+_)(main|sub)(.*)$"),
                                            QRegularExpression::CaseInsensitiveOption);
    m = reolink.match(url);
    if (m.hasMatch()) {
        const char *kinds[2] = {"main", "sub"};
        for (int s = 0; s <= 1; ++s) {
            StreamOption o;
            o.label = kindName(s) + QStringLiteral(" (%1)").arg(QLatin1String(kinds[s]));
            o.url = m.captured(1) + QLatin1String(kinds[s]) + m.captured(3);
            out.append(o);
        }
        return out;
    }

    // Uniview: .../unicast/c1/s0/live и .../s1/live
    static const QRegularExpression uniview(QStringLiteral("^(.*\\/unicast\\/c\\d+\\/s)([01])(\\/live.*)$"),
                                            QRegularExpression::CaseInsensitiveOption);
    m = uniview.match(url);
    if (m.hasMatch()) {
        for (int s = 0; s <= 1; ++s) {
            StreamOption o;
            o.label = kindName(s) + QStringLiteral(" (s%1)").arg(s);
            o.url = m.captured(1) + QString::number(s) + m.captured(3);
            out.append(o);
        }
        return out;
    }

    // Axis: ...media.amp?streamprofile=Quality
    static const QRegularExpression axis(QStringLiteral("^(.*[?&]streamprofile=)([^&]+)(.*)$"),
                                         QRegularExpression::CaseInsensitiveOption);
    m = axis.match(url);
    if (m.hasMatch()) {
        const QPair<QString, QString> profiles[2] = {
            {kindName(0) + QStringLiteral(" (Quality)"), QStringLiteral("Quality")},
            {kindName(1) + QStringLiteral(" (Mobile)"),  QStringLiteral("Mobile")}
        };
        for (int s = 0; s < 2; ++s) {
            StreamOption o;
            o.label = profiles[s].first;
            o.url = m.captured(1) + profiles[s].second + m.captured(3);
            out.append(o);
        }
        return out;
    }
    return out;
}

// ======================================================================
// core/sys_stats.cpp
// ======================================================================
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif

namespace {
// Разница между двумя замерами FILETIME в «тиках» по 100 нс
qint64 ftDiff(const FILETIME &a, const FILETIME &b)
{
    auto toI64 = [](const FILETIME &f) { return (qint64(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return toI64(b) - toI64(a);
}
} // namespace

SysStats readSysStats()
{
    SysStats s;

    static FILETIME prevIdle{}, prevKernel{}, prevUser{};
    static bool havePrev = false;
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        if (havePrev) {
            const qint64 idleDiff = ftDiff(prevIdle, idle);
            const qint64 kernelDiff = ftDiff(prevKernel, kernel);
            const qint64 userDiff = ftDiff(prevUser, user);
            const qint64 total = kernelDiff + userDiff;   // kernel-время уже включает idle
            if (total > 0)
                s.cpuPercent = 100.0 * double(total - idleDiff) / double(total);
        }
        prevIdle = idle;
        prevKernel = kernel;
        prevUser = user;
        havePrev = true;
    }

    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        s.memPercent = double(mem.dwMemoryLoad);
        s.memTotalMb = qint64(mem.ullTotalPhys / (1024 * 1024));
        s.memUsedMb = s.memTotalMb - qint64(mem.ullAvailPhys / (1024 * 1024));
    }
    return s;
}
#else
SysStats readSysStats()
{
    return SysStats();   // сборка вне Windows: показатели недоступны
}
#endif

// ======================================================================
// core/users.cpp
// ======================================================================
namespace {

constexpr int kUserIterations = 600000;
constexpr int kUserKeyLen = 32;
QString g_currentUserId;

struct UserRec {
    QString id, name, role, salt, hash, remember;
    bool enabled = true;
    int iterations = kUserIterations;
};

QList<UserRec> loadUserRecs()
{
    QList<UserRec> out;
    QSettings s;
    const int n = s.beginReadArray(QStringLiteral("users"));
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        UserRec r;
        r.id = s.value(QStringLiteral("id")).toString();
        r.name = s.value(QStringLiteral("name")).toString();
        r.role = s.value(QStringLiteral("role")).toString();
        r.salt = s.value(QStringLiteral("salt")).toString();
        r.hash = s.value(QStringLiteral("hash")).toString();
        r.remember = s.value(QStringLiteral("remember")).toString();
        r.enabled = s.value(QStringLiteral("enabled"), true).toBool();
        r.iterations = s.value(QStringLiteral("iterations"), kUserIterations).toInt();
        if (!r.id.isEmpty())
            out.append(r);
    }
    s.endArray();
    return out;
}

void saveUserRecs(const QList<UserRec> &v)
{
    QSettings s;
    s.remove(QStringLiteral("users"));
    s.beginWriteArray(QStringLiteral("users"), int(v.size()));
    for (int i = 0; i < v.size(); ++i) {
        s.setArrayIndex(i);
        const UserRec &r = v.at(i);
        s.setValue(QStringLiteral("id"), r.id);
        s.setValue(QStringLiteral("name"), r.name);
        s.setValue(QStringLiteral("role"), r.role);
        s.setValue(QStringLiteral("salt"), r.salt);
        s.setValue(QStringLiteral("hash"), r.hash);
        s.setValue(QStringLiteral("remember"), r.remember);
        s.setValue(QStringLiteral("enabled"), r.enabled);
        s.setValue(QStringLiteral("iterations"), r.iterations);
    }
    s.endArray();
    s.sync();
}

QByteArray randomUserBytes(int n)
{
    QByteArray b(n, '\0');
    for (char &c : b)
        c = char(QRandomGenerator::system()->bounded(256));
    return b;
}

QByteArray deriveUserKey(const QString &password, const QByteArray &salt, int iterations)
{
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, password.toUtf8(), salt,
                                              iterations, kUserKeyLen);
}

bool userBytesEqual(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i)) ^ static_cast<unsigned char>(b.at(i));
    return diff == 0;
}

QString makeUserId()
{
    return QStringLiteral("u") + QUuid::createUuid().toString(QUuid::Id128).left(8);
}

UserInfo toUserInfo(const UserRec &r)
{
    UserInfo u;
    u.id = r.id;
    u.name = r.name;
    u.role = r.role;
    u.hasPassword = !r.hash.isEmpty();
    u.enabled = r.enabled;
    return u;
}

void applyUserPassword(UserRec &r, const QString &password)
{
    r.remember.clear();
    if (password.isEmpty()) {
        r.salt.clear();
        r.hash.clear();
        return;
    }
    const QByteArray salt = randomUserBytes(16);
    r.iterations = kUserIterations;
    r.salt = QString::fromLatin1(salt.toBase64());
    r.hash = QString::fromLatin1(deriveUserKey(password, salt, r.iterations).toBase64());
}

QString tokenHash(const QString &token)
{
    return QString::fromLatin1(QCryptographicHash::hash(token.toLatin1(), QCryptographicHash::Sha256).toBase64());
}

} // namespace

QList<UserInfo> Users::all()
{
    QList<UserInfo> out;
    for (const UserRec &r : loadUserRecs())
        out.append(toUserInfo(r));
    return out;
}

UserInfo Users::find(const QString &id)
{
    for (const UserRec &r : loadUserRecs())
        if (r.id == id)
            return toUserInfo(r);
    return UserInfo();
}

void Users::ensureDefault()
{
    QList<UserRec> users = loadUserRecs();
    auto hasRole = [&users](const QString &role) {
        for (const UserRec &u : users)
            if (u.role == role) return true;
        return false;
    };
    if (!hasRole(QStringLiteral("guest"))) {
        UserRec guest;
        guest.id = makeUserId();
        guest.name = T("Гость");
        guest.role = QStringLiteral("guest");
        users.prepend(guest);
    }
    if (!hasRole(QStringLiteral("admin"))) {
        UserRec admin;
        admin.id = makeUserId();
        admin.name = QStringLiteral("Administrator");
        admin.role = QStringLiteral("admin");
        users.append(admin);
    } else {
        for (UserRec &u : users) {
            if (u.role == QStringLiteral("admin") &&
                (u.name.compare(QStringLiteral("Администратор"), Qt::CaseInsensitive) == 0 ||
                 u.name.compare(QStringLiteral("Administrator"), Qt::CaseInsensitive) == 0)) {
                u.name = QStringLiteral("Administrator");
                break;
            }
        }
    }
    if (!hasRole(QStringLiteral("developer"))) {
        UserRec developer;
        developer.id = makeUserId();
        developer.name = T("Разработчик");
        developer.role = QStringLiteral("developer");
        users.append(developer);
    }

    // Однократная миграция пароля непосредственно в запись пользователя «Разработчик».
    // В бинарнике находятся только соль и PBKDF2-HMAC-SHA256-хэш; пароль в открытом виде
    // не сохраняется. Хэш записывается в тот же массив users, что и остальные учётные записи,
    // поэтому он переживает перезапуск, смену раздела и обычное переключение пользователей.
    // Новая версия ключа миграции гарантирует обновление уже существующих установок.
    QSettings securitySettings;
    const QString migrationKey = QStringLiteral("security/developerAccountCredentialV2");
    if (!securitySettings.value(migrationKey, false).toBool()) {
        bool developerFound = false;
        for (UserRec &u : users) {
            if (u.role == QLatin1String("developer")) {
                u.salt = QStringLiteral("e50vSmweg1C3otnwQWzjWg==");
                u.hash = QStringLiteral("EvB7nrzng2YIjFQG+e/h0NAAKflcjR3IOhScz9dKUCE=");
                u.iterations = kUserIterations;
                u.remember.clear();
                u.enabled = true;
                developerFound = true;
                break;
            }
        }
        if (developerFound) {
            saveUserRecs(users);
            securitySettings.setValue(migrationKey, true);
            securitySettings.sync();
        }
    }
    for (UserRec &u : users) {
        if (u.role == QLatin1String("guest") || u.role == QLatin1String("developer"))
            u.enabled = true;
    }
    saveUserRecs(users);
}

bool Users::needsLoginDialog()
{
    return false; // по умолчанию запускаемся под Administrator без выбора пользователя
}

QString Users::add(const QString &name, const QString &role, const QString &password, QString *error)
{
    if (!Users::current().isAdmin()) { if (error) *error = T("Недостаточно прав: требуется администратор."); SecurityAudit::write(QStringLiteral("permission_denied"), QStringLiteral("users.add")); return QString(); }
    const QString n = name.trimmed();
    if (n.isEmpty()) {
        if (error) *error = T("Укажите имя пользователя.");
        return QString();
    }
    QList<UserRec> v = loadUserRecs();
    for (const UserRec &r : v) {
        if (r.name.compare(n, Qt::CaseInsensitive) == 0) {
            if (error) *error = T("Пользователь с таким именем уже есть.");
            return QString();
        }
    }
    if (!password.isEmpty() && password.size() < kMinPassword) {
        if (error) *error = T("Пароль слишком короткий (минимум %1).").arg(kMinPassword);
        return QString();
    }
    UserRec r;
    r.id = makeUserId();
    r.name = n;
    r.role = role == QLatin1String("admin") ? QStringLiteral("admin") : QStringLiteral("operator");
    r.enabled = true;
    applyUserPassword(r, password);
    v.append(r);
    saveUserRecs(v);
    SecurityAudit::write(QStringLiteral("user_created"), r.id + QStringLiteral(" role=") + r.role);
    return r.id;
}

bool Users::remove(const QString &id, QString *error)
{
    if (!Users::current().isAdmin()) { if (error) *error = T("Недостаточно прав: требуется администратор."); SecurityAudit::write(QStringLiteral("permission_denied"), QStringLiteral("users.remove")); return false; }
    QList<UserRec> v = loadUserRecs();
    if (v.size() <= 1) {
        if (error) *error = T("Нельзя удалить единственного пользователя.");
        return false;
    }
    if (id == g_currentUserId) {
        if (error) *error = T("Нельзя удалить пользователя, под которым выполнен вход.");
        return false;
    }
    int idx = -1, admins = 0;
    for (int i = 0; i < v.size(); ++i) {
        if (v.at(i).role == QLatin1String("admin"))
            ++admins;
        if (v.at(i).id == id)
            idx = i;
    }
    if (idx < 0)
        return false;
    if (v.at(idx).role == QLatin1String("developer") || v.at(idx).role == QLatin1String("guest")) {
        if (error) *error = v.at(idx).role == QLatin1String("guest")
            ? T("Системную учётную запись «Гость» нельзя удалить.")
            : T("Учётную запись «Разработчик» нельзя удалить.");
        return false;
    }
    if (v.at(idx).role == QLatin1String("admin") && admins <= 1) {
        if (error) *error = T("Нельзя удалить последнего администратора.");
        return false;
    }
    v.removeAt(idx);
    saveUserRecs(v);
    SecurityAudit::write(QStringLiteral("user_removed"), id);
    return true;
}

bool Users::setPassword(const QString &id, const QString &password, QString *error)
{
    if (!Users::current().isAdmin() && id != Users::current().id) { if (error) *error = T("Недостаточно прав."); SecurityAudit::write(QStringLiteral("permission_denied"), QStringLiteral("users.setPassword")); return false; }
    if (Users::find(id).isDeveloper() && !Users::current().isDeveloper()) {
        if (error) *error = T("Изменять пароль учётной записи «Разработчик» может только сам разработчик.");
        return false;
    }
    QList<UserRec> v = loadUserRecs();
    for (UserRec &r : v) {
        if (r.id == id) {
            if (r.role == QLatin1String("developer") && password.isEmpty()) {
                if (error) *error = T("Для учётной записи «Разработчик» пароль обязателен.");
                return false;
            }
            if (!password.isEmpty() && password.size() < kMinPassword) {
                if (error) *error = T("Пароль слишком короткий (минимум %1).").arg(kMinPassword);
                return false;
            }
            applyUserPassword(r, password);
            saveUserRecs(v);
            SecurityAudit::write(QStringLiteral("user_password_changed"), id);
            return true;
        }
    }
    return false;
}

bool Users::setRole(const QString &id, const QString &role, QString *error)
{
    if (!Users::current().isAdmin()) { if (error) *error = T("Недостаточно прав: требуется администратор."); SecurityAudit::write(QStringLiteral("permission_denied"), QStringLiteral("users.setRole")); return false; }
    const QString newRole = role == QLatin1String("admin") ? QStringLiteral("admin") : QStringLiteral("operator");
    const UserInfo targetUser = Users::find(id);
    if (targetUser.isDeveloper() || targetUser.isGuest()) {
        if (error) *error = targetUser.isGuest()
            ? T("Роль системной учётной записи «Гость» нельзя изменить.")
            : T("Роль учётной записи «Разработчик» нельзя изменить.");
        return false;
    }
    QList<UserRec> v = loadUserRecs();
    int idx = -1, admins = 0;
    for (int i = 0; i < v.size(); ++i) {
        if (v.at(i).role == QLatin1String("admin"))
            ++admins;
        if (v.at(i).id == id)
            idx = i;
    }
    if (idx < 0)
        return false;
    if (v.at(idx).role == QLatin1String("admin") && newRole != QLatin1String("admin") && admins <= 1) {
        if (error) *error = T("Нельзя понизить последнего администратора.");
        return false;
    }
    v[idx].role = newRole;
    saveUserRecs(v);
    SecurityAudit::write(QStringLiteral("user_role_changed"), id + QStringLiteral(" role=") + newRole);
    return true;
}

bool Users::setEnabled(const QString &id, bool enabled, QString *error)
{
    if (!Users::current().isAdmin()) {
        if (error) *error = T("Недостаточно прав: требуется администратор.");
        SecurityAudit::write(QStringLiteral("permission_denied"), QStringLiteral("users.setEnabled"));
        return false;
    }
    if (id == Users::current().id) {
        if (error) *error = T("Нельзя отключить учётную запись, под которой выполнен вход.");
        return false;
    }
    QList<UserRec> v = loadUserRecs();
    int idx = -1;
    int enabledAdmins = 0;
    for (int i = 0; i < v.size(); ++i) {
        if (v.at(i).role == QLatin1String("admin") && v.at(i).enabled)
            ++enabledAdmins;
        if (v.at(i).id == id)
            idx = i;
    }
    if (idx < 0) {
        if (error) *error = T("Пользователь не найден.");
        return false;
    }
    if (v.at(idx).role == QLatin1String("guest") || v.at(idx).role == QLatin1String("developer")) {
        if (error) *error = T("Системные учётные записи «Гость» и «Разработчик» нельзя отключить.");
        return false;
    }
    if (!enabled && v.at(idx).role == QLatin1String("admin") && enabledAdmins <= 1) {
        if (error) *error = T("Нельзя отключить последнего активного администратора.");
        return false;
    }
    v[idx].enabled = enabled;
    v[idx].remember.clear();
    saveUserRecs(v);
    SecurityAudit::write(enabled ? QStringLiteral("user_enabled") : QStringLiteral("user_disabled"), id);
    return true;
}

bool Users::verify(const QString &id, const QString &password)
{
    for (const UserRec &r : loadUserRecs()) {
        if (r.id != id)
            continue;
        if (!r.enabled)
            return false;
        if (r.hash.isEmpty())
            return true;                              // пароль не задан
        const QByteArray salt = QByteArray::fromBase64(r.salt.toLatin1());
        const QByteArray stored = QByteArray::fromBase64(r.hash.toLatin1());
        return userBytesEqual(deriveUserKey(password, salt, r.iterations), stored);
    }
    return false;
}

QString Users::issueRememberToken(const QString &id)
{
    QList<UserRec> v = loadUserRecs();
    for (UserRec &r : v) {
        if (r.id == id) {
            const QString token = QString::fromLatin1(randomUserBytes(32).toBase64());
            r.remember = tokenHash(token);
            saveUserRecs(v);
            QSettings s;
            s.setValue(QStringLiteral("session/userId"), id);
            s.setValue(QStringLiteral("session/token"), token);
            return token;
        }
    }
    return QString();
}

bool Users::consumeRememberedLogin(QString *userId)
{
    QSettings s;
    const QString id = s.value(QStringLiteral("session/userId")).toString();
    const QString token = s.value(QStringLiteral("session/token")).toString();
    if (id.isEmpty() || token.isEmpty())
        return false;
    for (const UserRec &r : loadUserRecs()) {
        if (r.id == id && !r.remember.isEmpty()
            && userBytesEqual(tokenHash(token).toLatin1(), r.remember.toLatin1())) {
            if (userId)
                *userId = id;
            return true;
        }
    }
    return false;
}

void Users::forgetRemembered()
{
    QSettings s;
    const QString id = s.value(QStringLiteral("session/userId")).toString();
    s.remove(QStringLiteral("session/userId"));
    s.remove(QStringLiteral("session/token"));
    if (id.isEmpty())
        return;
    QList<UserRec> v = loadUserRecs();
    for (UserRec &r : v)
        if (r.id == id)
            r.remember.clear();
    saveUserRecs(v);
}

void Users::setCurrent(const QString &id)
{
    g_currentUserId = id;
}

UserInfo Users::current()
{
    return find(g_currentUserId);
}

QString Users::lastUserId()
{
    return QSettings().value(QStringLiteral("session/lastUser")).toString();
}

void Users::setLastUserId(const QString &id)
{
    QSettings().setValue(QStringLiteral("session/lastUser"), id);
}

// ======================================================================
// app/win_integration.h
// ======================================================================
#include <QWidget>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

// Windows 10/11: тёмная рамка заголовка окна под цвет программы
inline void applyDarkTitleBar(QWidget *w)
{
#ifdef Q_OS_WIN
    if (!w || !w->isWindow())
        return;
    const HWND hwnd = reinterpret_cast<HWND>(w->winId());
    const BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &on, sizeof(on));   // DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 2004+ и 11)
    DwmSetWindowAttribute(hwnd, 19, &on, sizeof(on));   // то же для ранних сборок Windows 10
#else
    Q_UNUSED(w)
#endif
}

// ======================================================================
