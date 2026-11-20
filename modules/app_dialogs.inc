// PSVms module: Диалоги, ввод данных и менеджер камер
// This file is included by psvms.cpp to preserve the original single-TU build
// while the code is being separated safely.

// app/analog_camera_dialog.cpp
// ======================================================================
AnalogCameraDialog::AnalogCameraDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(T("Аналоговая камера (карта видеозахвата)"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(520);

    auto *hint = new QLabel(
        T("Для камеры, подключённой через карту видеозахвата (USB или PCI). Устройство должно быть "
          "подключено и видно Windows. Запись в архив для таких камер не ведётся — доступен только "
          "живой просмотр."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#8a8a8e;"));

    m_name = new QLineEdit(this);
    m_group = new QLineEdit(this);
    m_group->setPlaceholderText(T("необязательно"));
    m_device = new QComboBox(this);
    for (const QCameraDevice &d : QMediaDevices::videoInputs())
        m_device->addItem(d.description(), QString::fromLatin1(d.id().toBase64()));
    if (m_device->count() == 0)
        m_device->addItem(T("Устройства захвата не найдены"), QString());

    auto *form = new QFormLayout;
    form->addRow(T("Устройство"), m_device);
    form->addRow(T("Название"), m_name);
    form->addRow(T("Группа"), m_group);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    localizeButtons(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(hint);
    lay->addLayout(form);
    lay->addWidget(buttons);
}

CameraEntry AnalogCameraDialog::entry() const
{
    CameraEntry e;
    e.name = m_name->text().trimmed();
    e.group = m_group->text().trimmed();
    e.url = QStringLiteral("qcam:") + m_device->currentData().toString();
    e.record = false;   // для локальных устройств захвата запись не поддерживается
    return e;
}

void AnalogCameraDialog::accept()
{
    if (m_device->currentData().toString().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), T("Устройство захвата не выбрано или не найдено."));
        return;
    }
    if (m_name->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), T("Укажите название камеры."));
        return;
    }
    QDialog::accept();
}

// ======================================================================
// app/camera_dialog.cpp
// ======================================================================
CameraDialog::CameraDialog(const CameraEntry &base, const QStringList &groups, QWidget *parent)
    : QDialog(parent), m_base(base)
{
    setWindowTitle(base.id.isEmpty() ? T("Добавить камеру") : T("Настройки камеры"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(640);

    m_name = new QLineEdit(base.name, this);
    m_group = new QComboBox(this);
    m_group->setEditable(true);
    m_group->addItems(groups);
    m_group->setCurrentText(base.group);

    m_url = new QLineEdit(base.url, this);
    m_url->setPlaceholderText(T("rtsp://логин:пароль@192.168.1.64:554/Streaming/Channels/101"));
    m_liveUrl = new QLineEdit(base.liveUrl, this);
    m_webUrl = new QLineEdit(base.webUrl, this);
    m_webUrl->setPlaceholderText(T("http://192.168.1.64 или https://192.168.1.64"));
    m_bitrate = new QSpinBox(this);
    m_bitrate->setRange(64, 200000);
    m_bitrate->setSuffix(T(" кбит/с"));
    m_bitrate->setValue(qBound(64, base.bitrateKbps, 200000));
    m_codec = new QComboBox(this);
    m_codec->addItems({QStringLiteral("H.264"), QStringLiteral("H.265 / HEVC"), QStringLiteral("MJPEG"), T("Другой / неизвестен")});
    m_codec->setCurrentText(base.codec.isEmpty() ? QStringLiteral("H.264") : base.codec);
    m_liveUrl->setPlaceholderText(T("необязательно: поток меньшего разрешения для мозаики"));
    m_urlPick = new QComboBox(this);
    m_livePick = new QComboBox(this);
    m_urlPick->setMinimumWidth(200);
    m_livePick->setMinimumWidth(200);

    m_quality = new QComboBox(this);
    m_quality->addItem(T("Авто: подпоток в мозаике, основной в раскладке 1 × 1"), 0);
    m_quality->addItem(T("Высокое: всегда основной поток"), 1);
    m_quality->addItem(T("Низкое: всегда подпоток (экономия ресурсов)"), 2);
    m_quality->setCurrentIndex(qBound(0, base.viewQuality, 2));

    m_brightness = new QSlider(Qt::Horizontal, this);
    m_brightness->setRange(-50, 50);
    m_brightness->setValue(base.brightness);
    m_brightLabel = new QLabel(QString::number(base.brightness), this);
    m_brightLabel->setMinimumWidth(36);
    connect(m_brightness, &QSlider::valueChanged, this, [this](int v) { m_brightLabel->setText(QString::number(v)); });
    auto *brightRow = new QWidget(this);
    auto *brightLay = new QHBoxLayout(brightRow);
    brightLay->setContentsMargins(0, 0, 0, 0);
    brightLay->addWidget(m_brightness, 1);
    brightLay->addWidget(m_brightLabel);

    m_record = new QCheckBox(T("Вести непрерывную запись в архив"), this);
    m_record->setChecked(base.record);
    m_cameraScheduleEnabled = new QCheckBox(T("Использовать индивидуальное расписание этой камеры"), this);
    m_cameraScheduleEnabled->setChecked(base.scheduleEnabled);
    m_cameraScheduleFrom = new QTimeEdit(base.scheduleFrom, this);
    m_cameraScheduleTo = new QTimeEdit(base.scheduleTo, this);
    m_cameraScheduleFrom->setDisplayFormat(QStringLiteral("HH:mm"));
    m_cameraScheduleTo->setDisplayFormat(QStringLiteral("HH:mm"));
    auto *cameraScheduleTimes = new QWidget(this);
    auto *cameraScheduleTimesLayout = new QHBoxLayout(cameraScheduleTimes);
    cameraScheduleTimesLayout->setContentsMargins(0, 0, 0, 0);
    cameraScheduleTimesLayout->addWidget(new QLabel(T("С"), cameraScheduleTimes));
    cameraScheduleTimesLayout->addWidget(m_cameraScheduleFrom);
    cameraScheduleTimesLayout->addWidget(new QLabel(T("По"), cameraScheduleTimes));
    cameraScheduleTimesLayout->addWidget(m_cameraScheduleTo);
    cameraScheduleTimesLayout->addStretch(1);
    auto *cameraScheduleDays = new QWidget(this);
    auto *cameraScheduleDaysLayout = new QHBoxLayout(cameraScheduleDays);
    cameraScheduleDaysLayout->setContentsMargins(0, 0, 0, 0);
    const QStringList cameraDayNames = {T("Пн"), T("Вт"), T("Ср"), T("Чт"), T("Пт"), T("Сб"), T("Вс")};
    for (int day = 0; day < 7; ++day) {
        auto *check = new QCheckBox(cameraDayNames.at(day), cameraScheduleDays);
        check->setChecked((base.scheduleDays & (1u << day)) != 0);
        m_cameraScheduleDays.append(check);
        cameraScheduleDaysLayout->addWidget(check);
    }
    cameraScheduleDaysLayout->addStretch(1);
    auto updateCameraScheduleControls = [this, cameraScheduleTimes, cameraScheduleDays](bool enabled) {
        cameraScheduleTimes->setEnabled(enabled);
        cameraScheduleDays->setEnabled(enabled);
    };
    updateCameraScheduleControls(base.scheduleEnabled);
    connect(m_cameraScheduleEnabled, &QCheckBox::toggled, this, updateCameraScheduleControls);
    m_showTime = new QCheckBox(T("Показывать дату и время на картинке"), this);
    m_showTime->setChecked(base.showTime);
    m_audio = new QCheckBox(T("Включать звук при показе"), this);
    m_audio->setChecked(base.audio);

    auto *form = new QFormLayout;
    form->addRow(T("Название"), m_name);
    form->addRow(T("Группа"), m_group);
    form->addRow(T("Поток записи"), withPicker(m_url, m_urlPick));
    form->addRow(T("Подпоток для мозаики"), withPicker(m_liveUrl, m_livePick));
    form->addRow(T("Веб-интерфейс устройства"), m_webUrl);
    form->addRow(T("Битрейт основного потока"), m_bitrate);
    form->addRow(T("Видеокодек"), m_codec);
    auto *detectButton = new QPushButton(T("Получить кодек и битрейт автоматически (ISAPI)…"), this);
    detectButton->setToolTip(T("Попытка чтения параметров HiWatch/Hikvision. Для авторизации используются логин и пароль из RTSP-адреса; если устройство не поддерживает ISAPI, параметры можно задать вручную."));
    form->addRow(QString(), detectButton);
    connect(detectButton, &QPushButton::clicked, this, [this, detectButton] {
        QUrl streamUrl(m_url->text().trimmed());
        QString host = streamUrl.host();
        if (host.isEmpty()) {
            const QUrl webUrl(m_webUrl->text().trimmed());
            host = webUrl.host();
        }
        if (host.isEmpty()) {
            QMessageBox::warning(this, windowTitle(), T("Сначала укажите RTSP-адрес или веб-адрес устройства."));
            return;
        }
        QUrl apiUrl;
        const QUrl webUrl(m_webUrl->text().trimmed());
        if (webUrl.isValid() && !webUrl.host().isEmpty()) {
            apiUrl = webUrl;
            apiUrl.setPath(QStringLiteral("/ISAPI/Streaming/channels/101"));
            apiUrl.setQuery(QString());
        } else {
            apiUrl.setScheme(QStringLiteral("http"));
            apiUrl.setHost(host);
            apiUrl.setPort(80);
            apiUrl.setPath(QStringLiteral("/ISAPI/Streaming/channels/101"));
        }
        detectButton->setEnabled(false);
        detectButton->setText(T("Получение параметров…"));
        QNetworkAccessManager manager;
        QNetworkRequest request(apiUrl);
        request.setTransferTimeout(5500);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("PSVms/1.0"));
        QNetworkReply *reply = manager.get(request);
        const QString user = streamUrl.userName(QUrl::FullyDecoded);
        const QString pass = streamUrl.password(QUrl::FullyDecoded);
        if (!user.isEmpty()) {
            QObject::connect(&manager, &QNetworkAccessManager::authenticationRequired, this,
                [user, pass](QNetworkReply *, QAuthenticator *auth) {
                    auth->setUser(user);
                    auth->setPassword(pass);
                });
        }
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(6500);
        loop.exec();
        const bool timedOut = timeout.isActive() == false && !reply->isFinished();
        if (timedOut) reply->abort();
        const auto networkError = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();
        reply->deleteLater();
        detectButton->setEnabled(true);
        detectButton->setText(T("Получить кодек и битрейт автоматически (ISAPI)…"));
        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
            QMessageBox::information(this, T("Автоопределение"), T("Не удалось получить параметры через ISAPI (HTTP %1). Проверьте доступность HTTP/HTTPS, логин/пароль в RTSP-адресе и поддержку ISAPI. Можно продолжить с ручными параметрами.").arg(status ? QString::number(status) : T("нет ответа")));
            return;
        }
        QString codec;
        int bitrate = 0;
        QXmlStreamReader xml(payload);
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            const QString key = xml.name().toString().toLower();
            const QString value = xml.readElementText().trimmed();
            if (key == QStringLiteral("videocodectype") || key == QStringLiteral("codec")) codec = value;
            else if (key == QStringLiteral("constantbitrate") || key == QStringLiteral("vbruppercap") || key == QStringLiteral("maxbitrate")) {
                bool ok = false;
                const int parsed = value.toInt(&ok);
                if (ok && parsed > 0) bitrate = qMax(bitrate, parsed);
            }
        }
        if (codec.contains(QStringLiteral("265"))) m_codec->setCurrentText(QStringLiteral("H.265 / HEVC"));
        else if (codec.contains(QStringLiteral("264"))) m_codec->setCurrentText(QStringLiteral("H.264"));
        else if (codec.contains(QStringLiteral("mjpeg"), Qt::CaseInsensitive)) m_codec->setCurrentText(QStringLiteral("MJPEG"));
        if (bitrate > 0) m_bitrate->setValue(qBound(64, bitrate, 200000));
        if (codec.isEmpty() && bitrate <= 0) {
            QMessageBox::information(this, T("Автоопределение"), T("Устройство ответило, но не вернуло распознаваемые поля кодека/битрейта. Оставьте или укажите значения вручную."));
        } else {
            QMessageBox::information(this, T("Автоопределение"), T("Полученные параметры применены. Проверьте их перед сохранением: некоторые прошивки возвращают верхний предел VBR, а не фактический средний битрейт."));
        }
    });
    form->addRow(T("Качество картинки"), m_quality);
    form->addRow(T("Яркость"), brightRow);
    form->addRow(QString(), m_record);
    form->addRow(QString(), m_cameraScheduleEnabled);
    form->addRow(T("Дни индивидуального расписания"), cameraScheduleDays);
    form->addRow(T("Время индивидуальной записи"), cameraScheduleTimes);
    form->addRow(QString(), m_showTime);
    form->addRow(QString(), m_audio);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    localizeButtons(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // выбор потока из шаблонов производителя: в поле подставляется готовый адрес
    connect(m_url, &QLineEdit::textChanged, this, [this] { rebuildPickers(); });
    connect(m_urlPick, &QComboBox::activated, this, [this](int i) {
        const QString u = m_urlPick->itemData(i).toString();
        m_urlPick->setCurrentIndex(0);
        if (!u.isEmpty())
            m_url->setText(u);
    });
    connect(m_livePick, &QComboBox::activated, this, [this](int i) {
        const QString u = m_livePick->itemData(i).toString();
        m_livePick->setCurrentIndex(0);
        if (u == QLatin1String("-"))
            m_liveUrl->clear();
        else if (!u.isEmpty())
            m_liveUrl->setText(u);
    });
    rebuildPickers();

    auto *lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(buttons);
}

QWidget *CameraDialog::withPicker(QLineEdit *edit, QComboBox *combo)
{
    auto *w = new QWidget(this);
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(edit, 1);
    l->addWidget(combo);
    return w;
}

// Варианты потоков строятся по адресу записи (Hikvision/HiWatch, Dahua, iCSee)
void CameraDialog::rebuildPickers()
{
    const QList<StreamOption> opts = streamOptionsFor(m_url->text().trimmed());

    m_urlPick->clear();
    m_urlPick->addItem(T("Выбрать поток…"), QString());
    for (const StreamOption &o : opts)
        m_urlPick->addItem(o.label, o.url);
    m_urlPick->setEnabled(!opts.isEmpty());

    m_livePick->clear();
    m_livePick->addItem(T("Выбрать поток…"), QString());
    m_livePick->addItem(T("Как у записи (без подпотока)"), QStringLiteral("-"));
    for (const StreamOption &o : opts)
        m_livePick->addItem(o.label, o.url);

    const QString tip = opts.isEmpty() ? T("Для этого адреса шаблон потоков неизвестен: введите адрес вручную") : QString();
    m_urlPick->setToolTip(tip);
    m_livePick->setToolTip(tip);
}

CameraEntry CameraDialog::entry() const
{
    CameraEntry e = m_base;
    e.name = m_name->text().trimmed();
    e.group = m_group->currentText().trimmed();
    e.url = m_url->text().trimmed();
    e.liveUrl = m_liveUrl->text().trimmed();
    e.webUrl = m_webUrl->text().trimmed();
    e.bitrateKbps = m_bitrate->value();
    e.codec = m_codec->currentText();
    e.viewQuality = m_quality->currentData().toInt();
    e.brightness = m_brightness->value();
    e.record = m_record->isChecked();
    e.scheduleEnabled = m_cameraScheduleEnabled->isChecked();
    e.scheduleFrom = m_cameraScheduleFrom->time();
    e.scheduleTo = m_cameraScheduleTo->time();
    e.scheduleDays = 0;
    for (int day = 0; day < m_cameraScheduleDays.size(); ++day)
        if (m_cameraScheduleDays.at(day)->isChecked())
            e.scheduleDays |= quint8(1u << day);
    e.showTime = m_showTime->isChecked();
    e.audio = m_audio->isChecked();
    return e;
}

void CameraDialog::accept()
{
    const QUrl u(m_url->text().trimmed());
    const QUrl web(m_webUrl->text().trimmed());
    if (!m_webUrl->text().trimmed().isEmpty() && (!web.isValid() || (web.scheme() != QLatin1String("http") && web.scheme() != QLatin1String("https")))) {
        QMessageBox::warning(this, windowTitle(), T("Веб-адрес должен начинаться с http:// или https://"));
        return;
    }
    if (m_name->text().trimmed().isEmpty() || !u.isValid() || u.scheme().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), T("Укажите название и корректный адрес потока, например rtsp://…"));
        return;
    }
    QDialog::accept();
}

// ======================================================================
// app/camera_manager.cpp
// ======================================================================
namespace {

QString newCameraId()
{
    return QStringLiteral("cam") + QUuid::createUuid().toString(QUuid::Id128).left(8);
}

int probeIntervalMs()
{
    return 20000;
}

} // namespace

CameraManager::CameraManager(QObject *parent) : QObject(parent)
{
    m_storageRoot = Cfg::videoDir();
    QDir().mkpath(m_storageRoot);

    m_startTimer = new QTimer(this);
    m_startTimer->setInterval(120);
    connect(m_startTimer, &QTimer::timeout, this, &CameraManager::startNext);

    m_scheduleTimer = new QTimer(this);
    m_scheduleTimer->setInterval(15000);
    connect(m_scheduleTimer, &QTimer::timeout, this, &CameraManager::applyRecordingSchedule);
    m_scheduleTimer->start();

    m_probeTimer = new QTimer(this);
    m_probeTimer->setInterval(probeIntervalMs());
    connect(m_probeTimer, &QTimer::timeout, this, &CameraManager::probeNow);
    connect(Cfg::notifier(), &CfgNotifier::changed, this, [this] { m_probeTimer->setInterval(probeIntervalMs()); });

    load();
    applyRecordingSchedule();
    if (Cfg::autostartCameras()) {
        for (const CameraEntry &e : std::as_const(m_cameras))
            syncWorker(e);
    }
    m_probeTimer->start();
    QTimer::singleShot(800, this, &CameraManager::probeNow);
}

CameraManager::~CameraManager()
{
    shutdown();
}

const CameraEntry *CameraManager::find(const QString &id) const
{
    for (const CameraEntry &e : m_cameras)
        if (e.id == id)
            return &e;
    return nullptr;
}

CameraWorker::State CameraManager::stateOf(const QString &id) const
{
    return m_states.value(id, CameraWorker::State::Stopped);
}

bool CameraManager::isStarted(const QString &id) const
{
    return m_workers.contains(id) || m_startQueue.contains(id);
}

bool CameraManager::isOnline(const QString &id) const
{
    return m_online.value(id, false) || stateOf(id) == CameraWorker::State::Recording;
}

int CameraManager::onlineCount() const
{
    int n = 0;
    for (const CameraEntry &e : m_cameras)
        if (isOnline(e.id))
            ++n;
    return n;
}

int CameraManager::offlineCount() const
{
    return int(m_cameras.size()) - onlineCount();
}

int CameraManager::recordingCount() const
{
    int n = 0;
    for (const CameraEntry &e : m_cameras)
        if (stateOf(e.id) == CameraWorker::State::Recording)
            ++n;
    return n;
}

namespace {
static const QByteArray kPsvmsLicensePublicKey = QByteArray::fromHex(
    "0bfe4cb9c32f723e982c597f6df3aa72b982d9aa18fcf865850163580ed6e49e");

QString psvmsMachineFingerprint()
{
    QByteArray machine = QSysInfo::machineUniqueId();
    if (machine.isEmpty())
        machine = (QSysInfo::machineHostName() + QSysInfo::productType()
                   + QSysInfo::currentCpuArchitecture()).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(machine, QCryptographicHash::Sha256)
                               .toHex().left(32)).toUpper();
}

bool psvmsVerifyLicense(const QByteArray &document, QJsonObject *payloadOut, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(document, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("Файл лицензии имеет неверный формат JSON."));
    const QJsonObject root = doc.object();
    const QJsonObject payload = root.value(QStringLiteral("payload")).toObject();
    const QByteArray signature = QByteArray::fromBase64(
        root.value(QStringLiteral("signature")).toString().toLatin1());
    if (payload.isEmpty() || signature.size() != 64)
        return fail(QStringLiteral("В файле отсутствуют данные или цифровая подпись."));
    const QByteArray signedBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                 reinterpret_cast<const unsigned char *>(kPsvmsLicensePublicKey.constData()),
                                                 size_t(kPsvmsLicensePublicKey.size()));
    if (!key)
        return fail(QStringLiteral("Не удалось инициализировать проверку подписи."));
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const bool initialized = ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1;
    const bool verified = initialized &&
        EVP_DigestVerify(ctx,
            reinterpret_cast<const unsigned char *>(signature.constData()), size_t(signature.size()),
            reinterpret_cast<const unsigned char *>(signedBytes.constData()), size_t(signedBytes.size())) == 1;
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (!verified)
        return fail(QStringLiteral("Подпись лицензии недействительна. Файл изменён или выпущен не вашим генератором."));
    if (payload.value(QStringLiteral("schema")).toInt() != 1)
        return fail(QStringLiteral("Неподдерживаемая версия лицензии."));
    if (payload.value(QStringLiteral("machineId")).toString() != psvmsMachineFingerprint())
        return fail(QStringLiteral("Эта лицензия выпущена для другого компьютера."));
    const QString mode = payload.value(QStringLiteral("mode")).toString();
    if (mode == QStringLiteral("duration")) {
        if (payload.value(QStringLiteral("durationSeconds")).toVariant().toLongLong() <= 0)
            return fail(QStringLiteral("В лицензии указан неверный срок действия."));
    } else if (mode == QStringLiteral("fixed")) {
        const QDateTime expiry = QDateTime::fromString(
            payload.value(QStringLiteral("expiresAt")).toString(), Qt::ISODateWithMs);
        if (!expiry.isValid())
            return fail(QStringLiteral("В лицензии не указана корректная дата окончания."));
    } else {
        return fail(QStringLiteral("Неизвестный тип срока действия лицензии."));
    }
    if (payloadOut) *payloadOut = payload;
    if (error) error->clear();
    return true;
}

bool psvmsLicenseActive(QString *reason = nullptr, QDateTime *expiryOut = nullptr)
{
    QSettings settings;
    const QByteArray document = settings.value(QStringLiteral("license/document")).toByteArray();
    if (document.isEmpty()) {
        if (reason) *reason = QStringLiteral("Лицензия не активирована. Доступен демонстрационный режим.");
        return false;
    }
    QJsonObject payload;
    QString error;
    if (!psvmsVerifyLicense(document, &payload, &error)) {
        if (reason) *reason = error;
        return false;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString mode = payload.value(QStringLiteral("mode")).toString();
    QDateTime expiry;
    if (mode == QStringLiteral("duration")) {
        QDateTime activation = settings.value(QStringLiteral("license/firstActivationUtc")).toDateTime();
        if (!activation.isValid()) {
            activation = now;
            settings.setValue(QStringLiteral("license/firstActivationUtc"), activation);
            settings.setValue(QStringLiteral("license/lastSeenUtc"), activation);
        }
        const qint64 seconds = payload.value(QStringLiteral("durationSeconds")).toVariant().toLongLong();
        expiry = activation.addSecs(seconds);
    } else {
        expiry = QDateTime::fromString(payload.value(QStringLiteral("expiresAt")).toString(), Qt::ISODateWithMs);
        expiry = expiry.toUTC();
    }

    const QDateTime lastSeen = settings.value(QStringLiteral("license/lastSeenUtc")).toDateTime().toUTC();
    if (lastSeen.isValid() && now < lastSeen) {
        if (reason) *reason = QStringLiteral("Обнаружен перевод системных часов назад. Проверьте дату и время Windows.");
        return false;
    }
    if (now > expiry) {
        if (reason) *reason = QStringLiteral("Срок действия лицензии истёк: %1 UTC.")
            .arg(expiry.toString(QStringLiteral("dd.MM.yyyy HH:mm")));
        return false;
    }

    // Запоминаем последнее проверенное время, чтобы выявлять перевод часов назад
    // после перезапуска приложения. Запись ограничена одним разом в минуту.
    static QDateTime lastPersist;
    if (!lastSeen.isValid() || !lastPersist.isValid() || lastPersist.secsTo(now) >= 60) {
        settings.setValue(QStringLiteral("license/lastSeenUtc"), now);
        lastPersist = now;
    }
    if (expiryOut) *expiryOut = expiry;
    if (reason) *reason = QStringLiteral("Лицензия активна до %1 UTC.")
        .arg(expiry.toString(QStringLiteral("dd.MM.yyyy HH:mm")));
    return true;
}

bool psvmsImportLicense(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error) *error = QStringLiteral("Не удалось открыть файл лицензии.");
        return false;
    }
    const QByteArray document = file.readAll();
    QJsonObject payload;
    if (!psvmsVerifyLicense(document, &payload, error))
        return false;
    QSettings settings;
    settings.setValue(QStringLiteral("license/document"), document);
    settings.remove(QStringLiteral("license/firstActivationUtc"));
    settings.remove(QStringLiteral("license/lastSeenUtc"));
    // Для duration первая активация начинается сразу после успешного импорта.
    if (payload.value(QStringLiteral("mode")).toString() == QStringLiteral("duration")) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        settings.setValue(QStringLiteral("license/firstActivationUtc"), now);
        settings.setValue(QStringLiteral("license/lastSeenUtc"), now);
    }
    if (error) error->clear();
    return true;
}

inline bool isProVersion()
{
    // Учётная запись разработчика получает полный функционал для разработки и тестирования.
    // Это не создаёт и не подменяет коммерческую лицензию: проверка лицензий для остальных ролей
    // остаётся прежней.
    return Users::current().isDeveloper() || psvmsLicenseActive();
}
}

QString CameraManager::add(CameraEntry e)
{
    if (!Users::current().isAdmin()) { SecurityAudit::write(QStringLiteral("permission_denied"), QStringLiteral("camera.add")); return QString(); }
    // Не допускаем повторное добавление одного и того же устройства.
    const QString candidateUrl = QUrl(e.url).toString(QUrl::FullyEncoded).trimmed().toLower();
    for (const CameraEntry &existing : m_cameras) {
        const QString existingUrl = QUrl(existing.url).toString(QUrl::FullyEncoded).trimmed().toLower();
        if (!candidateUrl.isEmpty() && candidateUrl == existingUrl) {
            QMessageBox::information(nullptr, T("Устройство уже добавлено"),
                                     T("Это устройство уже есть в списке камер или регистраторов."));
            return QString();
        }
    }
    if (!isProVersion() && m_cameras.size() >= 10) {
        QMessageBox::information(nullptr, T("Демо-версия"),
            T("В демо-версии можно подключить не более 10 камер. Для снятия ограничения активируйте Pro-версию в настройках."));
        return QString();
    }
    e.id = newCameraId();
    m_cameras.append(e);
    save();
    SecurityAudit::write(QStringLiteral("camera_added"), e.id);
    syncWorker(e);
    emit camerasChanged();
    probeNow();
    return e.id;
}

void CameraManager::addMany(QList<CameraEntry> list)
{
    if (!Users::current().isAdmin()) { SecurityAudit::write(QStringLiteral("permission_denied"), QStringLiteral("camera.addMany")); return; }
    if (list.isEmpty())
        return;

    // Убираем дубликаты как относительно уже сохранённых устройств, так и внутри импортируемой пачки.
    QSet<QString> knownUrls;
    for (const CameraEntry &existing : m_cameras)
        knownUrls.insert(QUrl(existing.url).toString(QUrl::FullyEncoded).trimmed().toLower());
    QList<CameraEntry> uniqueList;
    int duplicates = 0;
    for (const CameraEntry &entry : list) {
        const QString key = QUrl(entry.url).toString(QUrl::FullyEncoded).trimmed().toLower();
        if (key.isEmpty() || knownUrls.contains(key)) {
            ++duplicates;
            continue;
        }
        knownUrls.insert(key);
        uniqueList.append(entry);
    }
    list = uniqueList;
    if (duplicates > 0)
        QMessageBox::information(nullptr, T("Устройство уже добавлено"),
                                 T("Пропущено повторных устройств: %1. Уже добавленные камеры и регистраторы не дублируются.").arg(duplicates));
    if (list.isEmpty())
        return;

    if (!isProVersion()) {
        const int available = qMax(0, 10 - m_cameras.size());
        if (available == 0) {
            QMessageBox::information(nullptr, T("Демо-версия"),
                T("Лимит демо-версии — 10 камер. Активируйте Pro-версию в настройках, чтобы добавить больше."));
            return;
        }
        if (list.size() > available) {
            list = list.mid(0, available);
            QMessageBox::information(nullptr, T("Демо-версия"),
                T("В демо-версии можно добавить ещё %1 камер. Остальные устройства не добавлены. Для полного доступа активируйте Pro-версию в настройках.").arg(available));
        }
    }
    for (CameraEntry &e : list) {
        e.id = newCameraId();
    }
    m_cameras.append(list);
    save();                                  // один раз на всю пачку
    SecurityAudit::write(QStringLiteral("camera_batch_added"), QStringLiteral("count=%1").arg(list.size()));
    for (const CameraEntry &e : std::as_const(list))
        syncWorker(e);
    emit camerasChanged();
    probeNow();
}

void CameraManager::enforceDemoLimit()
{
    if (isProVersion() || m_cameras.size() <= 10)
        return;

    for (int i = 10; i < m_cameras.size(); ++i) {
        const QString id = m_cameras.at(i).id;
        stopWorker(id);
        m_states.remove(id);
        m_online.remove(id);
    }
    m_cameras = m_cameras.mid(0, 10);
    save();
    emit camerasChanged();
    emit onlineChanged();
    QMessageBox::information(nullptr, T("Демо-версия"),
        T("В демо-режиме одновременно доступны не более 10 камер. Лишние камеры удалены из списка подключений."));
}

void CameraManager::update(const CameraEntry &e)
{
    if (!Users::current().isAdmin()) { SecurityAudit::write(QStringLiteral("permission_denied"), QStringLiteral("camera.update")); return; }
    for (CameraEntry &old : m_cameras) {
        if (old.id == e.id) {
            // рекордер перезапускаем, только если изменился адрес записи или её включение
            const bool restart = old.url != e.url || old.record != e.record;
            const bool wasStarted = isStarted(e.id);
            old = e;
            save();
            SecurityAudit::write(QStringLiteral("camera_updated"), e.id);
            if (restart && wasStarted)
                syncWorker(e);
            applyRecordingSchedule();
            emit camerasChanged();
            return;
        }
    }
}

void CameraManager::setLiveUrl(const QString &id, const QString &url)
{
    for (CameraEntry &e : m_cameras) {
        if (e.id == id) {
            if (e.liveUrl == url)
                return;
            e.liveUrl = url;
            save();
            emit camerasChanged();
            return;
        }
    }
}

void CameraManager::remove(const QString &id)
{
    if (!Users::current().isAdmin()) { SecurityAudit::write(QStringLiteral("permission_denied"), QStringLiteral("camera.remove")); return; }
    SecurityAudit::write(QStringLiteral("camera_removed"), id);
    stopWorker(id);
    for (int i = 0; i < m_cameras.size(); ++i) {
        if (m_cameras[i].id == id) {
            m_cameras.removeAt(i);
            break;
        }
    }
    m_states.remove(id);
    m_online.remove(id);
    save();
    emit camerasChanged();
    emit onlineChanged();
}

void CameraManager::startCamera(const QString &id)
{
    if (isStarted(id))
        return;
    if (const CameraEntry *e = find(id))
        syncWorker(*e);
}

void CameraManager::stopCamera(const QString &id)
{
    stopWorker(id);
    m_states.insert(id, CameraWorker::State::Stopped);
    emit cameraStateChanged(id, CameraWorker::State::Stopped);
    emit onlineChanged();
}

void CameraManager::startAll()
{
    for (const CameraEntry &e : std::as_const(m_cameras))
        startCamera(e.id);
}

void CameraManager::stopAll()
{
    for (const CameraEntry &e : std::as_const(m_cameras))
        if (isStarted(e.id))
            stopCamera(e.id);
}

void CameraManager::applyRecordingSchedule()
{
    QSettings settings;
    const bool globalEnabled = settings.value(QStringLiteral("recordSchedule/enabled"), false).toBool();
    const QTime globalFrom = settings.value(QStringLiteral("recordSchedule/from"), QTime(0, 0)).toTime();
    const QTime globalTo = settings.value(QStringLiteral("recordSchedule/to"), QTime(23, 59)).toTime();
    const QDateTime current = QDateTime::currentDateTime();
    const QTime now = current.time();

    auto scheduleAllows = [&](const QTime &from, const QTime &to, quint8 days) {
        if (!from.isValid() || !to.isValid() || days == 0)
            return false;
        const bool crossesMidnight = from > to;
        const bool inTime = !crossesMidnight
            ? (now >= from && now <= to)
            : (now >= from || now <= to);
        if (!inTime)
            return false;
        QDate scheduleDate = current.date();
        if (crossesMidnight && now <= to)
            scheduleDate = scheduleDate.addDays(-1);
        const int bit = scheduleDate.dayOfWeek() - 1;
        return (days & quint8(1u << bit)) != 0;
    };

    quint8 globalDays = 0;
    for (int day = 1; day <= 7; ++day) {
        if (settings.value(QStringLiteral("recordSchedule/day%1").arg(day), true).toBool())
            globalDays |= quint8(1u << (day - 1));
    }

    for (const CameraEntry &camera : std::as_const(m_cameras)) {
        if (!camera.record)
            continue;
        const bool scheduleConfigured = camera.scheduleEnabled || globalEnabled;
        if (!scheduleConfigured)
            continue; // если расписание не задано, ручное управление сохраняется

        const bool shouldRecord = camera.scheduleEnabled
            ? scheduleAllows(camera.scheduleFrom, camera.scheduleTo, camera.scheduleDays)
            : scheduleAllows(globalFrom, globalTo, globalDays);

        if (shouldRecord) {
            if (!isStarted(camera.id))
                startCamera(camera.id);
        } else if (isStarted(camera.id)) {
            stopCamera(camera.id);
        }
    }
}

void CameraManager::setStorageRoot(const QString &path)
{
    if (path.isEmpty() || path == m_storageRoot)
        return;
    QSettings().setValue(QStringLiteral("storage/video"), path);
    m_storageRoot = path;
    QDir().mkpath(m_storageRoot);

    QStringList ids = m_workers.keys();
    ids += m_startQueue;
    ids.removeDuplicates();
    for (const QString &id : std::as_const(ids))
        if (const CameraEntry *e = find(id))
            syncWorker(*e);        // перезапуск с новой папкой
}

void CameraManager::shutdown()
{
    m_startQueue.clear();
    m_startTimer->stop();
    for (CameraWorker *w : std::as_const(m_workers))
        w->stop();
    for (CameraWorker *w : std::as_const(m_workers))
        w->wait();
    qDeleteAll(m_workers);
    m_workers.clear();
}

void CameraManager::load()
{
    QSettings s;
    const int n = s.beginReadArray(QStringLiteral("cameras"));
    bool needsCredentialMigration = false;
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        CameraEntry e;
        e.id = s.value(QStringLiteral("id")).toString();
        e.name = s.value(QStringLiteral("name")).toString();
        e.group = s.value(QStringLiteral("group")).toString();
        const QString storedUrl = s.value(QStringLiteral("url")).toString();
        if (!storedUrl.isEmpty() && !storedUrl.startsWith(QLatin1String("enc:v2:"))
            && !storedUrl.startsWith(QLatin1String("legacy:v1:")))
            needsCredentialMigration = true;
        e.url = storedUrl.startsWith(QLatin1String("enc:v2:"))
            ? SecureStore::decrypt(storedUrl.mid(7))
            : ((storedUrl.startsWith(QLatin1String("enc:v1:")) || storedUrl.startsWith(QLatin1String("legacy:v1:")))
                ? SecureStore::decryptLegacy(storedUrl.mid(storedUrl.startsWith(QLatin1String("legacy:v1:")) ? 10 : 7)) : storedUrl);
        const QString storedLiveUrl = s.value(QStringLiteral("liveUrl")).toString();
        if (!storedLiveUrl.isEmpty() && !storedLiveUrl.startsWith(QLatin1String("enc:v2:"))
            && !storedLiveUrl.startsWith(QLatin1String("legacy:v1:")))
            needsCredentialMigration = true;
        e.webUrl = s.value(QStringLiteral("webUrl")).toString();
        e.bitrateKbps = qBound(64, s.value(QStringLiteral("bitrateKbps"), 4096).toInt(), 200000);
        e.codec = s.value(QStringLiteral("codec"), QStringLiteral("H.264")).toString();
        e.liveUrl = storedLiveUrl.startsWith(QLatin1String("enc:v2:"))
            ? SecureStore::decrypt(storedLiveUrl.mid(7))
            : ((storedLiveUrl.startsWith(QLatin1String("enc:v1:")) || storedLiveUrl.startsWith(QLatin1String("legacy:v1:")))
                ? SecureStore::decryptLegacy(storedLiveUrl.mid(storedLiveUrl.startsWith(QLatin1String("legacy:v1:")) ? 10 : 7)) : storedLiveUrl);
        e.record = s.value(QStringLiteral("record"), false).toBool();
        e.scheduleEnabled = s.value(QStringLiteral("scheduleEnabled"), false).toBool();
        e.scheduleFrom = s.value(QStringLiteral("scheduleFrom"), QTime(0, 0)).toTime();
        if (!e.scheduleFrom.isValid()) e.scheduleFrom = QTime(0, 0);
        e.scheduleTo = s.value(QStringLiteral("scheduleTo"), QTime(23, 59)).toTime();
        if (!e.scheduleTo.isValid()) e.scheduleTo = QTime(23, 59);
        e.scheduleDays = quint8(s.value(QStringLiteral("scheduleDays"), 0x7f).toUInt() & 0x7f);
        e.viewQuality = s.value(QStringLiteral("viewQuality"), 0).toInt();
        e.brightness = s.value(QStringLiteral("brightness"), 0).toInt();
        e.showTime = s.value(QStringLiteral("showTime"), true).toBool();
        e.audio = s.value(QStringLiteral("audio"), false).toBool();
        e.fillCell = s.value(QStringLiteral("fillCell"), false).toBool();
        e.ptzXAddr = s.value(QStringLiteral("ptzXAddr")).toString();
        e.ptzToken = s.value(QStringLiteral("ptzToken")).toString();
        e.ptzUser = s.value(QStringLiteral("ptzUser")).toString();
        const QString storedPtzPassword = s.value(QStringLiteral("ptzPassword")).toString();
        if (!storedPtzPassword.isEmpty() && !storedPtzPassword.startsWith(QLatin1String("enc:v2:"))
            && !storedPtzPassword.startsWith(QLatin1String("legacy:v1:")))
            needsCredentialMigration = true;
        e.ptzPassword = storedPtzPassword.startsWith(QLatin1String("enc:v2:"))
            ? SecureStore::decrypt(storedPtzPassword.mid(7))
            : ((storedPtzPassword.startsWith(QLatin1String("enc:v1:")) || storedPtzPassword.startsWith(QLatin1String("legacy:v1:")))
                ? SecureStore::decryptLegacy(storedPtzPassword.mid(storedPtzPassword.startsWith(QLatin1String("legacy:v1:")) ? 10 : 7)) : storedPtzPassword);
        if (!e.id.isEmpty())
            m_cameras.append(e);
    }
    s.endArray();

    QSettings migration;
    const QString migrationKey = QStringLiteral("migration/localRecordingOptIn");
    if (!migration.value(migrationKey, false).toBool()) {
        // Existing camera definitions are retained, but local recording is opt-in.
        // The device list and cell/group placement are not changed.
        for (CameraEntry &entry : m_cameras)
            entry.record = false;
        migration.setValue(migrationKey, true);
        save();
    } else if (needsCredentialMigration) {
        save(); // rewrite legacy credentials in the current encrypted format
    }

    if (!isProVersion() && m_cameras.size() > 10) {
        m_cameras = m_cameras.mid(0, 10);
        save();
        QTimer::singleShot(0, [] {
            QMessageBox::information(nullptr, T("Демо-версия"),
                T("В демо-режиме доступны первые 10 подключённых камер. Остальные устройства не загружены."));
        });
    }
}

void CameraManager::save() const
{
    QSettings s;
    s.remove(QStringLiteral("cameras"));
    s.beginWriteArray(QStringLiteral("cameras"), int(m_cameras.size()));
    for (int i = 0; i < m_cameras.size(); ++i) {
        s.setArrayIndex(i);
        const CameraEntry &e = m_cameras.at(i);
        s.setValue(QStringLiteral("id"), e.id);
        s.setValue(QStringLiteral("name"), e.name);
        s.setValue(QStringLiteral("group"), e.group);
        s.setValue(QStringLiteral("webUrl"), e.webUrl);
        s.setValue(QStringLiteral("bitrateKbps"), e.bitrateKbps);
        s.setValue(QStringLiteral("codec"), e.codec);
        const QString encrypted_url = SecureStore::encrypt(e.url);
        s.setValue(QStringLiteral("url"), e.url.isEmpty() ? QString() :
                   (encrypted_url.startsWith(QLatin1String("legacy:v1:"))
                        ? encrypted_url : QStringLiteral("enc:v2:") + encrypted_url));
        const QString encrypted_liveUrl = SecureStore::encrypt(e.liveUrl);
        s.setValue(QStringLiteral("liveUrl"), e.liveUrl.isEmpty() ? QString() :
                   (encrypted_liveUrl.startsWith(QLatin1String("legacy:v1:"))
                        ? encrypted_liveUrl : QStringLiteral("enc:v2:") + encrypted_liveUrl));
        s.setValue(QStringLiteral("record"), e.record);
        s.setValue(QStringLiteral("scheduleEnabled"), e.scheduleEnabled);
        s.setValue(QStringLiteral("scheduleFrom"), e.scheduleFrom);
        s.setValue(QStringLiteral("scheduleTo"), e.scheduleTo);
        s.setValue(QStringLiteral("scheduleDays"), uint(e.scheduleDays));
        s.setValue(QStringLiteral("viewQuality"), e.viewQuality);
        s.setValue(QStringLiteral("brightness"), e.brightness);
        s.setValue(QStringLiteral("showTime"), e.showTime);
        s.setValue(QStringLiteral("audio"), e.audio);
        s.setValue(QStringLiteral("fillCell"), e.fillCell);
        s.setValue(QStringLiteral("ptzXAddr"), e.ptzXAddr);
        s.setValue(QStringLiteral("ptzToken"), e.ptzToken);
        s.setValue(QStringLiteral("ptzUser"), e.ptzUser);
        const QString encrypted_ptzPassword = SecureStore::encrypt(e.ptzPassword);
        s.setValue(QStringLiteral("ptzPassword"), e.ptzPassword.isEmpty() ? QString() :
                   (encrypted_ptzPassword.startsWith(QLatin1String("legacy:v1:"))
                        ? encrypted_ptzPassword : QStringLiteral("enc:v2:") + encrypted_ptzPassword));
    }
    s.endArray();
    s.sync();
    if (s.status() != QSettings::NoError)
        writePsvmsLog(QStringLiteral("CameraManager: failed to persist camera settings securely."));
}

void CameraManager::stopWorker(const QString &id)
{
    m_startQueue.removeAll(id);
    if (CameraWorker *w = m_workers.take(id)) {
        w->stop();
        w->wait();
        delete w;
    }
}

void CameraManager::syncWorker(const CameraEntry &e)
{
    stopWorker(e.id);
    // Аналоговая камера через карту видеозахвата: это не RTSP-поток, рекордер на базе FFmpeg
    // открыть его не может — для таких камер доступен только живой просмотр
    if (!e.record || e.url.startsWith(QLatin1String("qcam:"))) {
        m_states.insert(e.id, CameraWorker::State::Stopped);
        emit cameraStateChanged(e.id, CameraWorker::State::Stopped);
        return;
    }
    // запуск не сразу, а через очередь
    m_states.insert(e.id, CameraWorker::State::Connecting);
    emit cameraStateChanged(e.id, CameraWorker::State::Connecting);
    m_startQueue.append(e.id);
    if (!m_startTimer->isActive())
        m_startTimer->start();
}

void CameraManager::startNext()
{
    if (m_startQueue.isEmpty()) {
        m_startTimer->stop();
        return;
    }
    const QString id = m_startQueue.takeFirst();
    const CameraEntry *e = find(id);
    if (!e || !e->record || m_workers.contains(id))
        return;

    CameraConfig cfg;
    cfg.id = e->id;
    cfg.name = e->name;
    cfg.url = e->url;
    cfg.storageRoot = m_storageRoot;
    cfg.dbPath = dbPath();
    cfg.minimumFreeBytes = qint64(qBound(256, QSettings().value(QStringLiteral("storage/minFreeMB"), 1024).toInt(), 20480))
        * 1024LL * 1024LL;

    auto *w = new CameraWorker(cfg, this);
    connect(w, &CameraWorker::stateChanged, this, [this](const QString &cid, CameraWorker::State st) {
        m_states.insert(cid, st);
        emit cameraStateChanged(cid, st);
        emit onlineChanged();
    });
    connect(w, &CameraWorker::errorOccurred, this, &CameraManager::cameraError);
    m_workers.insert(e->id, w);
    w->startRecording();
}

// ---- проверка доступности: короткое TCP-подключение к порту камеры ----

void CameraManager::probeNow()
{
    if (m_probing)
        return;
    m_probeQueue.clear();
    for (const CameraEntry &e : std::as_const(m_cameras))
        m_probeQueue.append(e.id);
    m_probeResults.clear();
    m_probing = true;
    pumpProbe();
}

void CameraManager::pumpProbe()
{
    while (m_probeActive < 16 && !m_probeQueue.isEmpty()) {
        const QString id = m_probeQueue.takeFirst();
        const CameraEntry *e = find(id);
        if (!e)
            continue;
        const QUrl u(e->url);
        const QString host = u.host();
        if (host.isEmpty()) {
            m_probeResults.insert(id, false);
            continue;
        }

        // Состояние «В сети» не должно зависеть только от RTSP-порта: часть камер
        // закрывает RTSP для неавторизованных клиентов, но отвечает на HTTP/HTTPS.
        QList<quint16> ports;
        auto appendPort = [&ports](int p) {
            if (p > 0 && p <= 65535 && !ports.contains(quint16(p)))
                ports.append(quint16(p));
        };
        appendPort(u.port(u.scheme() == QLatin1String("rtsps") ? 322 : 554));
        appendPort(554);
        appendPort(80);
        appendPort(8000);
        appendPort(443);

        ++m_probeActive;
        auto index = std::make_shared<int>(0);
        auto attempt = std::make_shared<std::function<void()>>();
        *attempt = [this, id, host, ports, index, attempt]() {
            if (*index >= ports.size()) {
                m_probeResults.insert(id, false);
                --m_probeActive;
                *attempt = {};
                pumpProbe();
                return;
            }
            const quint16 port = ports.at((*index)++);
            auto *sock = new QTcpSocket(this);
            auto socketDone = std::make_shared<bool>(false);
            auto finish = [this, sock, id, socketDone, attempt](bool ok) {
                if (*socketDone)
                    return;
                *socketDone = true;
                sock->abort();
                sock->deleteLater();
                if (ok) {
                    m_probeResults.insert(id, true);
                    --m_probeActive;
                    *attempt = {};
                    pumpProbe();
                } else {
                    (*attempt)();
                }
            };
            connect(sock, &QTcpSocket::connected, this, [finish] { finish(true); });
            connect(sock, &QAbstractSocket::errorOccurred, this,
                    [finish](QAbstractSocket::SocketError) { finish(false); });
            QTimer::singleShot(1200, sock, [finish] { finish(false); });
            sock->connectToHost(host, port);
        };
        (*attempt)();
    }

    if (m_probing && m_probeQueue.isEmpty() && m_probeActive == 0) {
        m_probing = false;
        m_online = m_probeResults;
        emit onlineChanged();
    }
}

// ======================================================================
// app/cloud_dialog.cpp
// ======================================================================
CloudDialog::CloudDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(T("Облако Hik-Connect"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(520);

    QSettings s;

    auto *info = new QLabel(
        T("Hik-Connect — облачный сервис Hikvision. Подключение к нему из сторонних программ "
          "возможно только через официальный SDK и ключи разработчика Hikvision (Open Platform), "
          "поэтому в этой версии получение камер из облака не работает."), this);
    info->setWordWrap(true);
    info->setStyleSheet(QStringLiteral("color:#63e6ee;"));

    m_region = new QComboBox(this);
    m_region->addItems({T("Россия и СНГ"), T("Европа"), T("Азия"), T("Америка"), T("Другой регион")});
    m_region->setCurrentIndex(qBound(0, s.value(QStringLiteral("cloud/region"), 0).toInt(), 4));
    m_account = new QLineEdit(s.value(QStringLiteral("cloud/account")).toString(), this);
    m_account->setPlaceholderText(T("e-mail или телефон"));
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);

    m_status = new QLabel(T("Статус: не подключено"), this);

    auto *form = new QFormLayout;
    form->addRow(T("Регион"), m_region);
    form->addRow(T("Аккаунт"), m_account);
    form->addRow(T("Пароль"), m_password);

    auto *connectBtn = new QPushButton(T("Подключиться"), this);
    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(connectBtn, QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close)->setText(T("Закрыть"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked, this, &QDialog::accept);

    connect(connectBtn, &QPushButton::clicked, this, [this] {
        QSettings st;
        st.setValue(QStringLiteral("cloud/region"), m_region->currentIndex());
        st.setValue(QStringLiteral("cloud/account"), m_account->text().trimmed());   // пароль не сохраняем
        m_status->setText(T("Статус: недоступно в этой версии (нужны ключи Hikvision Open Platform). "
                            "Добавляйте камеры по IP в локальной сети."));
        m_status->setWordWrap(true);
    });

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(info);
    lay->addLayout(form);
    lay->addWidget(m_status);
    lay->addWidget(buttons);
}

// ======================================================================
// app/credentials_dialog.cpp
// ======================================================================
bool askCredentials(QWidget *parent, const QString &address, QString *user, QString *password)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(T("Нужен пароль"));
    dlg.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dlg.setMinimumWidth(400);

    auto *hint = new QLabel(T("Устройство %1 требует логин и пароль.\nВведите данные для входа.").arg(address), &dlg);
    hint->setWordWrap(true);
    auto *u = new QLineEdit(user ? *user : QString(), &dlg);
    auto *p = new QLineEdit(password ? *password : QString(), &dlg);
    p->setEchoMode(QLineEdit::Password);
    p->setPlaceholderText(T("Пароль устройства"));

    auto *form = new QFormLayout;
    form->addRow(T("Логин"), u);
    form->addRow(T("Пароль"), p);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    localizeButtons(buttons);
    buttons->button(QDialogButtonBox::Cancel)->setText(T("Пропустить"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(hint);
    lay->addLayout(form);
    lay->addWidget(buttons);

    p->setFocus();
    if (dlg.exec() != QDialog::Accepted)
        return false;
    if (user)
        *user = u->text().trimmed();
    if (password)
        *password = p->text();
    return true;
}

// ======================================================================
