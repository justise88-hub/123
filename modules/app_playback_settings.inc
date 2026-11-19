// PSVMS module: Архив, PTZ, настройки, навигация и панели
// This file is included by psvms.cpp to preserve the original single-TU build
// while the code is being separated safely.

// app/playback_page.cpp
// ======================================================================
namespace {

QString formatMs(qint64 ms)
{
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

PlaybackPage::PlaybackPage(CameraManager *mgr, QWidget *parent) : QWidget(parent), m_mgr(mgr)
{
    m_index.open(mgr->dbPath());

    // ---- левая колонка: простой поиск и навигация по архиву ----
    auto *archiveTitle = new QLabel(T("АРХИВ ЗАПИСЕЙ"), this);
    archiveTitle->setStyleSheet(QStringLiteral(
        "QLabel { color:#8beaf0; background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #3b4249,stop:1 #22272d); "
        "border:1px solid #287f9e; border-radius:7px; padding:11px 12px; font-size:10pt; font-weight:800; }"));
    auto *archiveHint = new QLabel(T("Выберите камеру и дату, затем откройте нужную запись."), this);
    archiveHint->setWordWrap(true);
    archiveHint->setStyleSheet(QStringLiteral("color:#8995a5; font-size:10pt; padding:2px 3px;"));

    m_camera = new QComboBox(this);
    m_camera->setMinimumHeight(34);
    m_calendar = new QCalendarWidget(this);
    m_calendar->setGridVisible(true);
    m_calendar->setMaximumDate(QDate::currentDate());
    m_calendar->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);
    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(true);
    m_list->setSpacing(3);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(T("Поиск записи по времени…"));
    m_archiveCount = new QLabel(this);
    m_archiveCount->setStyleSheet(QStringLiteral("color:#7d8998; font-size:10pt;"));

    auto *quickDates = new QHBoxLayout;
    auto *todayBtn = new QPushButton(T("Сегодня"), this);
    auto *yesterdayBtn = new QPushButton(T("Вчера"), this);
    todayBtn->setToolTip(T("Показать записи за сегодня"));
    yesterdayBtn->setToolTip(T("Показать записи за вчера"));
    quickDates->addWidget(todayBtn);
    quickDates->addWidget(yesterdayBtn);
    auto *refresh = new QPushButton(T("⟳  Обновить архив"), this);
    auto *exportBtn = new QPushButton(T("⇩  Экспортировать выбранную запись"), this);
    exportBtn->setObjectName(QStringLiteral("archiveExportButton"));
    exportBtn->setToolTip(T("Сохранить выбранный сегмент архива в отдельный MKV-файл"));

    auto *left = new QVBoxLayout;
    left->setSpacing(8);
    left->addWidget(archiveTitle);
    left->addWidget(archiveHint);
    left->addWidget(new QLabel(T("Камера"), this));
    left->addWidget(m_camera);
    left->addLayout(quickDates);
    left->addWidget(m_calendar);
    left->addWidget(new QLabel(T("Записи за выбранный день"), this));
    left->addWidget(m_search);
    left->addWidget(m_archiveCount);
    left->addWidget(m_list, 1);
    left->addWidget(refresh);
    left->addWidget(exportBtn);

    auto *leftBox = new QWidget(this);
    leftBox->setLayout(left);
    leftBox->setFixedWidth(360);
    leftBox->setStyleSheet(QStringLiteral(
        "QListWidget { border:1px solid #59616a; border-radius:7px; padding:4px; "
        "background:#1a1e23; alternate-background-color:#22272d; }"
        "QListWidget::item { padding:8px 7px; border-radius:5px; border-bottom:1px solid #343a41; }"
        "QListWidget::item:hover { background:#343b42; }"
        "QListWidget::item:selected { background:#17647a; color:#f2ffff; }"
        "QPushButton { min-height:30px; }"));

    connect(todayBtn, &QPushButton::clicked, this, [this] {
        m_calendar->setSelectedDate(QDate::currentDate());
    });
    connect(yesterdayBtn, &QPushButton::clicked, this, [this] {
        m_calendar->setSelectedDate(QDate::currentDate().addDays(-1));
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &query) {
        int visible = 0;
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem *item = m_list->item(i);
            const bool show = item->data(Qt::UserRole).isValid()
                && item->text().contains(query.trimmed(), Qt::CaseInsensitive);
            item->setHidden(!show);
            if (show) ++visible;
        }
        if (m_archiveCount)
            m_archiveCount->setText(T("Найдено: %1").arg(visible));
    });

    // ---- правая часть: видео и управление ----
    m_video = new QVideoWidget(this);
    m_video->setToolTip(T("Двойной щелчок — открыть или закрыть полноэкранный просмотр"));
    m_video->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    m_player->setVideoOutput(m_video);

    m_playBtn = new QPushButton(QStringLiteral("▶"), this);
    m_playBtn->setFixedWidth(48);
    m_slider = new QSlider(Qt::Horizontal, this);
    m_time = new QLabel(QStringLiteral("00:00 / 00:00"), this);

    auto *controls = new QHBoxLayout;
    controls->addWidget(m_playBtn);
    controls->addWidget(m_slider, 1);
    controls->addWidget(m_time);

    m_videoLayout = new QVBoxLayout;
    m_videoLayout->addWidget(m_video, 1);
    m_videoLayout->addLayout(controls);
    m_video->installEventFilter(this);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->addWidget(leftBox);
    root->addLayout(m_videoLayout, 1);

    // ---- сигналы ----
    connect(m_mgr, &CameraManager::camerasChanged, this, &PlaybackPage::reloadCameras);
    connect(m_camera, &QComboBox::currentIndexChanged, this, &PlaybackPage::reloadSegments);
    connect(m_calendar, &QCalendarWidget::selectionChanged, this, &PlaybackPage::reloadSegments);
    connect(refresh, &QPushButton::clicked, this, &PlaybackPage::reloadSegments);
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        const QListWidgetItem *item = m_list->currentItem();
        if (!item || !item->data(Qt::UserRole).isValid()) {
            QMessageBox::information(this, T("Экспорт архива"), T("Сначала выберите запись в списке."));
            return;
        }
        const QString sourcePath = item->data(Qt::UserRole).toString();
        if (!QFileInfo::exists(sourcePath)) {
            QMessageBox::warning(this, T("Экспорт архива"), T("Файл записи не найден на диске."));
            reloadSegments();
            return;
        }
        QString suggested = QFileInfo(sourcePath).completeBaseName() + QStringLiteral(".mkv");
        const QString targetPath = QFileDialog::getSaveFileName(this, T("Экспортировать запись"),
            QDir::toNativeSeparators(QDir::home().filePath(suggested)),
            T("Видео Matroska (*.mkv);;Все файлы (*.*)"));
        if (targetPath.isEmpty())
            return;
        QFile input(sourcePath);
        if (!input.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, T("Экспорт архива"), T("Не удалось открыть запись: %1").arg(input.errorString()));
            return;
        }
        QSaveFile output(targetPath);
        if (!output.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, T("Экспорт архива"), T("Не удалось создать файл: %1").arg(output.errorString()));
            return;
        }
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        while (!input.atEnd()) {
            const qint64 bytesRead = input.read(buffer.data(), buffer.size());
            if (bytesRead < 0 || output.write(buffer.constData(), bytesRead) != bytesRead) {
                output.cancelWriting();
                QMessageBox::warning(this, T("Экспорт архива"), T("Ошибка чтения или записи при экспорте."));
                return;
            }
        }
        if (!output.commit()) {
            QMessageBox::warning(this, T("Экспорт архива"), T("Не удалось завершить экспорт: %1").arg(output.errorString()));
            return;
        }
        QMessageBox::information(this, T("Экспорт архива"), T("Запись успешно экспортирована."));
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, &PlaybackPage::playCurrent);
    connect(m_playBtn, &QPushButton::clicked, this, &PlaybackPage::togglePlay);
    auto *spacePlayback = new QShortcut(QKeySequence(Qt::Key_Space), this);
    spacePlayback->setContext(Qt::WidgetWithChildrenShortcut);
    connect(spacePlayback, &QShortcut::activated, this, [this] {
        QWidget *focus = QApplication::focusWidget();
        if (qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus))
            return;
        togglePlay();
    });

    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 d) {
        m_slider->setRange(0, int(d));
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 p) {
        if (!m_slider->isSliderDown())
            m_slider->setValue(int(p));
        m_time->setText(formatMs(p) + QStringLiteral(" / ") + formatMs(m_player->duration()));
    });
    connect(m_slider, &QSlider::sliderMoved, m_player, &QMediaPlayer::setPosition);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState s) {
        m_playBtn->setText(s == QMediaPlayer::PlayingState ? QStringLiteral("⏸") : QStringLiteral("▶"));
    });

    reloadCameras();
}

bool PlaybackPage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_video && event->type() == QEvent::MouseButtonDblClick) {
        if (!m_videoFullscreen) {
            auto *dialog = new QDialog(this, Qt::Window);
            dialog->setWindowTitle(T("Архив — полноэкранный просмотр"));
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            auto *layout = new QVBoxLayout(dialog);
            layout->setContentsMargins(0, 0, 0, 0);
            m_videoLayout->removeWidget(m_video);
            m_video->setParent(dialog);
            layout->addWidget(m_video);
            m_videoFullscreen = dialog;
            connect(dialog, &QDialog::finished, this, [this, dialog] {
                if (m_video) {
                    m_video->setParent(this);
                    m_videoLayout->insertWidget(0, m_video, 1);
                }
                m_videoFullscreen = nullptr;
                dialog->deleteLater();
            });
            dialog->showFullScreen();
        } else {
            m_videoFullscreen->close();
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void PlaybackPage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    m_calendar->setMaximumDate(QDate::currentDate());
    reloadSegments();   // подхватить сегменты, записанные пока страница была скрыта
}

void PlaybackPage::reloadCameras()
{
    const QString keep = m_camera->currentData().toString();

    m_camera->blockSignals(true);
    m_camera->clear();
    for (const CameraEntry &e : m_mgr->cameras())
        m_camera->addItem(e.name, e.id);
    const int idx = m_camera->findData(keep);
    if (idx >= 0)
        m_camera->setCurrentIndex(idx);
    m_camera->blockSignals(false);

    reloadSegments();
}

void PlaybackPage::reloadSegments()
{
    m_list->clear();
    const QString camId = m_camera->currentData().toString();
    if (camId.isEmpty()) {
        m_archiveCount->setText(T("Добавьте камеру, чтобы просматривать архив."));
        auto *empty = new QListWidgetItem(T("Нет выбранной камеры"), m_list);
        empty->setFlags(Qt::NoItemFlags);
        return;
    }

    const QDate day = m_calendar->selectedDate();
    const qint64 from = day.startOfDay().toMSecsSinceEpoch();
    const qint64 to = day.addDays(1).startOfDay().toMSecsSinceEpoch();
    const QList<SegmentInfo> segments = m_index.segmentsInRange(camId, from, to);

    for (const SegmentInfo &s : segments) {
        QString text = QDateTime::fromMSecsSinceEpoch(s.startMs).toString(QStringLiteral("HH:mm:ss"))
                       + QStringLiteral("  —  ");
        text += s.endMs ? QDateTime::fromMSecsSinceEpoch(s.endMs).toString(QStringLiteral("HH:mm:ss"))
                        : T("идёт запись");
        if (s.sizeBytes > 0)
            text += T("   ·   %1 МБ").arg(s.sizeBytes / 1048576.0, 0, 'f', 1);

        auto *it = new QListWidgetItem(QStringLiteral("▶  ") + text, m_list);
        it->setData(Qt::UserRole, s.path);
        it->setToolTip(T("Дважды щёлкните, чтобы воспроизвести запись"));
    }

    if (segments.isEmpty()) {
        auto *empty = new QListWidgetItem(T("За этот день записей нет.\nВыберите другую дату или нажмите «Обновить архив»."), m_list);
        empty->setFlags(Qt::NoItemFlags);
        empty->setTextAlignment(Qt::AlignCenter);
    }
    m_archiveCount->setText(T("Записей: %1").arg(segments.size()));
    const QString query = m_search->text().trimmed();
    if (!query.isEmpty()) {
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem *item = m_list->item(i);
            item->setHidden(!item->data(Qt::UserRole).isValid()
                            || !item->text().contains(query, Qt::CaseInsensitive));
        }
    }
}

void PlaybackPage::playCurrent()
{
    const QListWidgetItem *it = m_list->currentItem();
    if (!it)
        return;
    const QString path = it->data(Qt::UserRole).toString();
    if (!QFileInfo::exists(path))
        return;
    m_player->setSource(QUrl::fromLocalFile(path));
    m_player->play();
}

void PlaybackPage::togglePlay()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else
        m_player->play();
}

void PlaybackPage::selectCamera(const QString &id)
{
    const int idx = m_camera->findData(id);
    if (idx >= 0)
        m_camera->setCurrentIndex(idx);
}

// ======================================================================
// app/playback_window.cpp
// ======================================================================
PlaybackWindow::PlaybackWindow(CameraManager *mgr, const QString &cameraId, QWidget *parent) : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(T("Архив"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(1180, 700);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    auto *page = new PlaybackPage(mgr, this);
    lay->addWidget(page);
    if (!cameraId.isEmpty())
        page->selectCamera(cameraId);
}

// ======================================================================
// app/ptz_panel.cpp
// ======================================================================
namespace {
// Направления по индексу кнопки 0..8 (как на цифровой клавиатуре, без центра): x, y
const double kDirX[9] = {-1, 0, 1, -1, 0, 1, -1, 0, 1};
const double kDirY[9] = {-1, -1, -1, 0, 0, 0, 1, 1, 1};
}

PtzPanel::PtzPanel(CameraManager *mgr, QWidget *parent) : QWidget(parent), m_mgr(mgr)
{
    m_nam = new QNetworkAccessManager(this);

    auto *box = new QGroupBox(T("PTZ-управление"), this);
    auto *lay = new QGridLayout(box);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setHorizontalSpacing(3);
    lay->setVerticalSpacing(3);

    const char *arrows[9] = {"↖", "↑", "↗", "←", "●", "→", "↙", "↓", "↘"};
    for (int i = 0; i < 9; ++i) {
        auto *b = new QToolButton(box);
        b->setText(QString::fromUtf8(arrows[i]));
        b->setFixedSize(42, 37);
        b->setStyleSheet(QStringLiteral("QToolButton{padding:0px;font-size:10pt;min-width:0px;min-height:0px;}"));
        b->setEnabled(false);
        if (i != 4) {   // центральная кнопка декоративная, движения не делает
            connect(b, &QToolButton::pressed, this, [this, i] {
                const CameraEntry *e = m_mgr->find(m_cameraId);
                if (!e || !e->hasPtz())
                    return;
                const double speed = m_speed->value() / 8.0;
                ptzContinuousMove(m_nam, {e->ptzXAddr, e->ptzToken, e->ptzUser, e->ptzPassword},
                                  kDirX[i] * speed, -kDirY[i] * speed, 0.0);   // y камеры обычно вверх — положительный
            });
            connect(b, &QToolButton::released, this, [this] {
                if (const CameraEntry *e = m_mgr->find(m_cameraId))
                    ptzStop(m_nam, {e->ptzXAddr, e->ptzToken, e->ptzUser, e->ptzPassword});
            });
        } else {
            b->setEnabled(false);
        }
        m_dirs[i] = b;
        lay->addWidget(b, i / 3, i % 3, Qt::AlignCenter);
    }

    m_zoomOut = new QToolButton(box);
    m_zoomOut->setText(QStringLiteral("−"));
    m_zoomOut->setFixedSize(36, 31);
    m_zoomOut->setEnabled(false);
    m_zoomIn = new QToolButton(box);
    m_zoomIn->setText(QStringLiteral("+"));
    m_zoomIn->setFixedSize(36, 31);
    m_zoomIn->setEnabled(false);
    lay->addWidget(new QLabel(T("Зум"), box), 3, 0);
    lay->addWidget(m_zoomOut, 3, 1, Qt::AlignCenter);
    lay->addWidget(m_zoomIn, 3, 2, Qt::AlignCenter);

    connect(m_zoomIn, &QToolButton::pressed, this, [this] {
        if (const CameraEntry *e = m_mgr->find(m_cameraId))
            ptzContinuousMove(m_nam, {e->ptzXAddr, e->ptzToken, e->ptzUser, e->ptzPassword}, 0, 0, m_speed->value() / 8.0);
    });
    connect(m_zoomOut, &QToolButton::pressed, this, [this] {
        if (const CameraEntry *e = m_mgr->find(m_cameraId))
            ptzContinuousMove(m_nam, {e->ptzXAddr, e->ptzToken, e->ptzUser, e->ptzPassword}, 0, 0, -m_speed->value() / 8.0);
    });
    connect(m_zoomIn, &QToolButton::released, this, [this] {
        if (const CameraEntry *e = m_mgr->find(m_cameraId))
            ptzStop(m_nam, {e->ptzXAddr, e->ptzToken, e->ptzUser, e->ptzPassword});
    });
    connect(m_zoomOut, &QToolButton::released, this, [this] {
        if (const CameraEntry *e = m_mgr->find(m_cameraId))
            ptzStop(m_nam, {e->ptzXAddr, e->ptzToken, e->ptzUser, e->ptzPassword});
    });

    m_speed = new QSlider(Qt::Horizontal, box);
    m_speed->setRange(1, 8);
    m_speed->setValue(4);
    lay->addWidget(new QLabel(T("Скорость"), box), 4, 0);
    lay->addWidget(m_speed, 4, 1, 1, 2);

    m_note = new QLabel(T("Выберите поворотную камеру, добавленную по ONVIF"), box);
    static_cast<QLabel *>(m_note)->setWordWrap(true);
    m_note->setStyleSheet(QStringLiteral("color:#6b7480; font-size:10pt;"));
    lay->addWidget(m_note, 5, 0, 1, 3);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(box);

    connect(m_mgr, &CameraManager::camerasChanged, this, [this] { updateEnabled(); });
}

void PtzPanel::setCamera(const QString &cameraId)
{
    m_cameraId = cameraId;
    updateEnabled();
}

void PtzPanel::updateEnabled()
{
    const CameraEntry *e = m_mgr->find(m_cameraId);
    const bool on = isProVersion() && QSettings().value(QStringLiteral("camera/ptzEnabled"), true).toBool() && e && e->hasPtz();
    for (int i = 0; i < 9; ++i)
        if (i != 4)
            m_dirs[i]->setEnabled(on);
    m_zoomIn->setEnabled(on);
    m_zoomOut->setEnabled(on);
    m_speed->setEnabled(on);
    m_note->setVisible(!on);
}

// ======================================================================
// app/rtsp_quick_dialog.cpp
// ======================================================================
RtspQuickDialog::RtspQuickDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(T("Быстрое добавление по RTSP"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(520);

    auto *hint = new QLabel(T("Для камер и регистраторов, адрес потока которых у вас уже есть."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#8a8a8e;"));

    m_name = new QLineEdit(this);
    m_url = new QLineEdit(this);
    m_url->setPlaceholderText(T("rtsp://логин:пароль@192.168.1.64:554/Streaming/Channels/101"));
    m_group = new QLineEdit(this);
    m_group->setPlaceholderText(T("необязательно"));
    m_record = new QCheckBox(T("Вести непрерывную запись в архив"), this);
    m_record->setChecked(false);

    auto *form = new QFormLayout;
    form->addRow(T("Название"), m_name);
    form->addRow(T("Адрес RTSP"), m_url);
    form->addRow(T("Группа"), m_group);
    form->addRow(QString(), m_record);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    localizeButtons(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(hint);
    lay->addLayout(form);
    lay->addWidget(buttons);
}

CameraEntry RtspQuickDialog::entry() const
{
    CameraEntry e;
    e.name = m_name->text().trimmed();
    e.url = m_url->text().trimmed();
    e.group = m_group->text().trimmed();
    e.record = m_record->isChecked();
    return e;
}

void RtspQuickDialog::accept()
{
    const QUrl u(m_url->text().trimmed());
    if (m_name->text().trimmed().isEmpty() || !u.isValid() || u.scheme().compare(QLatin1String("rtsp"), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(this, windowTitle(), T("Укажите название и корректный адрес, начинающийся с rtsp://"));
        return;
    }
    QDialog::accept();
}

// ======================================================================
// app/settings.cpp
// ======================================================================
namespace {

Cfg::View g_view;
bool g_loaded = false;

QString documentsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

} // namespace

CfgNotifier *Cfg::notifier()
{
    static CfgNotifier n;
    return &n;
}

void Cfg::reload()
{
    QSettings s;
    View v;
    v.lowPower = s.value(QStringLiteral("perf/lowPower"), false).toBool();
    v.maxFps = qBound(1, s.value(QStringLiteral("perf/maxFps"), 30).toInt(), 60);
    v.smooth = s.value(QStringLiteral("perf/smooth"), false).toBool();
    v.timestamp = s.value(QStringLiteral("view/timestamp"), true).toBool();
    v.showInfo = s.value(QStringLiteral("view/showInfo"), true).toBool();
    v.showFps = s.value(QStringLiteral("view/showFps"), true).toBool();
    const QString fmt = s.value(QStringLiteral("view/timeFormat")).toString();
    if (!fmt.isEmpty())
        v.timeFormat = fmt;
    if (v.lowPower) {            // слабый компьютер: меньше кадров, без сглаживания и анимаций
        v.maxFps = qMin(v.maxFps, 8);
        v.smooth = false;
    }
    v.animations = !v.lowPower && s.value(QStringLiteral("ui/animations"), true).toBool();
    g_view = v;
    g_loaded = true;
    Q_EMIT notifier()->changed();
}

const Cfg::View &Cfg::view()
{
    if (!g_loaded)
        reload();
    return g_view;
}

bool Cfg::autostartCameras()
{
    return QSettings().value(QStringLiteral("general/autostart"), true).toBool();
}

QString Cfg::videoDir()
{
    return QSettings().value(QStringLiteral("storage/video"),
                             documentsDir() + QStringLiteral("/PSVMS Archive")).toString();
}

QString Cfg::screenshotDir()
{
    return QSettings().value(QStringLiteral("storage/screenshots"),
                             documentsDir() + QStringLiteral("/PSVMS Screenshots")).toString();
}

// ======================================================================
// app/settings_page.cpp
// ======================================================================
namespace {

struct SettingsWinSize {
    int w;
    int h;
};
const SettingsWinSize kSettingsWinSizes[] = {{0, 0},       {1280, 720},  {1600, 900},  {1920, 1080},
                                             {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};

QCheckBox *settingsCheck(QWidget *parent, const QString &text, const QString &key, bool def)
{
    auto *c = new QCheckBox(text, parent);
    c->setChecked(QSettings().value(key, def).toBool());
    QObject::connect(c, &QCheckBox::toggled, c, [key](bool on) {
        QSettings().setValue(key, on);
        Cfg::reload();
    });
    return c;
}

// строка выбора папки: поле + «Обзор…» + «Открыть»
QWidget *settingsFolderRow(QWidget *parent, QLineEdit *edit, const std::function<void(const QString &)> &onPicked)
{
    auto *w = new QWidget(parent);
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    edit->setReadOnly(true);
    auto *browse = new QPushButton(T("Обзор…"), w);
    auto *open = new QPushButton(T("Открыть"), w);
    l->addWidget(edit, 1);
    l->addWidget(browse);
    l->addWidget(open);
    QObject::connect(browse, &QPushButton::clicked, w, [w, edit, onPicked] {
        const QString dir = QFileDialog::getExistingDirectory(w, T("Выберите папку"), edit->text());
        if (!dir.isEmpty()) {
            edit->setText(dir);
            onPicked(dir);
        }
    });
    QObject::connect(open, &QPushButton::clicked, w, [edit] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(edit->text()));
    });
    return w;
}

QWidget *settingsPane(QLayout *layout)
{
    auto *w = new QWidget;
    layout->setContentsMargins(20, 18, 20, 18);
    w->setLayout(layout);
    return w;
}

// Добавление пользователя
class SettingsUserDialog : public QDialog {
public:
    explicit SettingsUserDialog(QWidget *parent) : QDialog(parent)
    {
        setWindowTitle(T("Новый пользователь"));
        setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        setMinimumWidth(420);
        m_name = new QLineEdit(this);
        m_role = new QComboBox(this);
        m_role->addItem(T("Администратор (все разделы)"), QStringLiteral("admin"));
        m_role->addItem(T("Оператор (только просмотр)"), QStringLiteral("operator"));
        m_role->setCurrentIndex(1);
        m_msg = new QLabel(this);
        m_msg->setStyleSheet(QStringLiteral("color:#63e6ee;"));
        m_msg->setWordWrap(true);
        auto *hint = new QLabel(T("Новый пользователь будет создан без пароля. При необходимости пароль можно задать позже."), this);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color:#8a8a8e;"));
        auto *form = new QFormLayout;
        form->addRow(T("Имя"), m_name);
        form->addRow(T("Роль"), m_role);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        localizeButtons(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        auto *lay = new QVBoxLayout(this);
        lay->addWidget(hint);
        lay->addLayout(form);
        lay->addWidget(m_msg);
        lay->addWidget(buttons);
    }
    void accept() override
    {
        QString err;
        if (Users::add(m_name->text(), m_role->currentData().toString(), QString(), &err).isEmpty()) {
            m_msg->setText(err);
            return;
        }
        QDialog::accept();
    }
private:
    QLineEdit *m_name = nullptr;
    QComboBox *m_role = nullptr;
    QLabel *m_msg = nullptr;
};

// Новый пароль выбранного пользователя (пустой — без пароля)
class SettingsPasswordDialog : public QDialog {
public:
    SettingsPasswordDialog(const QString &userId, const QString &userName, QWidget *parent)
        : QDialog(parent), m_id(userId)
    {
        setWindowTitle(T("Пароль пользователя"));
        setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        setMinimumWidth(400);
        m_pass = new QLineEdit(this);
        m_pass->setEchoMode(QLineEdit::Password);
        m_pass2 = new QLineEdit(this);
        m_pass2->setEchoMode(QLineEdit::Password);
        m_msg = new QLabel(this);
        m_msg->setStyleSheet(QStringLiteral("color:#63e6ee;"));
        m_msg->setWordWrap(true);

        auto *hint = new QLabel(T("Новый пароль для пользователя «%1». Оставьте пустым, чтобы убрать пароль.").arg(userName), this);
        hint->setWordWrap(true);
        auto *form = new QFormLayout;
        form->addRow(T("Новый пароль"), m_pass);
        form->addRow(T("Повторите пароль"), m_pass2);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        localizeButtons(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        auto *lay = new QVBoxLayout(this);
        lay->addWidget(hint);
        lay->addLayout(form);
        lay->addWidget(m_msg);
        lay->addWidget(buttons);
    }

    void accept() override
    {
        if (m_pass->text() != m_pass2->text()) {
            m_msg->setText(T("Пароли не совпадают."));
            return;
        }
        QString err;
        if (!Users::setPassword(m_id, m_pass->text(), &err)) {
            m_msg->setText(err);
            return;
        }
        QDialog::accept();
    }

private:
    QString m_id;
    QLineEdit *m_pass = nullptr;
    QLineEdit *m_pass2 = nullptr;
    QLabel *m_msg = nullptr;
};

} // namespace

// Диалог встроенного ИИ-помощника PSVMS на базе Yandex AI Studio.
class YandexAssistantDialog final : public QDialog {
public:
    YandexAssistantDialog(const QString &apiKey, const QString &folderId, QWidget *parent = nullptr)
        : QDialog(parent), m_apiKey(apiKey), m_folderId(folderId), m_nam(new QNetworkAccessManager(this))
    {
        setWindowTitle(T("ИИ-помощник PSVms"));
        setMinimumSize(680, 500);
        m_chat = new QTextEdit(this);
        m_chat->setReadOnly(true);
        m_chat->setPlaceholderText(T("Здесь появятся ответы ИИ-помощника."));
        m_chat->setStyleSheet(QStringLiteral(
            "QTextEdit{color:#ECF0F5;background-color:#12151B;"
            "background-image:url(:/branding/ai_background.png);background-position:center;"
            "background-repeat:no-repeat;border:1px solid #C99600;border-radius:6px;"
            "padding:12px;font-size:10pt;}"));
        m_input = new QLineEdit(this);
        m_input->setPlaceholderText(T("Напишите вопрос о камерах, регистраторах или настройке PSVMS…"));
        auto *send = new QPushButton(T("Отправить"), this);
        auto *close = new QPushButton(T("Закрыть"), this);
        close->setToolTip(T("Закрыть окно ИИ-помощника"));
        auto *hint = new QLabel(T("Ответы генерируются Yandex AI Studio. Не отправляйте пароли и секретные данные."), this);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color:#92929a;"));
        auto *row = new QHBoxLayout;
        row->addWidget(m_input, 1);
        row->addWidget(send);
        row->addWidget(close);
        auto *lay = new QVBoxLayout(this);
        lay->addWidget(m_chat, 1);
        lay->addLayout(row);
        lay->addWidget(hint);
        connect(send, &QPushButton::clicked, this, [this] { sendMessage(); });
        connect(close, &QPushButton::clicked, this, &QDialog::close);
        connect(m_input, &QLineEdit::returnPressed, this, [this] { sendMessage(); });
        m_history.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
            {QStringLiteral("text"), QStringLiteral("Ты — технический помощник программы PSVMS для видеонаблюдения. Помогай диагностировать IP-камеры и видеорегистраторы, RTSP, ONVIF, TCP/UDP порты, сеть и настройки PSVMS. Давай пошаговые и безопасные инструкции. Не выдумывай точные порты конкретной модели, если модель неизвестна; проси уточнение. Предупреждай, что открытие портов в интернет повышает риск и предпочтительнее VPN. Отвечай по-русски, если пользователь пишет по-русски.")}});
        m_chat->append(QStringLiteral("<b>PSVms AI</b><br>%1").arg(T("Здравствуйте! Помогу с подключением камер, RTSP/ONVIF, портами и настройкой программы.")));
    }
private:
    void sendMessage()
    {
        const QString prompt = m_input->text().trimmed();
        if (prompt.isEmpty() || m_busy) return;
        m_input->clear();
        m_chat->append(QStringLiteral("<p style='color:#d8bd82'><b>%1</b><br>%2</p>")
                       .arg(T("Вы"), prompt.toHtmlEscaped()));
        m_history.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("text"), prompt}});
        QJsonArray messages;
        for (const auto &v : m_history) messages.append(v);
        QJsonObject body;
        body.insert(QStringLiteral("modelUri"), QStringLiteral("gpt://%1/yandexgpt-5.1").arg(m_folderId));
        body.insert(QStringLiteral("completionOptions"), QJsonObject{{QStringLiteral("stream"), false},
            {QStringLiteral("temperature"), 0.25}, {QStringLiteral("maxTokens"), QStringLiteral("1800")}});
        body.insert(QStringLiteral("messages"), messages);
        QNetworkRequest req(QUrl(QStringLiteral("https://ai.api.cloud.yandex.net/foundationModels/v1/completion")));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setRawHeader("Authorization", QByteArray("Api-Key ") + m_apiKey.toUtf8());
        req.setRawHeader("x-folder-id", m_folderId.toUtf8());
        m_busy = true;
        m_input->setEnabled(false);
        auto *reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            m_busy = false;
            m_input->setEnabled(true);
            const QByteArray raw = reply->readAll();
            if (reply->error() != QNetworkReply::NoError) {
                m_chat->append(QStringLiteral("<p style='color:#ef7777'><b>%1</b><br>%2</p>")
                    .arg(T("Ошибка Yandex AI"), QString::fromUtf8(raw).toHtmlEscaped().left(1200)));
                reply->deleteLater();
                return;
            }
            const QJsonObject root = QJsonDocument::fromJson(raw).object();
            const QJsonArray alternatives = root.value(QStringLiteral("alternatives")).toArray();
            QString answer;
            if (!alternatives.isEmpty())
                answer = alternatives.first().toObject().value(QStringLiteral("message")).toObject()
                    .value(QStringLiteral("text")).toString();
            if (answer.isEmpty()) answer = T("Сервис вернул пустой ответ. Проверьте модель, каталог и права API-ключа.");
            m_chat->append(QStringLiteral("<p><b style='color:#8ec6a0'>Yandex AI</b><br>%1</p>")
                           .arg(answer.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
            m_history.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("text"), answer}});
            // Ограничиваем историю, чтобы запросы оставались компактными.
            while (m_history.size() > 17) m_history.removeAt(1);
            reply->deleteLater();
        });
    }
    QString m_apiKey, m_folderId;
    QNetworkAccessManager *m_nam = nullptr;
    QTextEdit *m_chat = nullptr;
    QLineEdit *m_input = nullptr;
    QJsonArray m_history;
    bool m_busy = false;
};


namespace {
void openYandexAssistant(QWidget *parent)
{
    QSettings settings;
    if (!settings.value(QStringLiteral("ai/yandexEnabled"), false).toBool()) {
        QMessageBox::information(parent, T("ИИ-помощник PSVMS"),
            T("Сначала включите ИИ-помощника и укажите API-ключ и Folder ID в Настройки → ИИ-помощник."));
        return;
    }
    const QString apiKey = SecureStore::decrypt(settings.value(QStringLiteral("ai/yandexApiKey")).toString()).trimmed();
    const QString folderId = settings.value(QStringLiteral("ai/yandexFolderId")).toString().trimmed();
    if (apiKey.isEmpty() || folderId.isEmpty()) {
        QMessageBox::information(parent, T("Настройка ИИ-помощника"),
            T("Для подключения укажите API-ключ Yandex AI Studio и Folder ID в настройках программы."));
        return;
    }
    YandexAssistantDialog dialog(apiKey, folderId, parent);
    const QString initialPrompt = parent ? parent->property("psvms.aiPrompt").toString() : QString();
    if (parent) parent->setProperty("psvms.aiPrompt", QString());
    if (!initialPrompt.isEmpty()) {
        if (auto *input = dialog.findChild<QLineEdit *>()) {
            input->setText(initialPrompt);
            QTimer::singleShot(0, &dialog, [&dialog] {
                const auto buttons = dialog.findChildren<QPushButton *>();
                for (QPushButton *button : buttons) {
                    if (button->text() == T("Отправить")) {
                        button->click();
                        break;
                    }
                }
            });
        }
    }
    dialog.exec();
}
}

SettingsPage::SettingsPage(CameraManager *mgr, QWidget *parent) : QWidget(parent), m_mgr(mgr)
{
    auto *tabs = new QTabWidget(this);
    // Горизонтальная навигация по вкладкам настроек: если все разделы не
    // помещаются по ширине, Qt показывает стрелки прокрутки слева/справа.
    // Пользователь может пролистывать вкладки стрелками, не теряя доступ
    // к разделам и не меняя их порядок случайным перетаскиванием.
    if (QTabBar *bar = tabs->tabBar()) {
        bar->setUsesScrollButtons(true);
        bar->setElideMode(Qt::ElideNone);
        bar->setExpanding(false);
        bar->setMovable(false);
        bar->setDrawBase(true);
        bar->setMinimumHeight(48);
        bar->setIconSize(QSize(18, 18));
        bar->setStyleSheet(QStringLiteral(
            "QTabBar::tab { min-height:38px; padding:8px 16px; margin:3px 4px 0 0; "
            "background:#181D27; color:#C8D0DC; border:1px solid #2A3242; "
            "border-radius:6px; font-size:10pt; font-weight:600; }"
            "QTabBar::tab:hover { background:#222A38; color:#F5B700; border:1px solid #455169; }"
            "QTabBar::tab:selected { background:#232A38; color:#F5B700; "
            "border:1px solid #F5B700; border-bottom:2px solid #F5B700; }"
            "QTabBar QToolButton { background:#181D27; color:#D5DCE6; "
            "border:1px solid #2A3242; border-radius:6px; min-width:36px; "
            "max-width:36px; min-height:36px; max-height:36px; margin:3px; padding:2px; }"
            "QTabBar QToolButton:hover { background:#222A38; color:#F5B700; border:1px solid #F5B700; }"
            "QTabBar QToolButton:pressed { background:#F5B700; color:#111317; }"));
    }
    tabs->addTab(buildGeneralTab(), T("Общие"));
    tabs->addTab(buildVideoTab(), T("Видео и папки"));
    tabs->addTab(buildPerformanceTab(), T("Производительность"));
    tabs->addTab(buildUsersTab(), T("Пользователи"));
    tabs->addTab(buildBackupTab(), T("Резервная копия"));
    tabs->addTab(buildLicenseTab(), T("Лицензия"));
    tabs->addTab(buildAboutTab(), T("О программе"));
    tabs->addTab(buildNetworkTab(), T("Сеть и порты"));
    tabs->addTab(buildAiTab(), T("ИИ-помощник"));
    for (int i = 1; i <= 4; ++i)
        tabs->setTabEnabled(i, isProVersion());

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 12, 16, 12);
    lay->addWidget(tabs);
}

// ---------------------------------------------------------------- общие

QWidget *SettingsPage::buildGeneralTab()
{
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft);
    QSettings s;

    auto *lang = new QComboBox;
    lang->addItem(QString::fromUtf8("Русский"), QStringLiteral("ru"));
    lang->addItem(QStringLiteral("English"), QStringLiteral("en"));
    lang->setCurrentIndex(lang->findData(I18n::code()));
    connect(lang, &QComboBox::activated, this, [this, lang](int i) {
        const QString code = lang->itemData(i).toString();
        if (code != I18n::code())
            emit languageChosen(code);
    });
    form->addRow(T("Язык интерфейса"), lang);

    form->addRow(QString(), settingsCheck(this, T("Запускать камеры автоматически при запуске программы"),
                                          QStringLiteral("general/autostart"), true));
    form->addRow(QString(), settingsCheck(this, T("Открывать программу на весь экран"),
                                          QStringLiteral("ui/startFullscreen"), false));
    auto *ptzEnabled = settingsCheck(this, T("Разрешить управление PTZ (поворот и зум камер)"),
                                     QStringLiteral("camera/ptzEnabled"), true);
    form->addRow(QString(), ptzEnabled);
    connect(ptzEnabled, &QCheckBox::toggled, this, [this](bool) {
        if (auto *ptz = window()->findChild<PtzPanel *>()) ptz->refreshControls();
    });
    form->addRow(QString(), settingsCheck(this, T("Показывать счётчики «В сети» и «Не в сети» и кнопку «Состояние камер»"),
                                          QStringLiteral("ui/showCameraStatus"), true));

    // размер окна, в том числе для широкоформатных (21:9) мониторов
    auto *size = new QComboBox;
    size->addItem(T("Авто"), 0);
    const char *labels[] = {"", "1280 × 720", "1600 × 900", "1920 × 1080 (16:9)", "2560 × 1080 (21:9)",
                            "2560 × 1440 (16:9)", "3440 × 1440 (21:9)", "3840 × 2160 (4K)"};
    for (int i = 1; i < int(sizeof(kSettingsWinSizes) / sizeof(kSettingsWinSizes[0])); ++i)
        size->addItem(QString::fromUtf8(labels[i]), i);
    size->setCurrentIndex(qBound(0, s.value(QStringLiteral("ui/windowSize"), 0).toInt(), size->count() - 1));
    connect(size, &QComboBox::activated, this, [this, size](int i) {
        QSettings().setValue(QStringLiteral("ui/windowSize"), i);
        QWidget *w = window();
        if (i > 0 && !w->isFullScreen() && !w->isMaximized())
            w->resize(kSettingsWinSizes[i].w, kSettingsWinSizes[i].h);
    });
    form->addRow(T("Размер окна"), size);

    auto *scale = new QComboBox;
    scale->addItem(T("Авто"), 0.0);
    for (double v : {1.0, 1.1, 1.25, 1.5, 1.75, 2.0})
        scale->addItem(QStringLiteral("%1%").arg(int(v * 100)), v);
    const int sc = scale->findData(s.value(QStringLiteral("ui/scale"), 0.0).toDouble());
    scale->setCurrentIndex(sc >= 0 ? sc : 0);
    // Значения масштабирования редактируются в форме и записываются только
    // после нажатия кнопки «Применить изменения».
    form->addRow(T("Масштаб интерфейса (пресет, после перезапуска)"), scale);

    // Ручная настройка размеров интерфейса, текста и интерактивных элементов.
    auto makePercentSpin = [this](const QString &key, int fallback) {
        auto *spin = new QSpinBox(this);
        spin->setRange(75, 200);
        spin->setSingleStep(5);
        spin->setSuffix(QStringLiteral("%"));
        spin->setValue(qBound(75, QSettings().value(key, fallback).toInt(), 200));
        Q_UNUSED(key);
        return spin;
    };
    auto *manualInterface = makePercentSpin(QStringLiteral("ui/manualInterfacePercent"), 100);
    auto *manualText = makePercentSpin(QStringLiteral("ui/manualTextPercent"), 100);
    auto *manualElements = makePercentSpin(QStringLiteral("ui/manualElementsPercent"), 100);
    form->addRow(T("Ручной размер интерфейса (после перезапуска)"), manualInterface);
    form->addRow(T("Размер текста (после перезапуска)"), manualText);
    form->addRow(T("Размер кнопок и элементов (после перезапуска)"), manualElements);

    auto *resetScaling = new QPushButton(T("Вернуть масштабирование к стандартным настройкам"), this);
    resetScaling->setStyleSheet(QStringLiteral("QPushButton{background:#1F242D;color:#ECF0F5;border:1px solid #343C4A;border-radius:5px;padding:6px 12px;font-weight:600;}QPushButton:hover{background:#2A313D;border-color:#F5B700;color:#FFD54F;}"));
    resetScaling->setToolTip(T("Сбросить пресет, ручной масштаб, размер текста и элементов до 100%"));
    connect(resetScaling, &QPushButton::clicked, this,
            [scale, manualInterface, manualText, manualElements]() {
        // Сброс пока меняет только значения в форме. Для записи нажмите «Применить изменения».
        scale->setCurrentIndex(0);
        manualInterface->setValue(100);
        manualText->setValue(100);
        manualElements->setValue(100);
    });
    form->addRow(QString(), resetScaling);

    auto *applyScaling = new QPushButton(T("Применить изменения"), this);
    applyScaling->setStyleSheet(QStringLiteral("QPushButton{background:#252C38;color:#F5B700;border:1px solid #F5B700;border-radius:5px;padding:6px 14px;font-weight:700;}QPushButton:hover{background:#F5B700;color:#111317;}"));
    applyScaling->setToolTip(T("Сохранить выбранные параметры масштабирования"));
    connect(applyScaling, &QPushButton::clicked, this,
            [scale, manualInterface, manualText, manualElements]() {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/scale"), scale->currentData().toDouble());
        settings.setValue(QStringLiteral("ui/manualInterfacePercent"), manualInterface->value());
        settings.setValue(QStringLiteral("ui/manualTextPercent"), manualText->value());
        settings.setValue(QStringLiteral("ui/manualElementsPercent"), manualElements->value());
        settings.sync();
        QMessageBox::information(nullptr, T("Масштабирование"),
                                 T("Изменения сохранены. Новое масштабирование применится после перезапуска программы."));
    });
    form->addRow(QString(), applyScaling);

    form->addRow(QString(), settingsCheck(this, T("Показывать дату и время на камерах"), QStringLiteral("view/timestamp"), true));
    auto *fmt = new QComboBox;
    const QStringList formats = {QStringLiteral("dd.MM.yyyy HH:mm:ss"), QStringLiteral("yyyy-MM-dd HH:mm:ss"),
                                 QStringLiteral("HH:mm:ss  dd.MM.yyyy"), QStringLiteral("dd/MM/yyyy hh:mm:ss AP")};
    fmt->addItems(formats);
    const int fi = formats.indexOf(s.value(QStringLiteral("view/timeFormat"), formats.first()).toString());
    fmt->setCurrentIndex(fi >= 0 ? fi : 0);
    connect(fmt, &QComboBox::activated, this, [fmt](int i) {
        QSettings().setValue(QStringLiteral("view/timeFormat"), fmt->itemText(i));
        Cfg::reload();
    });
    form->addRow(T("Формат даты и времени"), fmt);

    auto *hotkeys = new QLabel(
        T("Горячие клавиши: F11 — во весь экран, F5 — обновить окна с камерами, "
          "1 / 2 / 3 / 4 — раскладка 1, 2×2, 3×3, 4×4, Delete — закрыть выбранную камеру "
          "(в разделе «Мониторинг»)."), this);
    hotkeys->setWordWrap(true);
    hotkeys->setStyleSheet(QStringLiteral("color:#8a8a8e; margin-top: 10px;"));

    auto *lay = new QVBoxLayout;
    lay->addLayout(form);
    lay->addWidget(hotkeys);
    lay->addStretch(1);
    return settingsPane(lay);
}

// ---------------------------------------------------------------- видео и папки

QWidget *SettingsPage::buildVideoTab()
{
    auto *form = new QFormLayout;

    auto *videoEdit = new QLineEdit(m_mgr->storageRoot());
    form->addRow(T("Папка для записей видео"), settingsFolderRow(this, videoEdit, [this](const QString &dir) {
        m_mgr->setStorageRoot(dir);         // запущенные рекордеры перезапускаются в новую папку
        emit storageChanged();
    }));

    auto *minFree = new QSpinBox(this);
    minFree->setRange(256, 20480);
    minFree->setSingleStep(256);
    minFree->setSuffix(QStringLiteral(" МБ"));
    minFree->setValue(qBound(256, QSettings().value(QStringLiteral("storage/minFreeMB"), 1024).toInt(), 20480));
    minFree->setToolTip(T("Когда свободного места станет меньше этого значения, новые сегменты записи не будут создаваться. Старые записи автоматически не удаляются."));
    connect(minFree, qOverload<int>(&QSpinBox::valueChanged), this, [](int value) {
        QSettings().setValue(QStringLiteral("storage/minFreeMB"), value);
    });
    form->addRow(T("Минимум свободного места для записи"), minFree);

    auto *scheduleEnabled = new QCheckBox(T("Включить недельное расписание записи"), this);
    scheduleEnabled->setChecked(QSettings().value(QStringLiteral("recordSchedule/enabled"), false).toBool());
    auto *scheduleFrom = new QTimeEdit(QSettings().value(QStringLiteral("recordSchedule/from"), QTime(0, 0)).toTime(), this);
    auto *scheduleTo = new QTimeEdit(QSettings().value(QStringLiteral("recordSchedule/to"), QTime(23, 59)).toTime(), this);
    scheduleFrom->setDisplayFormat(QStringLiteral("HH:mm"));
    scheduleTo->setDisplayFormat(QStringLiteral("HH:mm"));

    auto *daysWidget = new QWidget(this);
    auto *daysLayout = new QHBoxLayout(daysWidget);
    daysLayout->setContentsMargins(0, 0, 0, 0);
    daysLayout->setSpacing(8);
    const QStringList dayNames = {T("Пн"), T("Вт"), T("Ср"), T("Чт"), T("Пт"), T("Сб"), T("Вс")};
    for (int day = 1; day <= 7; ++day) {
        auto *check = new QCheckBox(dayNames.at(day - 1), daysWidget);
        check->setChecked(QSettings().value(QStringLiteral("recordSchedule/day%1").arg(day), true).toBool());
        daysLayout->addWidget(check);
        connect(check, &QCheckBox::toggled, this, [day](bool on) {
            QSettings().setValue(QStringLiteral("recordSchedule/day%1").arg(day), on);
        });
    }
    daysLayout->addStretch(1);

    const auto setScheduleControlsEnabled = [scheduleFrom, scheduleTo, daysWidget](bool on) {
        scheduleFrom->setEnabled(on);
        scheduleTo->setEnabled(on);
        daysWidget->setEnabled(on);
    };
    setScheduleControlsEnabled(scheduleEnabled->isChecked());
    connect(scheduleEnabled, &QCheckBox::toggled, this, [setScheduleControlsEnabled](bool on) {
        setScheduleControlsEnabled(on);
        QSettings().setValue(QStringLiteral("recordSchedule/enabled"), on);
    });
    connect(scheduleFrom, &QTimeEdit::timeChanged, this, [](const QTime &t) {
        QSettings().setValue(QStringLiteral("recordSchedule/from"), t);
    });
    connect(scheduleTo, &QTimeEdit::timeChanged, this, [](const QTime &t) {
        QSettings().setValue(QStringLiteral("recordSchedule/to"), t);
    });
    form->addRow(T("Расписание"), scheduleEnabled);
    form->addRow(T("Дни записи"), daysWidget);
    form->addRow(T("Начало записи"), scheduleFrom);
    form->addRow(T("Окончание записи"), scheduleTo);

    auto *shotEdit = new QLineEdit(Cfg::screenshotDir());
    form->addRow(T("Папка для скриншотов"), settingsFolderRow(this, shotEdit, [](const QString &dir) {
        QSettings().setValue(QStringLiteral("storage/screenshots"), dir);
    }));

    form->addRow(QString(), settingsCheck(this, T("Показывать разрешение и качество связи поверх камеры"),
                                          QStringLiteral("view/showInfo"), true));
    form->addRow(QString(), settingsCheck(this, T("Показывать FPS в ячейках камер"),
                                          QStringLiteral("view/showFps"), true));

    auto *note = new QLabel(T("Записи ведутся сегментами по 5 минут. При смене папки новые записи пишутся в неё, "
                              "старые остаются на прежнем месте. Скриншот делается из меню камеры (правая кнопка мыши)."), this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#8a8a8e;"));

    auto *lay = new QVBoxLayout;
    lay->addLayout(form);
    lay->addWidget(note);
    lay->addStretch(1);
    return settingsPane(lay);
}

// ---------------------------------------------------------------- производительность

QWidget *SettingsPage::buildPerformanceTab()
{
    auto *form = new QFormLayout;
    form->addRow(QString(), settingsCheck(this, T("Режим для слабого компьютера"), QStringLiteral("perf/lowPower"), false));

    auto *fps = new QSpinBox;
    fps->setRange(1, 30);
    fps->setValue(QSettings().value(QStringLiteral("perf/maxFps"), 30).toInt());
    connect(fps, &QSpinBox::valueChanged, this, [](int v) {
        QSettings().setValue(QStringLiteral("perf/maxFps"), v);
        Cfg::reload();
    });
    form->addRow(T("Предел кадров в секунду на ячейку"), fps);

    form->addRow(QString(), settingsCheck(this, T("Сглаживание картинки"), QStringLiteral("perf/smooth"), true));
    form->addRow(QString(), settingsCheck(this, T("Анимация интерфейса (заставка, Demo, значки и переходы)"),
                                          QStringLiteral("ui/animations"), true));
    form->addRow(QString(), settingsCheck(this, T("Свечение рамок камер "),
                                          QStringLiteral("ui/cameraGlow"), true));

    auto *note = new QLabel(T("Режим для слабого компьютера: не более 8 кадров в секунду, без сглаживания и анимаций, "
                              "реже проверяется связь с камерами. Ещё сильнее нагрузку снижает показ подпотока "
                              "(у камеры: «Качество картинки → Низкое») и меньшее число ячеек в раскладке."), this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#8a8a8e;"));

    auto *lay = new QVBoxLayout;
    lay->addLayout(form);
    lay->addWidget(note);
    lay->addStretch(1);
    return settingsPane(lay);
}

// ---------------------------------------------------------------- диагностика сети и портов

QWidget *SettingsPage::buildNetworkTab()
{
    auto *host = new QLineEdit(this);
    host->setPlaceholderText(QStringLiteral("192.168.1.64"));
    auto *port = new QSpinBox(this);
    port->setRange(1, 65535);
    port->setValue(554);
    auto *protocol = new QComboBox(this);
    protocol->addItem(QStringLiteral("TCP"), QStringLiteral("tcp"));
    protocol->addItem(T("UDP (проверка отправки)"), QStringLiteral("udp"));
    auto *vendor = new QComboBox(this);
    vendor->addItems({T("Универсальные / ONVIF"), T("Hikvision / HiWatch"),
                      T("Dahua"), T("Uniview"), T("Другой производитель")});
    auto *model = new QLineEdit(this);
    model->setPlaceholderText(T("Модель камеры или регистратора (необязательно)"));
    auto *service = new QComboBox(this);
    service->addItem(T("RTSP — видеопоток"), 554);
    service->addItem(T("HTTP — веб-интерфейс"), 80);
    service->addItem(T("HTTPS — защищённый веб-интерфейс"), 443);
    service->addItem(T("ONVIF — обнаружение/управление (часто HTTP)"), 80);
    service->addItem(T("Hikvision — SDK / Server"), 8000);
    service->addItem(T("Dahua — TCP Service"), 37777);
    service->addItem(T("Пользовательский порт"), 0);
    auto *result = new QLabel(T("Укажите IP-адрес и порт, затем нажмите «Проверить порт»."), this);
    result->setWordWrap(true);
    result->setMinimumHeight(54);
    result->setStyleSheet(QStringLiteral("QLabel{padding:12px;border-radius:10px;background:#25272d;color:#d8bd82;}"));
    auto *advice = new QLabel(this);
    advice->setWordWrap(true);
    advice->setText(T("Подсказки по типовым портам:\n• RTSP: 554/TCP — видеопоток большинства IP-камер.\n• HTTP: 80/TCP; HTTPS: 443/TCP — веб-интерфейс.\n• Hikvision/HiWatch: 8000/TCP — SDK/Server, 554/TCP — RTSP.\n• Dahua: 37777/TCP — сервисный порт, 554/TCP — RTSP.\n• ONVIF часто использует HTTP-порт устройства (обычно 80 или заданный вручную).\nПорты могут быть изменены производителем, установщиком или администратором. Сверяйтесь с сетевыми настройками конкретной модели."));
    advice->setStyleSheet(QStringLiteral("color:#a7a7ad;"));
    auto *test = new QPushButton(T("Проверить порт"), this);
    auto *fill = new QPushButton(T("Подставить порт службы"), this);
    auto *form = new QFormLayout;
    form->addRow(T("IP-адрес / имя узла"), host);
    form->addRow(T("Производитель"), vendor);
    form->addRow(T("Модель"), model);
    form->addRow(T("Тип службы"), service);
    form->addRow(T("Номер порта"), port);
    form->addRow(T("Протокол"), protocol);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(fill);
    buttons->addWidget(test);
    auto *lay = new QVBoxLayout;
    lay->addLayout(form);
    lay->addLayout(buttons);
    lay->addWidget(result);
    lay->addWidget(advice);
    lay->addStretch(1);
    connect(service, qOverload<int>(&QComboBox::currentIndexChanged), this, [service, port](int) {
        const int p = service->currentData().toInt();
        if (p > 0) port->setValue(p);
    });
    connect(fill, &QPushButton::clicked, this, [service, port] {
        const int p = service->currentData().toInt();
        if (p > 0) port->setValue(p);
    });
    connect(vendor, qOverload<int>(&QComboBox::currentIndexChanged), this, [vendor, advice] {
        const QString v = vendor->currentText();
        if (v.contains(QStringLiteral("Hikvision")))
            advice->setText(T("Hikvision / HiWatch: обычно HTTP 80/TCP, HTTPS 443/TCP, RTSP 554/TCP, SDK/Server 8000/TCP. В некоторых моделях есть дополнительные порты 8443, 7681 и 7682. Проверьте меню Configuration → Network → Basic Settings → Port и документацию именно вашей модели."));
        else if (v.contains(QStringLiteral("Dahua")))
            advice->setText(T("Dahua: часто HTTP 80/TCP, RTSP 554/TCP, TCP Service 37777/TCP. Порты зависят от модели и прошивки; уточните их в Network → Connection → Port или в руководстве устройства."));
        else if (v.contains(QStringLiteral("Uniview")))
            advice->setText(T("Uniview: обычно используются HTTP/HTTPS для веб-доступа и RTSP 554/TCP для потока; порт управления зависит от модели и версии прошивки. Проверьте Network → Port в веб-интерфейсе."));
        else
            advice->setText(T("Типовые значения: RTSP 554/TCP, HTTP 80/TCP, HTTPS 443/TCP, ONVIF — часто HTTP-порт устройства. Порты управления зависят от производителя и модели; не применяйте значения Hikvision/Dahua к другому устройству без проверки документации."));
    });
    connect(test, &QPushButton::clicked, this, [this, host, port, protocol, result, test] {
        const QString address = host->text().trimmed();
        const int number = port->value();
        if (address.isEmpty()) { result->setText(T("Введите IP-адрес или имя узла.")); return; }
        test->setEnabled(false);
        if (protocol->currentData().toString() == QStringLiteral("udp")) {
            auto *sock = new QUdpSocket(this);
            const QHostAddress ip(address);
            if (!ip.isNull()) {
                const QByteArray probe("PSVMS-port-check");
                const qint64 sent = sock->writeDatagram(probe, ip, quint16(number));
                result->setText(sent == probe.size()
                    ? T("UDP-пакет отправлен. Это НЕ подтверждает, что порт открыт: UDP не устанавливает соединение, а ответ зависит от службы устройства.")
                    : T("Не удалось отправить UDP-пакет. Проверьте IP, маршрут и настройки сети."));
            } else {
                result->setText(T("Для UDP-проверки укажите IP-адрес в числовом формате."));
            }
            sock->deleteLater();
            test->setEnabled(true);
            return;
        }
        auto *socket = new QTcpSocket(this);
        auto done = std::make_shared<bool>(false);
        auto finish = [socket, test, result, address, number, done](bool open, const QString &detail) {
            if (*done) return;
            *done = true;
            result->setText(QStringLiteral("%1:%2 — %3\n%4").arg(address).arg(number)
                .arg(open ? T("ПОРТ ДОСТУПЕН") : T("ПОРТ НЕДОСТУПЕН"), detail));
            result->setStyleSheet(open
                ? QStringLiteral("QLabel{padding:12px;border-radius:10px;background:#203b31;color:#9fe0b6;}")
                : QStringLiteral("QLabel{padding:12px;border-radius:10px;background:#43282a;color:#ffaaaa;}") );
            test->setEnabled(true);
            socket->deleteLater();
        };
        connect(socket, &QTcpSocket::connected, this, [finish] { finish(true, T("TCP-соединение установлено. Служба принимает подключения на этом порту.")); });
        connect(socket, &QTcpSocket::errorOccurred, this, [finish, socket](QAbstractSocket::SocketError) {
            finish(false, socket->errorString() + T(". Возможные причины: служба выключена, порт изменён, устройство недоступно или соединение блокирует firewall/маршрутизатор."));
        });
        QTimer::singleShot(4500, socket, [finish, socket] {
            if (socket->state() != QAbstractSocket::ConnectedState)
                finish(false, T("Истекло время ожидания. Устройство не ответило на TCP-подключение за 4,5 секунды."));
        });
        socket->connectToHost(address, quint16(number));
    });
    return settingsPane(lay);
}

// ---------------------------------------------------------------- ИИ-помощник Yandex

QWidget *SettingsPage::buildAiTab()
{
    QSettings settings;
    auto *enabled = new QCheckBox(T("Включить ИИ-помощника Yandex в программе"), this);
    enabled->setChecked(settings.value(QStringLiteral("ai/yandexEnabled"), false).toBool());
    auto *showHomePanel = new QCheckBox(T("Показывать панель ИИ-помощника на главной странице"), this);
    showHomePanel->setChecked(settings.value(QStringLiteral("ai/showHomePanel"), true).toBool());
    showHomePanel->setToolTip(T("Можно скрыть правую панель, не отключая сам ИИ-чат."));
    auto *key = new QLineEdit(this);
    key->setEchoMode(QLineEdit::Password);
    key->setPlaceholderText(T("Секретный API-ключ Yandex AI Studio"));
    key->setText(SecureStore::decrypt(settings.value(QStringLiteral("ai/yandexApiKey")).toString()));
    auto *folder = new QLineEdit(this);
    folder->setPlaceholderText(T("Идентификатор каталога Yandex Cloud (Folder ID)"));
    folder->setText(settings.value(QStringLiteral("ai/yandexFolderId")).toString());
    auto *save = new QPushButton(T("Сохранить параметры"), this);
    auto *open = new QPushButton(T("Открыть ИИ-чат"), this);
    auto *status = new QLabel(this);
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color:#a7a7ad;"));
    auto *note = new QLabel(T("Для работы нужен API-ключ сервисного аккаунта Yandex AI Studio с доступом к генерации текста и Folder ID. Используется модель YandexGPT Pro 5.1. Запросы отправляются в облако Yandex; тарификация зависит от вашего аккаунта и условий Yandex Cloud. При выключении переключателя чат не открывается и запросы не отправляются."), this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#a7a7ad;"));
    auto *form = new QFormLayout;
    form->addRow(T("API-ключ"), key);
    form->addRow(T("Folder ID"), folder);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(save);
    buttons->addWidget(open);
    auto *lay = new QVBoxLayout;
    lay->addWidget(enabled);
    lay->addWidget(showHomePanel);
    lay->addLayout(form);
    lay->addLayout(buttons);
    lay->addWidget(status);
    lay->addWidget(note);
    lay->addStretch(1);
    auto saveSettings = [this, enabled, showHomePanel, key, folder, status] {
        QSettings s;
        s.setValue(QStringLiteral("ai/yandexEnabled"), enabled->isChecked());
        s.setValue(QStringLiteral("ai/showHomePanel"), showHomePanel->isChecked());
        s.setValue(QStringLiteral("ai/yandexApiKey"), SecureStore::encrypt(key->text().trimmed()));
        s.setValue(QStringLiteral("ai/yandexFolderId"), folder->text().trimmed());
        s.sync();
        if (QWidget *panel = window()->findChild<QWidget *>(QStringLiteral("aiHomePanel"))) {
            const bool onHome = window()->findChild<QStackedWidget *>() &&
                window()->findChild<QStackedWidget *>()->currentIndex() == 0;
            panel->setVisible(showHomePanel->isChecked() && onHome);
        }
        status->setText(T("Параметры сохранены."));
    };
    connect(save, &QPushButton::clicked, this, saveSettings);
    connect(showHomePanel, &QCheckBox::toggled, this, [saveSettings](bool) { saveSettings(); });
    connect(enabled, &QCheckBox::toggled, this, [saveSettings, open](bool on) {
        saveSettings();
        open->setEnabled(on);
    });
    open->setEnabled(enabled->isChecked());
    connect(open, &QPushButton::clicked, this, [this, enabled, key, folder, status, saveSettings] {
        saveSettings();
        if (!enabled->isChecked()) { status->setText(T("ИИ-помощник отключён в настройках.")); return; }
        if (key->text().trimmed().isEmpty() || folder->text().trimmed().isEmpty()) {
            status->setText(T("Сначала укажите API-ключ и Folder ID."));
            return;
        }
        YandexAssistantDialog dlg(key->text().trimmed(), folder->text().trimmed(), this);
        dlg.exec();
    });
    return settingsPane(lay);
}

// ---------------------------------------------------------------- пользователи

QString SettingsPage::currentUserId() const
{
    const int row = m_users->currentRow();
    return row < 0 ? QString() : m_users->item(row, 0)->data(Qt::UserRole).toString();
}

QWidget *SettingsPage::buildUsersTab()
{
    m_users = new QTableWidget(0, 4, this);
    m_users->setHorizontalHeaderLabels({T("Имя"), T("Роль"), T("Пароль"), T("Состояние")});
    m_users->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_users->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_users->setSelectionMode(QAbstractItemView::SingleSelection);
    m_users->verticalHeader()->setVisible(false);
    m_users->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_users->setColumnWidth(1, 180);
    m_users->setColumnWidth(2, 110);
    m_users->setColumnWidth(3, 120);

    auto *add = new QPushButton(T("Добавить пользователя…"), this);
    auto *pass = new QPushButton(T("Пароль…"), this);
    auto *role = new QPushButton(T("Роль…"), this);
    auto *toggle = new QPushButton(T("Отключить / включить"), this);
    auto *audit = new QPushButton(T("Журнал действий…"), this);
    auto *del = new QPushButton(T("Удалить"), this);
    // Кнопки пользователей размещены сеткой 3x2, чтобы подписи полностью помещались.
    for (QPushButton *button : {add, pass, role, toggle, audit, del}) {
        QFont buttonFont = button->font();
        buttonFont.setPointSize(10);
        button->setFont(buttonFont);
        button->setMinimumSize(0, 36);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setStyleSheet(QStringLiteral("QPushButton { background:#1F242D; color:#ECF0F5; border:1px solid #343C4A; border-radius:5px; padding: 6px 10px; font-weight:600; } QPushButton:hover { background:#2A313D; border-color:#F5B700; color:#FFD54F; } QPushButton:pressed { background:#F5B700; color:#111317; } QPushButton:disabled { background:#151921; color:#697282; border-color:#232936; }"));
    }
    auto *buttons = new QGridLayout;
    buttons->setHorizontalSpacing(8);
    buttons->setVerticalSpacing(7);
    buttons->addWidget(add, 0, 0);
    buttons->addWidget(pass, 0, 1);
    buttons->addWidget(role, 0, 2);
    buttons->addWidget(toggle, 1, 0);
    buttons->addWidget(audit, 1, 1);
    buttons->addWidget(del, 1, 2);
    for (int col = 0; col < 3; ++col) buttons->setColumnStretch(col, 1);

    auto *note = new QLabel(T("Гость может только просматривать камеры. Новые пользователи создаются без пароля; Administrator может установить пароль при первом входе. "
                              "Разработчик всегда защищён обязательным паролем. Отключённые пользователи не могут войти. Журнал фиксирует входы и действия."), this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#8a8a8e;"));

    auto *idleBox = new QGroupBox(T("Автоматическая блокировка"));
    auto *idleEnabled = new QCheckBox(T("Блокировать PSVMS при бездействии"), idleBox);
    auto *idleMinutes = new QSpinBox(idleBox);
    idleMinutes->setRange(1, 240);
    idleMinutes->setSuffix(T(" мин."));
    idleMinutes->setValue(QSettings().value(QStringLiteral("security/idleLockMinutes"), 10).toInt());
    idleEnabled->setChecked(QSettings().value(QStringLiteral("security/idleLockEnabled"), false).toBool());
    idleMinutes->setEnabled(idleEnabled->isChecked());
    auto *idleRow = new QHBoxLayout(idleBox);
    idleRow->addWidget(idleEnabled);
    idleRow->addStretch(1);
    idleRow->addWidget(new QLabel(T("Время:"), idleBox));
    idleRow->addWidget(idleMinutes);
    connect(idleEnabled, &QCheckBox::toggled, idleMinutes, &QWidget::setEnabled);
    auto saveIdle = [idleEnabled, idleMinutes] {
        QSettings settings;
        settings.setValue(QStringLiteral("security/idleLockEnabled"), idleEnabled->isChecked());
        settings.setValue(QStringLiteral("security/idleLockMinutes"), idleMinutes->value());
        settings.sync();
        QCoreApplication::postEvent(qApp, new QEvent(QEvent::User));
        SecurityAudit::write(QStringLiteral("idle_lock_settings_changed"),
            QStringLiteral("enabled=%1 minutes=%2").arg(idleEnabled->isChecked() ? 1 : 0).arg(idleMinutes->value()));
    };
    connect(idleEnabled, &QCheckBox::toggled, this, [saveIdle](bool) { saveIdle(); });
    connect(idleMinutes, qOverload<int>(&QSpinBox::valueChanged), this, [saveIdle](int) { saveIdle(); });

    auto *switchRow = new QHBoxLayout;
    auto *switchLabel = new QLabel(T("Текущая учётная запись в сеансе: <b>%1</b>")
        .arg(Users::current().name.isEmpty() ? T("не выбран") : Users::current().name), this);
    switchLabel->setStyleSheet(QStringLiteral("color:#D5DCE6;"));
    auto *switchBtn = new QPushButton(T("Сменить пользователя…"), this);
    switchBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background:#252C38; color:#F5B700; border:1px solid #F5B700; border-radius:5px; padding:6px 12px; font-weight:700; }"
        "QPushButton:hover { background:#F5B700; color:#111317; }"
    ));
    connect(switchBtn, &QPushButton::clicked, this, [this, switchLabel] {
        if (QWidget *w = window()) {
            QMetaObject::invokeMethod(w, "switchUser", Qt::QueuedConnection);
        }
        switchLabel->setText(T("Текущая учётная запись в сеансе: <b>%1</b>")
            .arg(Users::current().name.isEmpty() ? T("не выбран") : Users::current().name));
    });
    switchRow->addWidget(switchLabel, 1);
    switchRow->addWidget(switchBtn);

    auto *lay = new QVBoxLayout;
    lay->addLayout(switchRow);
    lay->addWidget(idleBox);
    lay->addLayout(buttons);
    lay->addWidget(m_users, 1);
    lay->addWidget(note);

    if (!Users::current().isAdmin()) {           // управлять пользователями может только администратор
        add->setEnabled(false);
        pass->setEnabled(false);
        role->setEnabled(false);
        toggle->setEnabled(false);
        audit->setEnabled(false);
        del->setEnabled(false);
    }
    connect(add, &QPushButton::clicked, this, [this] {
        SettingsUserDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted)
            rebuildUsers();
    });
    connect(pass, &QPushButton::clicked, this, [this] {
        const UserInfo u = Users::find(currentUserId());
        if (!u.valid())
            return;
        SettingsPasswordDialog dlg(u.id, u.name, this);
        if (dlg.exec() == QDialog::Accepted)
            rebuildUsers();
    });
    connect(role, &QPushButton::clicked, this, [this] {
        const UserInfo u = Users::find(currentUserId());
        if (!u.valid())
            return;
        QMenu menu(this);
        QAction *asAdmin = menu.addAction(T("Администратор (все разделы)"));
        QAction *asOperator = menu.addAction(T("Оператор (только просмотр)"));
        asAdmin->setCheckable(true);
        asOperator->setCheckable(true);
        asAdmin->setChecked(u.isAdmin());
        asOperator->setChecked(!u.isAdmin());
        QAction *chosen = menu.exec(QCursor::pos());
        if (!chosen)
            return;
        const QString newRole = chosen == asAdmin ? QStringLiteral("admin") : QStringLiteral("operator");
        QString err;
        if (!Users::setRole(u.id, newRole, &err))
            QMessageBox::warning(this, T("Роль пользователя"), err);
        rebuildUsers();
    });
    auto updateToggleText = [this, toggle] {
        const UserInfo u = Users::find(currentUserId());
        toggle->setText(!u.valid() ? T("Включить / отключить")
                                  : u.enabled ? T("Временно отключить") : T("Включить пользователя"));
        toggle->setEnabled(Users::current().isAdmin() && u.valid()
                           && !u.isGuest() && !u.isDeveloper()
                           && u.id != Users::current().id);
    };
    connect(m_users, &QTableWidget::itemSelectionChanged, this, updateToggleText);
    connect(toggle, &QPushButton::clicked, this, [this, updateToggleText] {
        const UserInfo u = Users::find(currentUserId());
        if (!u.valid()) return;
        QString err;
        if (!Users::setEnabled(u.id, !u.enabled, &err))
            QMessageBox::warning(this, T("Состояние пользователя"), err);
        rebuildUsers();
        updateToggleText();
    });
    connect(audit, &QPushButton::clicked, this, [this] {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QFile file(dir + QStringLiteral("/security-audit.log"));
        QString content;
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            content = QString::fromUtf8(file.readAll());
        else
            content = T("Журнал пока пуст или недоступен.");
        QDialog dlg(this);
        dlg.setWindowTitle(T("Журнал входов и действий"));
        dlg.resize(900, 560);
        auto *view = new QPlainTextEdit(&dlg);
        view->setReadOnly(true);
        view->setPlainText(content);
        auto *close = new QPushButton(T("Закрыть"), &dlg);
        connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
        auto *layout = new QVBoxLayout(&dlg);
        layout->addWidget(view);
        layout->addWidget(close, 0, Qt::AlignRight);
        dlg.exec();
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const UserInfo u = Users::find(currentUserId());
        if (!u.valid())
            return;
        if (u.isGuest() || u.isDeveloper()) {
            QMessageBox::information(this, T("Удаление пользователя"),
                u.isGuest() ? T("Системную учётную запись «Гость» нельзя удалить.")
                            : T("Учётную запись «Разработчик» нельзя удалить."));
            return;
        }
        QMessageBox box(QMessageBox::Question, T("Удаление пользователя"),
                        T("Удалить пользователя «%1»?").arg(u.name), QMessageBox::NoButton, this);
        QPushButton *yes = box.addButton(T("Удалить"), QMessageBox::AcceptRole);
        box.addButton(T("Отмена"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() != yes)
            return;
        QString err;
        if (!Users::remove(u.id, &err))
            QMessageBox::warning(this, T("Удаление пользователя"), err);
        rebuildUsers();
    });

    rebuildUsers();
    updateToggleText();
    return settingsPane(lay);
}

void SettingsPage::rebuildUsers()
{
    m_users->setRowCount(0);
    for (const UserInfo &u : Users::all()) {
        const int row = m_users->rowCount();
        m_users->insertRow(row);
        auto *name = new QTableWidgetItem(u.name);
        name->setData(Qt::UserRole, u.id);
        m_users->setItem(row, 0, name);
        m_users->setItem(row, 1, new QTableWidgetItem(u.isDeveloper() ? T("Разработчик") :
            u.isGuest() ? T("Гость") : u.isAdmin() ? T("Администратор") : T("Оператор")));
        m_users->setItem(row, 2, new QTableWidgetItem(u.hasPassword ? T("задан") : T("без пароля")));
        m_users->setItem(row, 3, new QTableWidgetItem(u.enabled ? T("Активен") : T("Отключён")));
        if (!u.enabled) {
            for (int col = 0; col < m_users->columnCount(); ++col)
                m_users->item(row, col)->setForeground(QBrush(QColor("#999999")));
        }
    }
}

// ---------------------------------------------------------------- резервная копия

QWidget *SettingsPage::buildBackupTab()
{
    auto *info = new QLabel(T("В резервную копию входят камеры, настройки программы, шаблоны и расположение камер в окне. "
                              "Пользователи и пароли входа в копию не попадают.\n\n"
                              "Внимание: адреса камер содержат их пароли. Храните файл копии в безопасном месте."), this);
    info->setWordWrap(true);

    auto *exportBtn = new QPushButton(T("Создать резервную копию…"), this);
    exportBtn->setStyleSheet(QStringLiteral("QPushButton{background:#252C38;color:#F5B700;border:1px solid #F5B700;border-radius:5px;padding:6px 14px;font-weight:700;}QPushButton:hover{background:#F5B700;color:#111317;}"));
    auto *importBtn = new QPushButton(T("Восстановить из копии…"), this);
    importBtn->setStyleSheet(QStringLiteral("QPushButton{background:#1F242D;color:#ECF0F5;border:1px solid #343C4A;border-radius:5px;padding:6px 14px;font-weight:600;}QPushButton:hover{background:#2A313D;border-color:#F5B700;color:#FFD54F;}"));

    connect(exportBtn, &QPushButton::clicked, this, [this] {
        const QString def = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                            + QStringLiteral("/PSVMS-backup.json");
        const QString path = QFileDialog::getSaveFileName(this, T("Резервная копия"), def, QStringLiteral("JSON (*.json)"));
        if (path.isEmpty())
            return;
        QString err;
        if (Backup::exportTo(path, &err))
            QMessageBox::information(this, T("Резервная копия"), T("Резервная копия сохранена:\n%1").arg(path));
        else
            QMessageBox::warning(this, T("Резервная копия"), err);
    });
    connect(importBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, T("Восстановить из копии"), QString(), QStringLiteral("JSON (*.json)"));
        if (path.isEmpty())
            return;
        QMessageBox box(QMessageBox::Question, T("Восстановление"),
                        T("Текущие камеры и настройки будут заменены данными из копии, программа перезапустится. Продолжить?"),
                        QMessageBox::NoButton, this);
        QPushButton *yes = box.addButton(T("Восстановить"), QMessageBox::AcceptRole);
        box.addButton(T("Отмена"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() != yes)
            return;
        QString err;
        if (!Backup::importFrom(path, &err)) {
            QMessageBox::warning(this, T("Восстановление"), err);
            return;
        }
        QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
        QCoreApplication::quit();
    });

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(exportBtn);
    buttons->addWidget(importBtn);
    buttons->addStretch(1);

    auto *lay = new QVBoxLayout;
    lay->addWidget(info);
    lay->addLayout(buttons);
    lay->addStretch(1);
    return settingsPane(lay);
}

// ---------------------------------------------------------------- о программе

// ---------------------------------------------------------------- лицензия

QWidget *SettingsPage::buildLicenseTab()
{
    auto *editionTitle = new QLabel(this);
    editionTitle->setStyleSheet(QStringLiteral("font-size:10pt;font-weight:900;"));
    auto *editionStatus = new QLabel(this);
    editionStatus->setWordWrap(true);
    auto *machineLabel = new QLabel(this);
    machineLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    machineLabel->setWordWrap(true);

    auto refresh = [editionTitle, editionStatus, machineLabel] {
        QString status;
        QDateTime expiry;
        const bool developerAccess = Users::current().isDeveloper();
        const bool active = psvmsLicenseActive(&status, &expiry);
        if (developerAccess) {
            editionTitle->setText(T("PSVMS — Pro (режим разработчика)"));
            editionTitle->setStyleSheet(QStringLiteral("font-size:10pt;font-weight:900;color:#F5B700;"));
            editionStatus->setText(T("Полный функционал доступен для учётной записи разработчика. Коммерческая лицензия не изменена."));
        } else {
            editionTitle->setText(active ? T("PSVMS — полная Pro-версия") : T("PSVMS — DEMO ВЕРСИЯ"));
            editionTitle->setStyleSheet(active
                ? QStringLiteral("font-size:10pt;font-weight:900;color:#63e6ee;")
                : QStringLiteral("font-size:10pt;font-weight:900;color:#ff4548;letter-spacing:0.5px;"));
            editionStatus->setText(status + (active
                ? T("\nПолный доступ к функциям программы включён.")
                : T("\nДемо-режим: доступно не более 10 камер.")));
        }
        machineLabel->setText(T("ID этого компьютера для выпуска лицензии:\n%1")
                              .arg(psvmsMachineFingerprint()));
    };
    refresh();

    auto *copyMachineBtn = new QPushButton(T("Скопировать ID компьютера"), this);
    auto *activateBtn = new QPushButton(T("Активировать файл лицензии…"), this);
    auto *removeBtn = new QPushButton(T("Удалить лицензию"), this);
    connect(copyMachineBtn, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(psvmsMachineFingerprint());
        QMessageBox::information(this, T("ID компьютера"), T("ID компьютера скопирован. Отправьте его для выпуска лицензии."));
    });
    connect(activateBtn, &QPushButton::clicked, this, [this, refresh] {
        const QString path = QFileDialog::getOpenFileName(this, T("Выберите файл лицензии PSVMS"),
                                                          QString(), T("Лицензии PSVMS (*.lic);;Все файлы (*.*)"));
        if (path.isEmpty()) return;
        QString error;
        if (!psvmsImportLicense(path, &error)) {
            QMessageBox::warning(this, T("Лицензия не активирована"), error);
            return;
        }
        refresh();
        QMessageBox::information(this, T("Лицензия активирована"),
                                 T("Подпись проверена. Полная версия PSVMS активирована на этом компьютере."));
        if (auto *tabs = findChild<QTabWidget *>()) {
            for (int i = 1; i <= 4; ++i) tabs->setTabEnabled(i, true);
        }
    });
    connect(removeBtn, &QPushButton::clicked, this, [this, refresh] {
        if (QMessageBox::question(this, T("Удалить лицензию"),
                                  T("Удалить лицензию с этого компьютера? Программа вернётся в демо-режим.")) != QMessageBox::Yes)
            return;
        QSettings settings;
        settings.remove(QStringLiteral("license"));
        refresh();
        if (m_mgr) m_mgr->enforceDemoLimit();
        if (auto *tabs = findChild<QTabWidget *>()) {
            for (int i = 1; i <= 4; ++i) tabs->setTabEnabled(i, false);
        }
    });

    auto *info = new QLabel(T("Лицензия привязывается к ID компьютера. Передайте этот ID разработчику, "
                              "получите файл .lic и загрузите его здесь. Подпись проверяется встроенным "
                              "открытым ключом; срок лицензии нельзя изменить редактированием файла. "
                              "Для лицензии на срок в днях отсчёт начинается при активации."), this);
    info->setWordWrap(true);

    // Равные расширяемые кнопки лицензии с компактным шрифтом.
    for (QPushButton *button : {copyMachineBtn, activateBtn, removeBtn}) {
        QFont buttonFont = button->font();
        buttonFont.setPointSize(10);
        button->setFont(buttonFont);
        button->setMinimumSize(0, 38);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setStyleSheet(QStringLiteral("QPushButton { background:#1F242D; color:#ECF0F5; border:1px solid #343C4A; border-radius:5px; padding: 6px 12px; font-weight:600; } QPushButton:hover { background:#2A313D; border-color:#F5B700; color:#FFD54F; } QPushButton:pressed { background:#F5B700; color:#111317; } QPushButton:disabled { background:#151921; color:#697282; border-color:#232936; }"));
    }
    auto *buttons = new QGridLayout;
    buttons->setHorizontalSpacing(8);
    buttons->addWidget(copyMachineBtn, 0, 0);
    buttons->addWidget(activateBtn, 0, 1);
    buttons->addWidget(removeBtn, 0, 2);
    for (int col = 0; col < 3; ++col) buttons->setColumnStretch(col, 1);

    auto *lay = new QVBoxLayout;
    lay->addWidget(editionTitle);
    lay->addWidget(editionStatus);
    lay->addWidget(machineLabel);
    lay->addWidget(info);
    lay->addLayout(buttons);
    lay->addStretch(1);
    return settingsPane(lay);
}

QWidget *SettingsPage::buildAboutTab()
{
    auto *name = new QLabel(App::title(), this);
    name->setStyleSheet(QStringLiteral("font-size:10pt; font-weight: 700; color: #F5B700;"));
    auto *ver = new QLabel(T("Версия %1").arg(QString::fromLatin1(App::kVersion)), this);
    ver->setStyleSheet(QStringLiteral("color:#a8a8ac;"));
    auto *author = new QLabel(T("Разработчик: %1").arg(QString::fromUtf8(App::kAuthor)), this);
    author->setStyleSheet(QStringLiteral("color:#e6e6e6; margin-top: 18px;"));
    auto *lay = new QVBoxLayout;
    lay->addWidget(name);
    lay->addWidget(ver);
    lay->addWidget(author);
    lay->addStretch(1);
    return settingsPane(lay);
}

// ======================================================================
// app/side_nav.cpp
// ======================================================================
namespace {

constexpr int kNavExpandedWidth = 255;
constexpr int kNavCollapsedWidth = 68;

// Векторные значки ячеек: единая тонкая голубая неоновая графика PSVMS.
QIcon makeCellIcon(const QString &kind, const QColor &color)
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 4.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    if (kind == QStringLiteral("play")) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPolygonF tri;
        tri << QPointF(23, 15) << QPointF(49, 32) << QPointF(23, 49);
        p.drawPolygon(tri);
    } else if (kind == QStringLiteral("stop")) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(18, 18, 28, 28), 5, 5);
    } else if (kind == QStringLiteral("close")) {
        p.drawLine(21, 21, 43, 43);
        p.drawLine(43, 21, 21, 43);
    } else if (kind == QStringLiteral("plus")) {
        p.drawLine(32, 17, 32, 47);
        p.drawLine(17, 32, 47, 32);
    } else if (kind == QStringLiteral("mute") || kind == QStringLiteral("sound")) {
        QPolygonF body;
        body << QPointF(12,25) << QPointF(22,25) << QPointF(35,15)
             << QPointF(35,49) << QPointF(22,39) << QPointF(12,39);
        p.setBrush(color);
        p.drawPolygon(body);
        if (kind == QStringLiteral("sound")) {
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(29,18,23,28), -55*16, 110*16);
            p.drawArc(QRectF(29,10,32,44), -55*16, 110*16);
        } else {
            p.drawLine(42, 22, 55, 42);
            p.drawLine(55, 22, 42, 42);
        }
    }
    // Явно задаём pixmap для всех режимов QIcon. Некоторые стили Qt/Windows
    // при наведении переключают кнопку в Active; если у иконки есть только
    // Normal, золотой значок динамика может пропасть до снятия hover.
    QIcon icon;
    icon.addPixmap(pm, QIcon::Normal, QIcon::Off);
    icon.addPixmap(pm, QIcon::Normal, QIcon::On);
    icon.addPixmap(pm, QIcon::Active, QIcon::Off);
    icon.addPixmap(pm, QIcon::Active, QIcon::On);
    icon.addPixmap(pm, QIcon::Selected, QIcon::Off);
    icon.addPixmap(pm, QIcon::Selected, QIcon::On);
    icon.addPixmap(pm, QIcon::Disabled, QIcon::Off);
    icon.addPixmap(pm, QIcon::Disabled, QIcon::On);
    return icon;
}

// Большая эмблема пустой ячейки рисуется векторно: не зависит от внешних PNG,
// сохраняет чёткость при любом размере и использует полупрозрачный голубой неон PSVMS.
QPixmap makeCameraCellArtwork(const QSize &requestedSize)
{
    const QSize size(qMax(1, requestedSize.width()), qMax(1, requestedSize.height()));
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.scale(size.width() / 128.0, size.height() / 128.0);

    // Мягкий ореол и тонкие концентрические контуры.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 170, 255, 18));
    p.drawEllipse(QRectF(5, 5, 118, 118));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(35, 190, 255, 78), 1.4));
    p.drawEllipse(QRectF(10, 10, 108, 108));
    p.setPen(QPen(QColor(45, 205, 255, 145), 1.2));
    p.drawArc(QRectF(17, 17, 94, 94), 25 * 16, 285 * 16);

    // Корпус камеры и объектив — чистая геометрия без мелких деталей.
    QPainterPath body;
    body.addRoundedRect(QRectF(25, 43, 78, 49), 11, 11);
    p.setBrush(QColor(25, 155, 235, 34));
    p.setPen(QPen(QColor(70, 210, 255, 225), 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(body);
    QPainterPath hood;
    hood.moveTo(39, 43); hood.lineTo(47, 32); hood.lineTo(76, 32); hood.lineTo(85, 43);
    p.setBrush(QColor(20, 165, 245, 30));
    p.drawPath(hood);
    p.setBrush(QColor(10, 100, 190, 55));
    p.setPen(QPen(QColor(85, 225, 255, 235), 2.6));
    p.drawEllipse(QRectF(47, 52, 34, 34));
    p.setPen(QPen(QColor(35, 190, 255, 190), 1.6));
    p.drawEllipse(QRectF(54, 59, 20, 20));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(205, 250, 255, 220));
    p.drawEllipse(QRectF(61, 66, 6, 6));

    // Небольшой индикатор состояния и опорная линия.
    p.setBrush(QColor(35, 195, 255, 225));
    p.drawEllipse(QRectF(88, 52, 5, 5));
    p.setPen(QPen(QColor(35, 195, 255, 135), 1.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(43, 101), QPointF(85, 101));
    return pm;
}

// Единый фирменный значок приложения: используется в заголовке окна и на панели задач.
static QIcon makeAppIcon()
{
    QPixmap pm(128, 128);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#171d24"));
    p.drawRoundedRect(QRectF(5, 5, 118, 118), 27, 27);
    p.setPen(QPen(QColor("#63e6ee"), 7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(23, 29, 82, 57), 10, 10);
    p.drawLine(64, 87, 64, 101);
    p.drawLine(45, 103, 83, 103);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#63e6ee"));
    p.drawEllipse(QRectF(56, 48, 16, 16));
    p.setPen(QPen(QColor("#63e6ee"), 4, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(QRectF(42, 38, 44, 38), 25 * 16, 130 * 16);
    return QIcon(pm);
}

// круглый значок пользователя с первой буквой имени
QIcon makeAvatarIcon(const QString &name)
{
    const int size = 72;   // рисуем крупнее для чёткости на экранах с высоким DPI
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x8c, 0xff, 0x00));
    p.drawEllipse(0, 0, size, size);
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(32);
    p.setFont(f);
    p.setPen(QColor(0x1a, 0x1a, 0x1c));
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, name.isEmpty() ? QStringLiteral("?") : name.left(1).toUpper());
    return QIcon(pm);
}

} // namespace

// Фоновое изображение PSVMS встроено прямо в исходник, поэтому отдельный .qrc/файл не нужен.
static QPixmap makePsvmsBrandWallpaper()
{
    static const QPixmap wallpaper = [] {
        QPixmap pm;
        const QString imagePath = QStringLiteral(":/branding/wallpaper.jpg");
        if (!pm.load(imagePath, "JPG")) {
            qWarning() << "PSVMS embedded wallpaper could not be loaded:" << imagePath;
        }
        return pm;
    }();
    return wallpaper;
}

SideNav::SideNav(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("sideNav"));
    setAttribute(Qt::WA_StyledBackground, true);
    m_wallpaper.load(QStringLiteral(":/branding/sidebar.jpg"), "JPG");
    setFixedWidth(kNavExpandedWidth);

    // Подсветка создаётся первой, поэтому лежит под кнопками меню
    m_pill = new QFrame(this);
    m_pill->setObjectName(QStringLiteral("navPill"));
    m_pill->hide();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(5);

    // Шапка: компактная кнопка с анимированным сворачиванием навигации, логотип и бренд.
    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    m_toggleBtn = new QToolButton(this);
    m_toggleBtn->setObjectName(QStringLiteral("navToggle"));
    m_toggleBtn->setText(QStringLiteral("‹"));
    m_toggleBtn->setToolTip(T("Свернуть боковую панель"));
    m_toggleBtn->setCursor(Qt::PointingHandCursor);
    m_toggleBtn->setFixedSize(32, 32);
    m_toggleBtn->setStyleSheet(QStringLiteral(
        "QToolButton{color:#8cecf4;background:rgba(8,22,32,175);border:1px solid rgba(66,205,224,125);"
        "border-radius:10px;font-size:10pt;font-weight:500;padding:0;} "
        "QToolButton:hover{color:#ffffff;background:rgba(36,169,193,55);border-color:#68e4ed;}"));
    m_brandIcon = new QLabel(this);
    // Увеличенный фирменный знак в шапке боковой панели.
    m_brandIcon->setFixedSize(64, 64);
    m_brandIcon->setAlignment(Qt::AlignCenter);
    m_brandIcon->setPixmap(QPixmap(QStringLiteral(":/branding/psvms.png")).scaled(60, 60, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_brandIcon->setToolTip(App::title());

    // Ненавязчивое «дыхание» фирменного знака: мягкое свечение без изменения
    // размера, положения или геометрии шапки боковой панели.
    auto *brandGlow = new QGraphicsDropShadowEffect(m_brandIcon);
    brandGlow->setColor(QColor(245, 183, 0, 145));
    brandGlow->setOffset(0, 0);
    brandGlow->setBlurRadius(5.0);
    m_brandIcon->setGraphicsEffect(brandGlow);
    auto *brandPulse = new QPropertyAnimation(brandGlow, "blurRadius", this);
    brandPulse->setObjectName(QStringLiteral("brandPulse"));
    brandPulse->setDuration(4200);
    brandPulse->setStartValue(5.0);
    brandPulse->setKeyValueAt(0.5, 19.0);
    brandPulse->setEndValue(5.0);
    brandPulse->setEasingCurve(QEasingCurve::InOutSine);
    brandPulse->setLoopCount(-1);
    if (Cfg::view().animations) {
        brandPulse->start();
    } else {
        // В простом режиме логотип остаётся статичным, без фоновой анимации.
        brandGlow->setBlurRadius(5.0);
    }

    m_brand = new QLabel(QStringLiteral("PSVms"), this);
    m_brand->setObjectName(QStringLiteral("navBrand"));
    m_brand->setStyleSheet(QStringLiteral("font-size:21px;font-weight:850;letter-spacing:1.2px;color:#F5B700;"));
    m_brand->setToolTip(App::title());
    head->setSpacing(5);
    head->addWidget(m_brandIcon, 0, Qt::AlignVCenter);
    head->addWidget(m_brand, 0, Qt::AlignVCenter);
    head->addStretch(1);
    head->addWidget(m_toggleBtn, 0, Qt::AlignTop);
    connect(m_toggleBtn, &QToolButton::clicked, this, [this] { setExpanded(!m_expanded, true); });
    root->addLayout(head);
    root->addSpacing(18);

    m_itemsLayout = new QVBoxLayout;
    m_itemsLayout->setContentsMargins(0, 0, 0, 0);
    m_itemsLayout->setSpacing(7);
    root->addLayout(m_itemsLayout);
    root->addStretch(1);

    m_bottomLayout = new QVBoxLayout;
    m_bottomLayout->setContentsMargins(0, 0, 0, 0);
    m_bottomLayout->setSpacing(6);
    root->addLayout(m_bottomLayout);
    root->addSpacing(4);

    // Учётная запись располагается над нижними действиями; кнопка Exit — последняя.
    m_userBtn = new QPushButton(this);
    m_userBtn->setObjectName(QStringLiteral("navUserBtn"));
    m_userBtn->setCursor(Qt::PointingHandCursor);
    m_userBtn->setFocusPolicy(Qt::NoFocus);
    m_userBtn->setMinimumHeight(40);
    m_userBtn->setMaximumHeight(44);
    m_userBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_userBtn->setStyleSheet(QStringLiteral("QPushButton#navUserBtn{padding:5px 8px;font-family:'Segoe UI';font-size:10pt;font-weight:600;text-align:left;}"));
    m_userBtn->setIconSize(QSize(36, 36));
    m_bottomLayout->addWidget(m_userBtn);

    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    connect(m_group, &QButtonGroup::idClicked, this, [this](int i) {
        if (i >= 0 && i < m_items.size()) {
            if (m_items.at(i).isPage)
                setCurrent(m_items.at(i).pageId, true);
            else
                emit actionTriggered(i);
        }
    });
    connect(m_userBtn, &QPushButton::clicked, this, [this] { showUserMenu(); });

    applyTexts();
}

// Единый набор контурных иконок основного меню: одинаковый холст и толщина линий.
static QIcon makeNavigationIcon(const QString &kind)
{
    // Все основные значки берутся из Qt Resource System и встраиваются в EXE.
    static const QHash<QString, QString> iconAliases = {
        {QStringLiteral("▣"), QStringLiteral("home")},
        {QStringLiteral("▤"), QStringLiteral("camera")},
        {QStringLiteral("▧"), QStringLiteral("archive")},
        {QStringLiteral("♟"), QStringLiteral("users")},
        {QStringLiteral("⚙"), QStringLiteral("settings")},
        {QStringLiteral("ⓘ"), QStringLiteral("about")}
    };
    const QString alias = iconAliases.value(kind, QStringLiteral("events"));
    const QString resourcePath = QStringLiteral(":/branding/icons/") + alias + QStringLiteral(".png");
    if (QFile::exists(resourcePath))
        return QIcon(resourcePath);

    QPixmap pm(40, 40);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor gold("#F5B700");
    const QColor light("#d7dce1");
    p.setPen(QPen(gold, 2.25, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (kind == QStringLiteral("▣")) { // мониторинг: экран с четырьмя каналами
        p.drawRoundedRect(QRectF(4.5, 6, 31, 23), 3, 3);
        p.drawLine(20, 29, 20, 34);
        p.drawLine(13, 35, 27, 35);
        p.drawLine(20, 29, 20, 34);
        p.drawLine(20, 17, 20, 26);
        p.drawLine(9, 17, 31, 17);
        p.drawRect(QRectF(9, 11, 8, 5));
        p.drawRect(QRectF(23, 11, 8, 5));
        p.drawRect(QRectF(9, 19, 8, 6));
        p.drawRect(QRectF(23, 19, 8, 6));
    } else if (kind == QStringLiteral("▤")) { // устройства: купольная IP-камера
        p.drawRoundedRect(QRectF(5, 7, 30, 12), 5, 5);
        p.drawArc(QRectF(8, 13, 24, 22), 180 * 16, 180 * 16);
        p.drawEllipse(QRectF(15, 20, 10, 10));
        p.setPen(QPen(light, 1.5));
        p.drawEllipse(QRectF(18, 23, 4, 4));
        p.setPen(QPen(gold, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(11, 20, 8, 24);
        p.drawLine(29, 20, 32, 24);
    } else if (kind == QStringLiteral("▧")) { // архив
        p.drawRoundedRect(QRectF(5, 8, 30, 26), 4, 4);
        p.drawLine(5, 15, 35, 15);
        p.drawLine(15, 20, 25, 20);
        p.drawLine(15, 25, 25, 25);
    } else if (kind == QStringLiteral("♟")) { // пользователи
        p.drawEllipse(QRectF(13, 5, 14, 14));
        p.drawArc(QRectF(5, 18, 30, 24), 0, 180 * 16);
        p.drawEllipse(QRectF(3, 12, 9, 9));
        p.drawEllipse(QRectF(28, 12, 9, 9));
    } else if (kind == QStringLiteral("⚙")) { // настройки
        p.drawEllipse(QRectF(11, 11, 18, 18));
        p.drawEllipse(QRectF(17, 17, 6, 6));
        for (int i = 0; i < 8; ++i) {
            p.save(); p.translate(20, 20); p.rotate(i * 45.0);
            p.drawRoundedRect(QRectF(-2.5, -19, 5, 7), 1.5, 1.5); p.restore();
        }
    } else if (kind == QStringLiteral("ⓘ")) { // о программе
        p.drawEllipse(QRectF(4, 4, 32, 32));
        p.setPen(QPen(light, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawEllipse(QRectF(18, 11, 4, 4));
        p.drawLine(20, 19, 20, 29);
    } else { // события: журнал с отметкой времени
        p.drawRoundedRect(QRectF(7, 5, 26, 30), 3, 3);
        p.drawLine(13, 4, 13, 10);
        p.drawLine(27, 4, 27, 10);
        p.drawLine(8, 13, 32, 13);
        p.drawLine(12, 19, 18, 19);
        p.drawLine(12, 24, 18, 24);
        p.drawLine(12, 29, 18, 29);
        p.drawEllipse(QRectF(21, 19, 9, 9));
        p.drawLine(25.5, 21, 25.5, 24);
        p.drawLine(25.5, 24, 28, 25.5);
    }
    return QIcon(pm);
}

static QIcon makeCameraStatusIcon()
{
    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor cyan("#F5B700");
    const QColor gold("#FFD54F");
    // Чёткий силуэт камеры: корпус, объектив и небольшой индикатор записи.
    p.setPen(QPen(cyan, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(QColor(15, 68, 83, 125));
    p.drawRoundedRect(QRectF(5.5, 13, 27, 22), 5, 5);
    QPolygonF lens;
    lens << QPointF(33, 19) << QPointF(43, 14) << QPointF(43, 34) << QPointF(33, 29);
    p.drawPolygon(lens);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(gold, 2.4));
    p.drawEllipse(QRectF(13, 18, 12, 12));
    p.setPen(QPen(QColor("#fff0b0"), 1.5));
    p.drawPoint(QPointF(19, 24));
    p.setPen(Qt::NoPen);
    p.setBrush(gold);
    p.drawEllipse(QRectF(8, 7, 6, 6));
    return QIcon(pm);
}

int SideNav::addItem(const QString &icon, const QString &title, bool isPage)
{
    auto *b = new QPushButton(this);
    b->setObjectName(QStringLiteral("navItem"));
    b->setCheckable(isPage);
    b->setCursor(Qt::PointingHandCursor);
    b->setIcon(makeNavigationIcon(icon));
    b->setIconSize(QSize(32, 32));
    b->setFocusPolicy(Qt::NoFocus);
    // Единая геометрия всех пунктов навигации; подпись адаптируется отдельно.
    b->setFixedHeight(54);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->installEventFilter(this);   // следим за положением кнопки, чтобы подсветка не отставала

    const int idx = int(m_items.size());
    Item it;
    it.icon = icon;
    it.title = title;
    it.button = b;
    it.isPage = isPage;
    it.pageId = isPage ? m_nextPageId++ : -1;
    m_items.append(it);
    m_group->addButton(b, idx);
    m_itemsLayout->addWidget(b);
    applyTexts();
    return idx;
}

// Фирменные монохромные значки нижних пунктов меню: графит + янтарное золото.
static QIcon makeSideActionIcon(bool language)
{
    const QString resourcePath = language
        ? QStringLiteral(":/branding/icons/language.png")
        : QStringLiteral(":/branding/icons/settings.png");
    if (QFile::exists(resourcePath))
        return QIcon(resourcePath);

    QPixmap pm(40, 40);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(QColor("#F5B700"), 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    if (language) {
        p.drawEllipse(QRectF(5, 5, 30, 30));
        p.drawEllipse(QRectF(13, 5, 14, 30));
        p.drawArc(QRectF(7, 12, 26, 16), 0, 180 * 16);
        p.drawArc(QRectF(7, 12, 26, 16), 180 * 16, 180 * 16);
        p.drawLine(6, 20, 34, 20);
    } else {
        const QPointF c(20, 20);
        for (int i = 0; i < 8; ++i) {
            p.save(); p.translate(c); p.rotate(i * 45.0);
            p.drawRoundedRect(QRectF(-3.0, -17.0, 6.0, 9.0), 1.5, 1.5);
            p.restore();
        }
        p.drawEllipse(QRectF(8, 8, 24, 24));
        p.setPen(QPen(QColor("#F5B700"), 2.0));
        p.drawEllipse(QRectF(15, 15, 10, 10));
    }
    return QIcon(pm);
}

// Нижние действия сделаны крупнее; настройки и язык выделены фирменными векторными значками.
int SideNav::addBottomAction(const QString &icon, const QString &title)
{
    auto *b = new QPushButton(this);
    b->setObjectName(QStringLiteral("navBottomItem"));
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    const bool isExit = (icon == QStringLiteral("EXIT"));
    const bool prominent = (icon == QStringLiteral("⚙") || icon == QStringLiteral("🌐") || isExit);
    b->setProperty("prominent", prominent);
    b->setProperty("exitAction", isExit);
    // Одинаковая высота для обычных, системных и кнопки выхода.
    b->setFixedHeight(54);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (isExit) {
        const QString exitResource = QStringLiteral(":/branding/icons/exit.png");
        if (QFile::exists(exitResource)) {
            b->setIcon(QIcon(exitResource));
            b->setIconSize(QSize(32, 32));
        } else {
        QPixmap pm(40, 40);
        pm.fill(Qt::transparent);
        QPainter painter(&pm);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor("#F5B700"), 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(5, 5, 20, 30), 3, 3);
        painter.drawLine(15, 20, 35, 20);
        painter.drawLine(28, 13, 35, 20);
        painter.drawLine(28, 27, 35, 20);
        b->setIcon(QIcon(pm));
        b->setIconSize(QSize(32, 32));
        }
    } else if (prominent) {
        b->setIcon(makeSideActionIcon(icon == QStringLiteral("🌐")));
        b->setIconSize(QSize(32, 32));
    } else {
        b->setIconSize(QSize(32, 32));
    }

    const int idx = int(m_items.size());
    Item it;
    it.icon = icon;
    it.title = title;
    it.button = b;
    it.isPage = false;
    m_items.append(it);
    m_bottomLayout->addWidget(b);
    connect(b, &QPushButton::clicked, this, [this, idx] { emit actionTriggered(idx); });
    applyTexts();
    return idx;
}

int SideNav::addPage(const QString &icon, const QString &title)
{
    return addItem(icon, title, true);
}

int SideNav::addAction(const QString &icon, const QString &title)
{
    const int idx = addItem(icon, title, false);
    // пункт-действие не должен участвовать в выборе страницы
    m_group->removeButton(m_items.at(idx).button);
    connect(m_items.at(idx).button, &QPushButton::clicked, this, [this, idx] { emit actionTriggered(idx); });
    return idx;
}

void SideNav::setItemText(int index, const QString &icon, const QString &title)
{
    if (index < 0 || index >= m_items.size())
        return;
    m_items[index].icon = icon;
    m_items[index].title = title;
    applyTexts();
}

void SideNav::setItemVisible(int pageOrItemId, bool visible)
{
    // Для страниц используем постоянный pageId, поэтому скрытие/показ не ломается
    // после пользовательской перестановки пунктов меню.
    for (const Item &it : std::as_const(m_items)) {
        if (it.isPage && it.pageId == pageOrItemId) {
            it.button->setVisible(visible);
            if (m_current >= 0 && m_current < m_items.size() && m_items.at(m_current).button == it.button)
                updatePill(false);
            return;
        }
    }
    if (pageOrItemId >= 0 && pageOrItemId < m_items.size())
        m_items.at(pageOrItemId).button->setVisible(visible);
}

QWidget *SideNav::itemButton(int pageOrItemId) const
{
    for (const Item &it : std::as_const(m_items))
        if (it.isPage && it.pageId == pageOrItemId) return it.button;
    return (pageOrItemId >= 0 && pageOrItemId < m_items.size())
        ? m_items.at(pageOrItemId).button : nullptr;
}

void SideNav::setCurrent(int pageId, bool animate)
{
    int itemIndex = -1;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).isPage && m_items.at(i).pageId == pageId) {
            itemIndex = i;
            break;
        }
    }
    if (itemIndex < 0)
        return;
    const bool changed = itemIndex != m_current;
    m_current = itemIndex;
    m_items.at(itemIndex).button->setChecked(true);
    updatePill(animate && changed && Cfg::view().animations);
    // Сигнал передаёт стабильный ID страницы, а не её изменяемую позицию в меню.
    emit currentChanged(pageId);
}

void SideNav::setUser(const QString &name, const QString &role)
{
    m_userName = name;
    m_userRole = role;
    m_userBtn->setIcon(makeAvatarIcon(name));
    applyTexts();
}

void SideNav::showUserMenu()
{
    QMenu menu(this);
    QAction *title = menu.addAction(m_userName + QStringLiteral(" — ") + m_userRole);
    title->setEnabled(false);
    menu.addSeparator();
    QAction *pw = menu.addAction(T("Сменить пароль…"));
    QAction *out = menu.addAction(T("Сменить пользователя…"));
    connect(pw, &QAction::triggered, this, [this] { emit changePasswordRequested(); });
    connect(out, &QAction::triggered, this, [this] { emit logoutRequested(); });
    menu.exec(m_userBtn->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
}

void SideNav::setNavWidth(int w)
{
    setFixedWidth(w);
}

void SideNav::setExpanded(bool on, bool animate)
{
    const int target = on ? kNavExpandedWidth : kNavCollapsedWidth;
    if (on == m_expanded && width() == target)
        return;
    m_expanded = on;
    // Состояние панели сохраняется сразу, а не только при штатном закрытии окна.
    QSettings().setValue(QStringLiteral("ui/navExpanded"), m_expanded);

    if (m_widthAnim)
        m_widthAnim->stop();
    if (!animate || !Cfg::view().animations) {
        setNavWidth(target);
        applyTexts();
        return;
    }
    if (on)
        applyTexts();   // при раскрытии названия появляются сразу, пока меню растёт

    auto *anim = new QPropertyAnimation(this, "navWidth", this);
    anim->setDuration(220);
    anim->setEasingCurve(QEasingCurve::InOutCubic);
    anim->setStartValue(width());
    anim->setEndValue(target);
    connect(anim, &QPropertyAnimation::finished, this, [this] { applyTexts(); });   // при сворачивании — после анимации
    m_widthAnim = anim;
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void SideNav::applyTexts()
{
    for (const Item &it : std::as_const(m_items)) {
        const bool prominent = it.button->property("prominent").toBool();
        if (prominent) {
            it.button->setText(m_expanded ? it.title : QString());
            it.button->setToolTip(m_expanded ? QString() : it.title);
        } else if (it.isPage) {
            it.button->setText(m_expanded ? it.title : QString());
            it.button->setToolTip(m_expanded ? QString() : it.title);
        } else if (m_expanded) {
            it.button->setText(it.title);
            it.button->setToolTip(QString());
        } else {
            it.button->setText(QString());
            it.button->setToolTip(it.title);
        }
        it.button->setProperty("collapsed", !m_expanded);
        if (m_expanded) {
            it.button->setIconSize(QSize(32, 32));
            it.button->setMinimumWidth(0);
            it.button->setMaximumWidth(QWIDGETSIZE_MAX);
        } else {
            // В свёрнутом меню кнопка строго помещается во внутреннюю область
            // панели (68 px минус 12 px полей с каждой стороны = 44 px).
            // Фиксированная ширина исключает вылезание иконок за границы ячеек.
            it.button->setIconSize(QSize(24, 24));
            it.button->setFixedWidth(kNavCollapsedWidth - 24);
        }

        // Единая типографика меню в духе iVMS-4200: один шрифт,
        // один размер и одинаковая насыщенность для всех пунктов,
        // включая нижние действия. Длинные подписи не уменьшают шрифт.
        QFont navFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold);
        navFont.setStyleStrategy(QFont::PreferAntialias);
        it.button->setFont(navFont);
        if (m_expanded &&
            QFontMetrics(navFont).horizontalAdvance(it.button->text()) >
                qMax(70, kNavExpandedWidth - 30 - (it.button->icon().isNull() ? 12 : 52))) {
            it.button->setToolTip(it.button->text());
        }

        it.button->style()->unpolish(it.button);
        it.button->style()->polish(it.button);
    }
    QFont userNavFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold);
    userNavFont.setStyleStrategy(QFont::PreferAntialias);
    m_userBtn->setFont(userNavFont);
    m_userBtn->setText(m_expanded ? QStringLiteral("  ") + QFontMetrics(userNavFont).elidedText(m_userName, Qt::ElideRight, kNavExpandedWidth - 82) : QString());
    m_userBtn->setToolTip(m_expanded ? QString() : m_userName);
    for (const Item &it : std::as_const(m_items)) {
        if (it.button && it.button->objectName() == QStringLiteral("navBottomItem")) {
            if (m_expanded) {
                it.button->setIconSize(QSize(32, 32));
                it.button->setMinimumWidth(0);
                it.button->setMaximumWidth(QWIDGETSIZE_MAX);
            } else {
                it.button->setIconSize(QSize(24, 24));
                it.button->setFixedWidth(kNavCollapsedWidth - 24);
            }
        }
    }
    m_userBtn->setProperty("collapsed", !m_expanded);
    if (m_expanded) {
        m_userBtn->setIconSize(QSize(36, 36));
        m_userBtn->setMinimumWidth(0);
        m_userBtn->setMaximumWidth(QWIDGETSIZE_MAX);
    } else {
        m_userBtn->setIconSize(QSize(24, 24));
        m_userBtn->setFixedWidth(kNavCollapsedWidth - 24);
    }
    m_userBtn->style()->unpolish(m_userBtn);
    m_userBtn->style()->polish(m_userBtn);
    m_brand->setVisible(m_expanded);
    if (m_brandIcon) m_brandIcon->setVisible(m_expanded);
    if (m_toggleBtn) {
        m_toggleBtn->setText(m_expanded ? QStringLiteral("‹") : QStringLiteral("›"));
        m_toggleBtn->setToolTip(m_expanded ? T("Свернуть боковую панель") : T("Развернуть боковую панель"));
    }
}

void SideNav::updatePill(bool animate)
{
    if (m_current < 0 || m_current >= m_items.size())
        return;
    const QRect target = m_items.at(m_current).button->geometry();

    if (m_pillAnim)
        m_pillAnim->stop();
    if (!animate || !Cfg::view().animations || !m_pill->isVisible()) {
        m_pill->setGeometry(target);
        m_pill->show();
        m_pill->lower();
        return;
    }
    auto *anim = new QPropertyAnimation(m_pill, "geometry", this);
    anim->setDuration(280);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(m_pill->geometry());
    anim->setEndValue(target);
    m_pillAnim = anim;
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void SideNav::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e);
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.fillRect(rect(), QColor(4, 12, 24, 72));
    if (!m_wallpaper.isNull()) {
        const QSize hostSize = size();
        if (m_wallpaperScaled.isNull() || m_wallpaperHostSize != hostSize) {
            m_wallpaperHostSize = hostSize;
            const QSize scaled = m_wallpaper.size().scaled(hostSize, Qt::KeepAspectRatioByExpanding);
            m_wallpaperScaled = m_wallpaper.scaled(scaled, Qt::KeepAspectRatioByExpanding,
                                                    Qt::FastTransformation);
        }
        const QPixmap &fitted = m_wallpaperScaled;
        // Боковая панель использует левую часть композиции: логотип остаётся в кадре,
        // а камера не обрезает важные подписи меню.
        const int cropX = qMax(0, (fitted.width() - width()) / 2);
        const int cropY = qMax(0, (fitted.height() - height()) / 2);
        const QRect source(cropX, cropY, qMin(width(), fitted.width()-cropX), qMin(height(), fitted.height()-cropY));
        p.drawPixmap(rect(), fitted, source);
        // Оставляем изображение камеры различимым даже под пунктами меню.
        p.fillRect(rect(), QColor(5, 10, 16, 24));
        QLinearGradient shade(0, 0, width(), 0);
        shade.setColorAt(0.0, QColor(7, 12, 18, 38));
        shade.setColorAt(1.0, QColor(7, 12, 18, 14));
        p.fillRect(rect(), shade);
    }
    p.setPen(QPen(QColor(48, 190, 226, 185), 1));
    p.drawLine(width()-1, 0, width()-1, height());
}

int SideNav::itemAtGlobalY(int globalY) const
{
    if (m_items.isEmpty()) return -1;
    int lastPage = -1;
    for (int i = 0; i < m_items.size(); ++i) {
        if (!m_items.at(i).isPage) continue;
        QPushButton *button = m_items.at(i).button;
        if (!button || !button->isVisible()) continue;
        lastPage = i;
        const int center = button->mapToGlobal(QPoint(0, button->height() / 2)).y();
        if (globalY < center) return i;
    }
    return lastPage;
}

void SideNav::reorderItem(int from, int to)
{
    if (from < 0 || to < 0 || from >= m_items.size() || to >= m_items.size() || from == to
        || !m_items.at(from).isPage || !m_items.at(to).isPage)
        return;
    QPushButton *currentButton = (m_current >= 0 && m_current < m_items.size())
        ? m_items.at(m_current).button : nullptr;
    Item moved = m_items.takeAt(from);
    m_items.insert(to, moved);

    if (m_itemsLayout) {
        m_itemsLayout->removeWidget(moved.button);
        int layoutIndex = 0;
        for (int i = 0; i < to; ++i)
            if (m_items.at(i).button && m_itemsLayout->indexOf(m_items.at(i).button) >= 0)
                ++layoutIndex;
        m_itemsLayout->insertWidget(layoutIndex, moved.button);
    }
    if (m_group) {
        // В группе остаются только страницы. Действия (Архив/Выход) не должны
        // попадать в эксклюзивный выбор при перестановке страниц.
        for (const Item &it : std::as_const(m_items))
            m_group->removeButton(it.button);
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items.at(i).isPage)
                m_group->addButton(m_items.at(i).button, i);
    }
    m_current = -1;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).button == currentButton) { m_current = i; break; }
    }
    updatePill(false);
    QStringList order;
    for (const Item &it : std::as_const(m_items))
        if (it.isPage) order << QString::number(it.pageId);
    QSettings settings;
    settings.setValue(QStringLiteral("ui/navPageOrder"), order);
    settings.sync();
}

void SideNav::restorePageOrder()
{
    const QStringList saved = QSettings().value(QStringLiteral("ui/navPageOrder")).toStringList();
    if (saved.isEmpty()) return;
    for (int target = 0; target < saved.size(); ++target) {
        bool ok = false;
        const int pageId = saved.at(target).toInt(&ok);
        if (!ok) continue;
        int from = -1;
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items.at(i).isPage && m_items.at(i).pageId == pageId) { from = i; break; }
        if (from >= 0 && from != target && target < m_items.size() && m_items.at(target).isPage)
            reorderItem(from, target);
    }
}

bool SideNav::eventFilter(QObject *obj, QEvent *e)
{
    auto *button = qobject_cast<QPushButton*>(obj);
    int itemIndex = -1;
    if (button) {
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items.at(i).button == button) { itemIndex = i; break; }
    }

    if (button && itemIndex >= 0 && m_items.at(itemIndex).isPage) {
        if (e->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent*>(e);
            if (mouse->button() == Qt::LeftButton) {
                m_dragButton = button;
                m_dragStartPos = mouse->globalPosition().toPoint();
                m_dragging = false;
            }
        } else if (e->type() == QEvent::MouseMove && m_dragButton == button) {
            auto *mouse = static_cast<QMouseEvent*>(e);
            const QPoint global = mouse->globalPosition().toPoint();
            if (!m_dragging && (global - m_dragStartPos).manhattanLength() >= QApplication::startDragDistance()) {
                m_dragging = true;
                button->setProperty("dragging", true);
                button->style()->unpolish(button);
                button->style()->polish(button);
                button->setCursor(Qt::ClosedHandCursor);
            }
            if (m_dragging) {
                int from = -1;
                for (int i = 0; i < m_items.size(); ++i)
                    if (m_items.at(i).button == button) { from = i; break; }
                const int to = itemAtGlobalY(global.y());
                if (from >= 0 && to >= 0 && from != to) reorderItem(from, to);
            }
        } else if (e->type() == QEvent::MouseButtonRelease && m_dragButton == button) {
            if (m_dragging) {
                button->setProperty("dragging", false);
                button->style()->unpolish(button);
                button->style()->polish(button);
            }
            button->setCursor(Qt::PointingHandCursor);
            m_dragButton.clear();
            m_dragging = false;
        }
    }

    // Кнопка сдвинулась или изменила размер: подсветка следует за выбранным пунктом.
    if ((e->type() == QEvent::Resize || e->type() == QEvent::Move) && m_current >= 0
        && m_current < m_items.size() && obj == m_items.at(m_current).button && !m_pillAnim)
        updatePill(false);
    return QWidget::eventFilter(obj, e);
}

// ======================================================================
// app/splash.cpp
// ======================================================================
static QPixmap psvmsSplashPixmap()
{
    // Splash artwork is shipped in assets/ to keep large binary data out of the C++ translation unit.
    qInfo() << "PSVMS splash: pixmap load entered";
    QPixmap source;
    const QString imagePath = QStringLiteral(":/branding/splash.png");
    qInfo() << "PSVMS splash: loading embedded image" << imagePath;
    const bool loaded = source.load(imagePath, "PNG");
    qInfo() << "PSVMS splash: image load finished" << loaded
            << "size" << source.size();
    if (source.isNull()) {
        qWarning() << "PSVMS splash image could not be loaded:" << imagePath;
        QPixmap fallback(1200, 675);
        fallback.fill(QColor("#0D0F12"));
        QPainter painter(&fallback);
        painter.setPen(QColor("#F5B700"));
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 58, QFont::Black));
        painter.drawText(fallback.rect(), Qt::AlignCenter, QStringLiteral("PSVms"));
        return fallback;
    }
    qInfo() << "PSVMS splash: scaling image entered";
    QPixmap result = source.scaled(QSize(1200, 675), Qt::KeepAspectRatioByExpanding,
                                   Qt::SmoothTransformation);
    qInfo() << "PSVMS splash: scaling image finished" << result.size();
    return result;
}

class AnimatedSplash final : public QSplashScreen
{
public:
    AnimatedSplash()
        : QSplashScreen(psvmsSplashPixmap(),
                        Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint)
    {
        qInfo() << "PSVMS splash: AnimatedSplash constructor entered";
        setFixedSize(pixmap().size());
        setWindowTitle(QStringLiteral("PSVms"));
        m_timer = new QTimer(this);
        m_timer->setInterval(40);
        connect(m_timer, &QTimer::timeout, this, [this] {
            m_phase = std::fmod(m_phase + 0.018, 1.0);
            update();
        });
        // Заставка всегда анимирована независимо от режима интерфейса.
        m_timer->start();
        qInfo() << "PSVMS splash: AnimatedSplash constructor finished; animation="
                << Cfg::view().animations;
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QSplashScreen::paintEvent(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal pulse = 0.5 + 0.5 * std::sin(m_phase * 6.283185307179586);
        const QPointF center(width() / 2.0, 235.0);

        p.setPen(QPen(QColor(245, 183, 0, 55 + int(95 * pulse)), 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, 100.0 + 6.0 * pulse, 100.0 + 6.0 * pulse);

        const QRectF track(350, 444, 500, 5);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(52, 60, 74, 220));
        p.drawRoundedRect(track, 2.5, 2.5);

        const qreal barW = 150.0;
        const qreal barX = track.left() + m_phase * (track.width() - barW);
        p.setBrush(QColor(245, 183, 0, 240));
        p.drawRoundedRect(QRectF(barX, track.top(), barW, track.height()), 2.5, 2.5);

        p.setPen(QColor(236, 240, 245, 235));
        p.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold));
        p.drawText(QRect(260, 496, width() - 520, 30), Qt::AlignCenter,
                   T("PSVms  •  ЗАГРУЗКА СИСТЕМЫ И МОДУЛЕЙ ВИДЕОНАБЛЮДЕНИЯ"));
    }

private:
    QTimer *m_timer = nullptr;
    qreal m_phase = 0.0;
};

QSplashScreen *createSplash()
{
    qInfo() << "PSVMS splash: createSplash entered";
    auto *splash = new AnimatedSplash;
    // Заставка не является главным окном приложения. Её закрытие не должно
    // инициировать QApplication::quit() до показа окна входа.
    splash->setAttribute(Qt::WA_QuitOnClose, false);
    qInfo() << "PSVMS splash: before show";
    splash->show();
    qInfo() << "PSVMS splash: after show; before processEvents";
    QCoreApplication::processEvents();
    qInfo() << "PSVMS splash: createSplash finished";
    return splash;
}

void finishSplash(QSplashScreen *splash)
{
    qInfo() << "PSVMS splash: finishSplash entered";
    if (!splash)
        return;
    {
        QEventLoop loop;
        auto *anim = new QPropertyAnimation(splash, "windowOpacity", splash);
        anim->setDuration(700);
        anim->setStartValue(1.0);
        anim->setEndValue(0.0);
        QObject::connect(anim, &QPropertyAnimation::finished, &loop, &QEventLoop::quit);
        anim->start();
        qInfo() << "PSVMS splash: fade animation started (mode-independent)";
        loop.exec();
        qInfo() << "PSVMS splash: fade animation finished";
    }
    splash->close();
    qInfo() << "PSVMS splash: splash closed; deleting";
    delete splash;
    qInfo() << "PSVMS splash: finishSplash completed";
}

// ======================================================================
// app/status_dialog.cpp
// ======================================================================
CameraStatusDialog::CameraStatusDialog(CameraManager *mgr, QWidget *parent) : QDialog(parent), m_mgr(mgr)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(T("Состояние камер"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(860, 560);

    auto *cards = new QHBoxLayout;
    for (int i = 0; i < 4; ++i) {
        m_cards[i] = new QLabel(this);
        m_cards[i]->setObjectName(QStringLiteral("statCard"));
        m_cards[i]->setAlignment(Qt::AlignCenter);
        m_cards[i]->setTextFormat(Qt::RichText);
        m_cards[i]->setMinimumHeight(86);
        cards->addWidget(m_cards[i], 1);
    }

    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({T("Название"), T("Группа"), T("Адрес"), T("Связь"), T("Запись")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 220);
    m_table->setColumnWidth(1, 160);
    m_table->setColumnWidth(3, 110);
    m_table->setColumnWidth(4, 200);
    m_table->setMouseTracking(true);
    m_table->viewport()->setMouseTracking(true);
    new CameraHoverPreview(m_table, m_mgr, m_table);

    auto *check = new QPushButton(T("Проверить сейчас"), this);
    auto *close = new QPushButton(T("Закрыть"), this);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(check);
    bottom->addStretch(1);
    bottom->addWidget(close);

    auto *root = new QVBoxLayout(this);
    root->addLayout(cards);
    root->addWidget(m_table, 1);
    root->addLayout(bottom);

    connect(check, &QPushButton::clicked, m_mgr, &CameraManager::probeNow);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(m_mgr, &CameraManager::onlineChanged, this, [this] { rebuild(); });
    connect(m_mgr, &CameraManager::camerasChanged, this, [this] { rebuild(); });
    rebuild();
}

void CameraStatusDialog::showOfflineOnly()
{
    m_offlineOnly = true;
    setWindowTitle(T("Камеры не в сети"));
    rebuild();
    show();
    raise();
    activateWindow();
}

void CameraStatusDialog::showAllCameras()
{
    m_offlineOnly = false;
    setWindowTitle(T("Состояние камер"));
    rebuild();
    show();
    raise();
    activateWindow();
}

void CameraStatusDialog::rebuild()
{
    const int total = int(m_mgr->cameras().size());
    const struct { QString label; int value; const char *color; } cards[4] = {
        {T("Всего камер"), total, "#e6e6e6"},
        {T("В сети"), m_mgr->onlineCount(), "#35e58b"},
        {T("Не в сети"), m_mgr->offlineCount(), "#F5B700"},
        {T("Идёт запись"), m_mgr->recordingCount(), "#FFD54F"},
    };
    for (int i = 0; i < 4; ++i) {
        m_cards[i]->setText(QStringLiteral("<span style='font-size:10pt; font-weight:700; color:%1'>%2</span><br>"
                                           "<span style='color:#a8a8ac'>%3</span>")
                                .arg(QLatin1String(cards[i].color)).arg(cards[i].value).arg(cards[i].label));
    }

    m_table->setRowCount(0);
    for (const CameraEntry &e : m_mgr->cameras()) {
        const bool online = m_mgr->isOnline(e.id);
        if (m_offlineOnly && online)
            continue;
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QString rec = m_mgr->isStarted(e.id) ? stateText(m_mgr->stateOf(e.id)) : T("Не запущена");
        const QString cells[5] = {e.name, e.group, QUrl(e.url).host(), online ? T("● В сети") : T("● Не в сети"), rec};
        for (int c = 0; c < 5; ++c) {
            auto *it = new QTableWidgetItem(cells[c]);
            if (c == 0)
                it->setData(Qt::UserRole, e.id);
            if (c == 3)
                it->setForeground(QBrush(QColor(online ? "#35e58b" : "#ff7777")));
            m_table->setItem(row, c, it);
        }
    }
}

// ======================================================================
// app/top_bar.cpp
// ======================================================================
class TopBarDraggableLabel : public QLabel {
public:
    using QLabel::QLabel;
};

// Единая типографика верхней панели. Шрифт автоматически уменьшается
// только при необходимости, чтобы длинные переводы и счётчики не выходили
// за границы своих кнопок/индикаторов.
static void fitTopBarText(QWidget *widget, const QString &text, qreal basePointSize = 10.0)
{
    if (!widget) return;
    QFont font(QStringLiteral("Segoe UI"), 10, QFont::DemiBold);
    font.setPointSizeF(basePointSize);
    const int available = qMax(24, widget->width() - 20);
    for (qreal pt = basePointSize; pt >= 8.0; pt -= 0.25) {
        font.setPointSizeF(pt);
        if (QFontMetrics(font).horizontalAdvance(text) <= available)
            break;
    }
    widget->setFont(font);
}

TopBar::TopBar(CameraManager *mgr, QWidget *parent) : QWidget(parent), m_mgr(mgr)
{
    setObjectName(QStringLiteral("topBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(96);

    // Заголовок слева; элементы управления размещаются на невидимой сетке.
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("topTitle"));

    m_online = new TopBarDraggableLabel(this);
    m_online->setObjectName(QStringLiteral("chipOnline"));
    m_online->setAlignment(Qt::AlignCenter);
    m_online->setFixedSize(176, 42);
    m_online->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_online->setTextInteractionFlags(Qt::NoTextInteraction);
    fitTopBarText(m_online, m_online->text());
    m_online->setCursor(Qt::ArrowCursor);
    m_online->setToolTip(T("Количество камер в сети"));

    m_offline = new TopBarDraggableLabel(this);
    m_offline->setObjectName(QStringLiteral("chipOffline"));
    m_offline->setAlignment(Qt::AlignCenter);
    m_offline->setFixedSize(176, 42);
    m_offline->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_offline->setTextInteractionFlags(Qt::NoTextInteraction);
    fitTopBarText(m_offline, m_offline->text());
    m_offline->setCursor(Qt::ArrowCursor);
    m_offline->setToolTip(T("Количество камер не в сети"));

    m_fullscreen = new QPushButton(T("⛶  Во весь экран"), this);
    m_fullscreen->setObjectName(QStringLiteral("topFullscreenBtn"));
    m_fullscreen->setStyleSheet(QStringLiteral(
        "QPushButton#topFullscreenBtn{font-family:'Segoe UI';font-weight:600;"
        "letter-spacing:0.2px;padding:0px 8px;}"));
    m_fullscreen->setCursor(Qt::PointingHandCursor);
    m_fullscreen->setFixedSize(176, 42);
    fitTopBarText(m_fullscreen, m_fullscreen->text());
    m_fullscreen->setToolTip(T("Переключить полноэкранный режим (F11)"));

    m_status = new QPushButton(T("Состояние"), this);
    m_status->setObjectName(QStringLiteral("cameraStatusBtn"));
    m_status->setIcon(QIcon());
    m_status->setFixedSize(176, 42);
    m_status->setStyleSheet(QStringLiteral(
        "QPushButton#cameraStatusBtn{padding:0px 36px 0px 16px;text-align:center;"
        "font-family:'Segoe UI';font-weight:600;letter-spacing:0.2px;}"));
    fitTopBarText(m_status, m_status->text());
    // Новая компоновка: пиктограмма справа от текста, по вертикальному центру,
    // с отдельным отступом, чтобы значок не касался надписи.
    auto *statusGlyph = new QLabel(m_status);
    statusGlyph->setObjectName(QStringLiteral("cameraStatusGlyph"));
    statusGlyph->setPixmap(makeCameraStatusIcon().pixmap(QSize(23, 23)));
    statusGlyph->setFixedSize(23, 23);
    statusGlyph->setAlignment(Qt::AlignCenter);
    statusGlyph->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // Центрируем надпись и значок как одну группу: значок справа от текста,
    // с небольшим визуальным интервалом, а не над надписью.
    QFont statusFont(QStringLiteral("Segoe UI"), 9, QFont::DemiBold);
    statusFont.setPointSizeF(10.0);
    m_status->setFont(statusFont);
    const int statusTextWidth = QFontMetrics(statusFont).horizontalAdvance(m_status->text());
    const int statusGap = 9;
    const int statusGroupWidth = statusTextWidth + statusGap + statusGlyph->width();
    const int statusGroupLeft = (m_status->width() - statusGroupWidth) / 2;
    statusGlyph->move(statusGroupLeft + statusTextWidth + statusGap,
                      (m_status->height() - statusGlyph->height()) / 2);
    statusGlyph->raise();
    m_status->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    m_status->setCursor(Qt::PointingHandCursor);
    m_status->setToolTip(T("Открыть состояние камер"));

    m_cloud = new QPushButton(QStringLiteral("☁  Hik-Connect"), this);
    m_cloud->setObjectName(QStringLiteral("cloudBtn"));
    m_cloud->setCursor(Qt::PointingHandCursor);
    m_cloud->setToolTip(T("Открыть Hik-Connect"));
    m_cloud->setFixedSize(176, 42);
    m_cloud->setContentsMargins(2, 1, 2, 1);
    QFont cloudFont = m_cloud->font();
    cloudFont.setPointSizeF(10.0);
    m_cloud->setFont(cloudFont);
    m_cloud->setStyleSheet(QStringLiteral("QPushButton { padding: 1px 2px; }"));

    m_mode = new QPushButton(this);
    m_mode->setObjectName(QStringLiteral("modeBtn"));
    m_mode->setCursor(Qt::PointingHandCursor);
    m_mode->setFixedSize(176, 42);
    fitTopBarText(m_mode, m_mode->text());
    m_mode->setContentsMargins(2, 1, 2, 1);
    m_mode->setStyleSheet(QStringLiteral(
        "QPushButton#modeBtn{font-family:'Segoe UI';font-weight:600;"
        "letter-spacing:0.2px;padding:0px 8px;}"));
    m_mode->setToolTip(T("Переключить режим производительности"));

    // Кнопки занимают одинаковые ячейки и выравниваются по нижней линии сетки.
    const int y = height() - 42 - 8;
    int x = 304;
    auto addDraggable = [this, &x, y](QWidget *w, int spacing = 8) {
        const int width = qMax(w->minimumWidth(), w->sizeHint().width());
        registerDraggable(w, QPoint(x, y));
        x += width + spacing;
    };
    addDraggable(m_online);
    addDraggable(m_offline);
    addDraggable(m_status);
    addDraggable(m_fullscreen);
    addDraggable(m_mode);
    // Hik-Connect закреплена в правом углу и не участвует в перетаскивании.

    connect(m_fullscreen, &QPushButton::clicked, this, &TopBar::fullscreenRequested);
    connect(m_status, &QPushButton::clicked, this, &TopBar::statusRequested);
    connect(m_cloud, &QPushButton::clicked, this, &TopBar::cloudRequested);
    connect(m_mode, &QPushButton::clicked, this, [this] { emit simpleModeToggled(!m_simple); });

    m_simple = QSettings().value(QStringLiteral("perf/lowPower"), false).toBool();
    updateModeButton();
    updateStatusVisibility();
    connect(Cfg::notifier(), &CfgNotifier::changed, this, [this] {
        const bool v = QSettings().value(QStringLiteral("perf/lowPower"), false).toBool();
        if (v != m_simple) { m_simple = v; updateModeButton(); }
        updateStatusVisibility();
    });
    connect(m_mgr, &CameraManager::onlineChanged, this, &TopBar::updateCounts);
    connect(m_mgr, &CameraManager::camerasChanged, this, &TopBar::updateCounts);
    updateCounts();
    applyControlPositions();
}

void TopBar::registerDraggable(QWidget *widget, const QPoint &defaultPos)
{
    Q_UNUSED(defaultPos);
    if (!widget) return;
    // Позиции верхней панели теперь управляются единой сеткой. Старые сохранённые
    // координаты не применяются: они могли накладывать ячейки друг на друга.
    m_draggableControls.append(widget);
}

void TopBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyControlPositions();
}

void TopBar::applyControlPositions()
{
    if (!m_title) return;

    constexpr int cellWidth = 176;
    constexpr int cellHeight = 42;
    const int controlCount = m_draggableControls.size();
    // На широком экране оставляем заголовок слева и одну строку из 6 ячеек.
    // На более узких окнах заголовок занимает отдельную строку, а элементы
    // собираются в плотную сетку 3×2 или 2×3 без изменения размеров ячеек.
    const bool singleRow = width() >= 1408;
    const int columns = singleRow ? 5 : (width() >= 560 ? 3 : 2);
    const int rows = qMax(1, (controlCount + columns - 1) / columns);
    const int titleHeight = singleRow ? 0 : 32;
    const int topPadding = singleRow ? (96 - cellHeight) / 2 : titleHeight;
    const int requiredHeight = singleRow ? 96 : titleHeight + rows * cellHeight + 8;
    if (height() != requiredHeight)
        setFixedHeight(requiredHeight);

    const int gridOriginX = singleRow ? 304 : 12;
    if (singleRow)
        m_title->setGeometry(12, 0, 280, height());
    else
        m_title->setGeometry(12, 0, qMax(0, width() - 24), titleHeight);

    for (int i = 0; i < controlCount; ++i) {
        QWidget *w = m_draggableControls.at(i);
        if (!w) continue;
        // Размер каждой ячейки строго сохраняется: 176×42 px.
        if (w->size() != QSize(cellWidth, cellHeight))
            w->setFixedSize(cellWidth, cellHeight);
        const int column = i % columns;
        const int row = i / columns;
        const int px = gridOriginX + column * (cellWidth + 8);
        const int py = topPadding + row * cellHeight;
        w->move(px, py);
        if (w == m_status) {
            if (auto *glyph = w->findChild<QLabel *>(QStringLiteral("cameraStatusGlyph"))) {
                const QFont statusFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold);
                const int textWidth = QFontMetrics(statusFont).horizontalAdvance(m_status->text());
                const int gap = 9;
                const int groupWidth = textWidth + gap + glyph->width();
                const int groupLeft = (w->width() - groupWidth) / 2;
                glyph->move(groupLeft + textWidth + gap,
                            (w->height() - glyph->height()) / 2);
                glyph->raise();
            }
        }
        w->raise();
    }
    if (m_cloud) {
        if (m_cloud->size() != QSize(cellWidth, cellHeight))
            m_cloud->setFixedSize(cellWidth, cellHeight);
        const int cloudX = qMax(8, width() - cellWidth - 8);
        const int cloudY = singleRow ? topPadding : topPadding + (rows - 1) * cellHeight;
        m_cloud->move(cloudX, cloudY);
        m_cloud->raise();
    }
}

bool TopBar::eventFilter(QObject *obj, QEvent *event)
{
    QWidget *w = qobject_cast<QWidget *>(obj);
    if (!w || !m_draggableControls.contains(w)) return QWidget::eventFilter(obj, event);
    if (event->type() == QEvent::MouseButtonPress) {
        auto *e = static_cast<QMouseEvent *>(event);
        if (e->button() == Qt::LeftButton) {
            m_dragCandidate = w; m_draggingControl = nullptr; m_dragMoved = false;
            m_dragOffset = e->globalPosition().toPoint() - w->mapToGlobal(QPoint(0, 0));
            m_dragPressGlobal = e->globalPosition().toPoint();
        }
    } else if (event->type() == QEvent::MouseMove && m_dragCandidate == w) {
        auto *e = static_cast<QMouseEvent *>(event);
        const QPoint global = e->globalPosition().toPoint();
        if (!m_draggingControl && (global - m_dragPressGlobal).manhattanLength() >= QApplication::startDragDistance()) {
            m_draggingControl = w; m_dragMoved = true;
        }
        if (m_draggingControl == w) {
            const int gridOriginX = width() < 720 ? 216 : 304;
            QPoint p = mapFromGlobal(global - m_dragOffset);
            const int maxX = qMax(gridOriginX, width() - w->width() - 8);
            // Перемещение строго по горизонтали; в компактном режиме — по ячейкам сетки.
            p.setX(qBound(gridOriginX, p.x(), maxX));
            const int index = m_draggableControls.indexOf(w);
            const bool narrow = width() < 720;
            const bool compact = width() < 1120;
            const int columns = narrow ? 2 : (compact ? 3 : 6);
            const int buttonWidth = w->width();
            const int row = compact ? index / columns : 0;
            if (compact) {
                const int maxColumn = qMax(0, columns - 1);
                const int snappedColumn = qBound(0, qRound(double(p.x() - gridOriginX) / (buttonWidth + 8)), maxColumn);
                p.setX(gridOriginX + snappedColumn * (buttonWidth + 8));
            } else {
                p.setX(gridOriginX + qRound(double(p.x() - gridOriginX) / 8.0) * 8);
                p.setX(qBound(gridOriginX, p.x(), maxX));
            }
            p.setY(compact ? 5 + row * 44 : (height() - w->height()) / 2);
            w->move(p);
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonRelease && m_dragCandidate == w) {
        const bool dragged = m_draggingControl == w && m_dragMoved;
        if (dragged) {
            const int gridOriginX = width() < 720 ? 216 : 304;
            const int index = m_draggableControls.indexOf(w);
            const bool compact = width() < 1120;
            const bool narrow = width() < 720;
            const int columns = narrow ? 2 : (compact ? 3 : 6);
            const int col = compact ? index % columns : index;
            const int targetX = compact ? gridOriginX + col * (w->width() + 8)
                                        : qBound(gridOriginX, gridOriginX + qRound(double(w->x() - gridOriginX) / 8.0) * 8,
                                                 qMax(gridOriginX, width() - w->width() - 8));
            const int targetY = compact ? 5 + (index / columns) * 44 : (height() - w->height()) / 2;
            const QPoint target(targetX, targetY);
            if (!Cfg::view().animations) {
                w->move(target);
                saveControlPositions();
                m_dragCandidate = nullptr; m_draggingControl = nullptr; m_dragMoved = false;
                return true;
            }
            auto *animation = new QPropertyAnimation(w, "pos", this);
            animation->setDuration(190);
            animation->setStartValue(w->pos());
            animation->setEndValue(target);
            animation->setEasingCurve(QEasingCurve::OutCubic);
            m_positionAnimations.insert(w, animation);
            connect(animation, &QPropertyAnimation::finished, this, [this, animation] {
                saveControlPositions();
                animation->deleteLater();
            });
            animation->start();
        }
        m_dragCandidate = nullptr; m_draggingControl = nullptr; m_dragMoved = false;
        return dragged;
    }
    return QWidget::eventFilter(obj, event);
}

void TopBar::saveControlPositions()
{
    if (width() < 1120) return; // в компактном режиме сетка рассчитывается автоматически
    QSettings settings;
    for (QWidget *w : m_draggableControls) {
        if (!w) continue;
        const QString key = QStringLiteral("ui/topBarPositions/") + w->objectName();
        settings.setValue(key + QStringLiteral("/x"), w->x());
        settings.remove(key + QStringLiteral("/y"));
    }
    settings.sync();
}

void TopBar::setTitle(const QString &title)
{
    m_title->setText(title);
}

void TopBar::updateCounts()
{
    m_online->setText(T("● В сети: %1").arg(m_mgr->onlineCount()));
    m_offline->setText(T("● Не в сети: %1").arg(m_mgr->offlineCount()));
    fitTopBarText(m_online, m_online->text());
    fitTopBarText(m_offline, m_offline->text());
    m_online->setFixedSize(176, 42);
    m_offline->setFixedSize(176, 42);
    m_online->setAlignment(Qt::AlignCenter);
    m_offline->setAlignment(Qt::AlignCenter);
}

void TopBar::updateStatusVisibility()
{
    const bool visible = QSettings().value(QStringLiteral("ui/showCameraStatus"), true).toBool();
    m_online->setVisible(visible);
    m_offline->setVisible(visible);
    m_status->setVisible(visible);
}

void TopBar::updateModeButton()
{
    m_mode->setText(m_simple ? T("Простой режим") : T("Расширенный режим"));
    fitTopBarText(m_mode, m_mode->text());
    m_mode->setProperty("active", m_simple);
    m_mode->style()->unpolish(m_mode);
    m_mode->style()->polish(m_mode);
}

// ======================================================================
