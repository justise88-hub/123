// PSVMS module: Видеоячейки, сетка и видеоповерхность
// This file is included by psvms.cpp to preserve the original single-TU build
// while the code is being separated safely.

// app/video_cell.cpp
// ======================================================================
VideoCell::VideoCell(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("videoCell"));
    setProperty("selected", false);
    setProperty("dropTarget", false);
    setProperty("cellHover", false);
    m_frameGlow = new QGraphicsDropShadowEffect(this);
    m_frameGlow->setOffset(0, 0);
    m_frameGlow->setColor(QColor(245, 183, 0, 120));
    m_frameGlow->setBlurRadius(15);
    setGraphicsEffect(m_frameGlow);
    updateFrameGlow();
    connect(Cfg::notifier(), &CfgNotifier::changed, this, [this] { updateFrameGlow(); });
    setAcceptDrops(true);
    setCursor(Qt::ArrowCursor);

    // Размер ячейки задаёт только сетка. Не позволяем переключению страниц
    // «пусто / видео» менять sizeHint и тем самым сжимать или растягивать соседние ячейки.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setMinimumSize(0, 0);

    // ---- заголовок ----
    m_bar = new QWidget(this);
    m_bar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_bar->setFixedHeight(34);
    m_bar->setObjectName(QStringLiteral("cellBar"));
    m_bar->setAttribute(Qt::WA_AlwaysStackOnTop, true);
    m_bar->setMouseTracking(true);
    auto *barLay = new QHBoxLayout(m_bar);
    barLay->setContentsMargins(0, 0, 0, 0);
    barLay->setSpacing(0);

    m_title = new QLabel(m_bar);
    m_title->setObjectName(QStringLiteral("cellTitle"));
    m_title->setCursor(Qt::OpenHandCursor);
    m_title->setMinimumWidth(0);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_title->setToolTip(T("Перетащите заголовок камеры на другую ячейку: камеры поменяются местами; положение сохранится автоматически"));
    m_title->installEventFilter(this);

    m_liveBadge = new QLabel(m_bar);
    m_liveBadge->setObjectName(QStringLiteral("cellLiveBadge"));
    m_liveBadge->setText(QStringLiteral("● В эфире"));
    m_liveBadge->setAlignment(Qt::AlignCenter);
    m_liveBadge->setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold));
    m_liveBadge->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_liveBadge->setMinimumWidth(84);
    m_liveBadge->setFixedHeight(25);
    m_liveBadge->setToolTip(T("Камера передаёт видеопоток"));
    m_liveBadge->setVisible(false);

    m_audioBtn = new QToolButton(m_bar);
    m_audioBtn->setObjectName(QStringLiteral("cellBtn"));
    m_audioBtn->setAttribute(Qt::WA_Hover, true);
    m_audioBtn->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    m_audioBtn->setAutoRaise(false);
    m_audioBtn->setToolTip(T("Показать регулятор громкости"));
    m_audioBtn->setIconSize(QSize(18, 18));
    // Ползунок — всплывающая панель поверх изображения, а не отдельный элемент строки заголовка.
    m_volumeSlider = new QSlider(Qt::Vertical, this);
    m_volumeSlider->setObjectName(QStringLiteral("cellVolume"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(70);
    m_volumeSlider->setFixedSize(30, 104);
    m_volumeSlider->setCursor(Qt::SizeVerCursor);
    m_volumeSlider->setToolTip(T("Громкость камеры"));
    m_volumeSlider->setStyleSheet(QStringLiteral(
        "QSlider#cellVolume{background:rgba(21,24,30,242);border:1px solid #F5B700;border-radius:6px;padding:5px;}"
        "QSlider#cellVolume::groove:vertical{background:#12151B;border:1px solid #343C4A;width:7px;border-radius:3px;}"
        "QSlider#cellVolume::sub-page:vertical{background:#F5B700;border-radius:3px;}"
        "QSlider#cellVolume::add-page:vertical{background:#1F242D;border-radius:3px;}"
        "QSlider#cellVolume::handle:vertical{background:#FFD54F;border:2px solid #FFFFFF;width:15px;height:15px;margin:-4px -5px;border-radius:8px;}"));
    m_volumeSlider->setVisible(false);
    m_closeBtn = new QToolButton(m_bar);
    m_closeBtn->setObjectName(QStringLiteral("cellBtn"));
    m_closeBtn->setIcon(makeCellIcon(QStringLiteral("close")));
    m_closeBtn->setIconSize(QSize(18, 18));
    m_closeBtn->setToolTip(T("Закрыть камеру"));
    barLay->addWidget(m_title, 1);
    barLay->addWidget(m_liveBadge);
    barLay->addWidget(m_audioBtn);
    barLay->addWidget(m_closeBtn);

    // ---- видео и «пустая» страница ----
    m_surface = new VideoSurface(this);
    // Видеоповерхность не должна диктовать минимальный размер строки сетки.
    m_surface->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_surface->setMinimumSize(0, 0);

    m_idle = new QWidget(this);
    m_idle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_idle->setMinimumSize(0, 0);
    m_idle->setObjectName(QStringLiteral("cellIdle"));
    // Все элементы пустой ячейки размещаются одним overlay-layout.
    // Ручной move() для QLabel здесь ненадёжен: при смене режима размеры
    // QStackedLayout и m_idle обновляются в разные проходы, из-за чего
    // декоративный значок мог оставаться в координатах предыдущей сетки.
    auto *idleLay = new QGridLayout(m_idle);
    idleLay->setContentsMargins(8, 8, 8, 8);
    idleLay->setSpacing(0);
    m_play = new QToolButton(m_idle);
    m_play->setObjectName(QStringLiteral("cellPlay"));
    m_play->setText(QString());
    m_play->setIcon(makeCellIcon(QStringLiteral("play"), QColor(28, 31, 35)));
    m_play->setIconSize(QSize(28, 28));
    m_play->setFixedSize(66, 66);
    m_play->setToolTip(T("Запустить камеру"));
    m_logo = new QLabel(m_idle);
    m_logo->setObjectName(QStringLiteral("emptyCellBrand"));
    m_logo->setAlignment(Qt::AlignCenter);
    m_logo->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // Эмблема занимает всю область пустой страницы как прозрачный overlay.
    // Сам pixmap центрируется средствами QLabel при отрисовке, а не через
    // move()/координаты дочернего QLabel. Это исключает накопление смещения
    // при асинхронном перерасчёте QStackedLayout после смены режима.
    m_logo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_logo->setGeometry(m_idle->rect());
    auto *logoOpacity = new QGraphicsOpacityEffect(m_logo);
    logoOpacity->setOpacity(0.92);
    m_logo->setGraphicsEffect(logoOpacity);
    m_hint = new QLabel(m_idle);
    m_hint->setObjectName(QStringLiteral("placeholder"));
    m_hint->setAlignment(Qt::AlignCenter);
    m_hint->setWordWrap(true);
    m_hint->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // Эмблема намеренно НЕ включена в layout: при смене режима Qt может
    // повторно вычислить геометрию overlay-layout в несколько проходов.
    // Её положение задаётся напрямую относительно m_idle в updateCellArtwork().
    idleLay->addWidget(m_play, 0, 0, Qt::AlignCenter);
    idleLay->addWidget(m_hint, 0, 0, Qt::AlignHCenter | Qt::AlignBottom);

    m_stack = new QStackedLayout;
    // Размер QStackedLayout остаётся независимым от sizeHint текущей страницы.
    m_stack->setSizeConstraint(QLayout::SetNoConstraint);
    m_stack->addWidget(m_idle);
    m_stack->addWidget(m_surface);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(1);
    root->setSizeConstraint(QLayout::SetNoConstraint);
    root->addWidget(m_bar);
    root->addLayout(m_stack, 1);

    // ---- воспроизведение ----
    m_player = new QMediaPlayer(this);
    m_player->setVideoSink(m_surface->sink());

    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    m_retry->setInterval(m_retryDelayMs);
    connect(m_retry, &QTimer::timeout, this, [this] { if (m_started) restart(); });

    // QMediaPlayer может перейти в PlayingState, но не выдать ни одного кадра
    // (зависший RTSP handshake, недоступный substream или проблема декодера).
    // Отсчитываем таймаут до первого реального кадра, а не до PlayingState.
    m_connectTimeout = new QTimer(this);
    m_connectTimeout->setSingleShot(true);
    m_connectTimeout->setInterval(20000);
    connect(m_connectTimeout, &QTimer::timeout, this, [this] {
        if (!m_started || isLocalCapture() || m_firstFrameReceived)
            return;
        const qint64 elapsed = m_connectStartedMs > 0
            ? QDateTime::currentMSecsSinceEpoch() - m_connectStartedMs : 20000;
        writePsvmsLog(QStringLiteral("RTSP: таймаут первого кадра; камера id=%1 host=%2 elapsed=%3 ms; будет переподключение")
            .arg(m_cameraId, m_url.host()).arg(elapsed));
        m_surface->setStatusText(T("Нет видеокадра — повторное подключение…"));
        m_surface->setLinkQuality(VideoSurface::LinkQuality::None);
        scheduleReconnect();
    });
    connect(m_surface->sink(), &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        if (!m_started || isLocalCapture() || !frame.isValid() || m_firstFrameReceived)
            return;
        m_firstFrameReceived = true;
        if (m_connectTimeout) m_connectTimeout->stop();
        const qint64 elapsed = m_connectStartedMs > 0
            ? QDateTime::currentMSecsSinceEpoch() - m_connectStartedMs : -1;
        writePsvmsLog(QStringLiteral("RTSP: получен первый декодированный кадр; камера id=%1 host=%2 connectToFirstFrame=%3 ms")
            .arg(m_cameraId, m_url.host()).arg(elapsed));
        m_retryDelayMs = 3000;
        if (m_retry->isActive()) m_retry->stop();
    });

    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error err) {
        if (!m_started)
            return;
        writePsvmsLog(QStringLiteral("Видеоплеер: ошибка камеры id=%1 host=%2 code=%3")
            .arg(m_cameraId, m_url.host()).arg(int(err)));
        m_surface->setLinkQuality(VideoSurface::LinkQuality::None);
        // AccessDeniedError — камера ответила «неверный логин или пароль»: вместо бесконечных
        // повторов сразу спрашиваем пароль прямо в ячейке
        if (err == QMediaPlayer::AccessDeniedError && !isLocalCapture()) {
            m_retry->stop();
            if (m_connectTimeout) m_connectTimeout->stop();
            m_surface->setStatusText(T("Нужен пароль"));
            showAuthPrompt();
        } else {
            if (m_connectTimeout) m_connectTimeout->stop();
            m_surface->setStatusText(T("Нет видеопотока"));
            scheduleReconnect();
        }
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus st) {
        if (!m_started)
            return;
        if (st == QMediaPlayer::LoadingMedia) {
            m_surface->setStatusText(T("Подключение…"));
            m_surface->setLinkQuality(VideoSurface::LinkQuality::Connecting);
        } else if (st == QMediaPlayer::EndOfMedia || st == QMediaPlayer::InvalidMedia) {
            if (m_connectTimeout) m_connectTimeout->stop();
            m_surface->setStatusText(T("Нет видеопотока"));
            m_surface->setLinkQuality(VideoSurface::LinkQuality::None);
            scheduleReconnect();
        }
    });
    // QMediaPlayer может восстановить RTSP-соединение после краткого обрыва,
    // хотя новый кадр приходит чуть позже. Снимаем устаревшую надпись «Нет сигнала»
    // сразу после перехода плеера в PlayingState; onFrame() уберёт ожидание при
    // поступлении первого декодированного кадра.
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
        if (!m_started)
            return;
        if (state == QMediaPlayer::PlayingState) {
            writePsvmsLog(QStringLiteral("Видеоплеер: RTSP-плеер перешёл в Playing; камера id=%1 host=%2")
                .arg(m_cameraId, m_url.host()));
            // Успешный запуск плеера сбрасывает накопленную задержку.
            m_retryDelayMs = 3000;
            if (m_retry->isActive()) m_retry->stop();
            if (m_surface->currentFrame().isNull()) {
                m_surface->setStatusText(T("Ожидание видеопотока…"));
                m_surface->setLinkQuality(VideoSurface::LinkQuality::Connecting);
            }
        }
    });

    // ---- кнопки и сигналы поверхности ----
    connect(m_closeBtn, &QToolButton::clicked, this, [this] { clear(true); });
    connect(m_audioBtn, &QToolButton::clicked, this, [this] {
        if (m_cameraId.isEmpty()) return;
        m_volumeSlider->setVisible(!m_volumeSlider->isVisible());
        if (m_volumeSlider->isVisible()) {
            m_volumeSlider->move(qMax(6, width() - m_volumeSlider->width() - 14),
                                 (m_bar ? m_bar->height() : 34) + 8);
            m_volumeSlider->raise();
            m_volumeSlider->setFocus();
        }
    });
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        if (!m_audioOut) m_audioOut = new QAudioOutput(this);
        m_audioOut->setVolume(value / 100.0);
        if (value == 0) setMuted(true);
        else if (m_muted) setMuted(false);
        else m_audioOut->setMuted(false);
    });
    connect(m_play, &QToolButton::clicked, this, [this] { startPlayback(); });
    connect(m_surface, &VideoSurface::clicked, this, [this] { emit clicked(this); });
    connect(m_surface, &VideoSurface::doubleClicked, this, [this] { emit doubleClicked(this); });
    connect(m_surface, &VideoSurface::contextMenuRequested, this,
            [this](const QPoint &pos) { emit contextMenuRequested(this, pos); });
    m_idle->installEventFilter(this);
    // Разрешаем перетаскивание не только за заголовок, но и прямо за изображение камеры.
    m_surface->installEventFilter(this);

    setMuted(true);
    clear(false);
    fitHintFont();
    updateCellArtwork();
}

// ---------------------------------------------------------------- назначение камеры

VideoSurface *VideoCell::takeSurfaceForFullscreen()
{
    if (!m_surface)
        return nullptr;
    m_stack->removeWidget(m_surface);
    m_surface->setParent(nullptr);
    return m_surface;
}

void VideoCell::restoreSurfaceFromFullscreen(VideoSurface *surface)
{
    if (!surface || surface != m_surface)
        return;
    surface->setParent(this);
    m_stack->insertWidget(1, surface);
    m_stack->setCurrentWidget(m_started ? m_surface : m_idle);
}

void VideoCell::setCamera(const QString &id, const QString &title, const QUrl &url, bool autoplay,
                          bool showTime, int brightness, bool audio, bool fillCell)
{
    m_cameraId = id;
    m_url = url;
    m_surface->setCameraKey(id);
    setViewOptions(title, showTime, brightness);
    setMuted(!audio);
    setFillCell(fillCell);
    m_audioBtn->setVisible(true);
    m_closeBtn->setVisible(true);
    // Динамик показывается только для назначенной камеры; ползунок открывается по нажатию.
    m_volumeSlider->hide();
    if (autoplay)
        startPlayback();
    else
        stopPlayback();
    emit assignmentChanged();
}

void VideoCell::setViewOptions(const QString &title, bool showTime, int brightness)
{
    m_titleText = title;
    m_title->setText(title);
    m_surface->setShowTime(showTime);
    m_surface->setBrightness(brightness);
    updateIdlePage();
}

void VideoCell::setFillCell(bool on)
{
    if (m_surface) m_surface->setFillCell(on);
}

void VideoCell::startPlayback()
{
    if (m_cameraId.isEmpty())
        return;
    m_retry->stop();
    if (m_connectTimeout) m_connectTimeout->stop();
    m_retryDelayMs = 3000;
    m_firstFrameReceived = false;
    m_connectStartedMs = 0;
    m_started = true;
    writePsvmsLog(QStringLiteral("Видеопоток: запуск камеры id=%1 host=%2")
        .arg(m_cameraId, m_url.host()));
    if (m_liveBadge) {
        m_liveBadge->setText(QStringLiteral("● В эфире"));
        m_liveBadge->setProperty("paused", false);
        m_liveBadge->style()->unpolish(m_liveBadge);
        m_liveBadge->style()->polish(m_liveBadge);
        m_liveBadge->setVisible(true);
    }
    if (m_volumeSlider) m_volumeSlider->hide();
    m_surface->setCameraRunning(true);
    m_stack->setCurrentWidget(m_surface);
    m_surface->setStatusText(T("Подключение…"));
    m_surface->setLinkQuality(VideoSurface::LinkQuality::Connecting);
    restart();
}

void VideoCell::stopPlayback()
{
    m_started = false;
    if (m_liveBadge) {
        m_liveBadge->setText(QStringLiteral("Ⅱ ПАУЗА"));
        m_liveBadge->setProperty("paused", true);
        m_liveBadge->style()->unpolish(m_liveBadge);
        m_liveBadge->style()->polish(m_liveBadge);
        m_liveBadge->setVisible(!m_cameraId.isEmpty());
    }
    if (m_volumeSlider) m_volumeSlider->hide();
    m_surface->setCameraRunning(false);
    m_retry->stop();
    if (m_connectTimeout) m_connectTimeout->stop();
    hideAuthPrompt();
    m_player->stop();
    m_player->setSource(QUrl());
    if (m_camera)
        m_camera->stop();
    m_surface->clearFrame();
    m_stack->setCurrentWidget(m_idle);
    updateIdlePage();
}

void VideoCell::clear(bool notify)
{
    m_started = false;
    if (m_liveBadge) m_liveBadge->setVisible(false);
    if (m_volumeSlider) m_volumeSlider->hide();
    m_surface->setCameraRunning(false);
    m_retry->stop();
    if (m_connectTimeout) m_connectTimeout->stop();
    hideAuthPrompt();
    m_player->stop();
    m_player->setSource(QUrl());
    if (m_camera)
        m_camera->stop();
    m_surface->clearFrame();
    m_surface->setStatusText(QString());
    m_cameraId.clear();
    m_titleText.clear();
    m_url.clear();
    m_title->clear();
    setMuted(true);
    m_audioBtn->setVisible(false);
    m_volumeSlider->hide();
    m_closeBtn->setVisible(false);
    m_stack->setCurrentWidget(m_idle);
    updateIdlePage();
    if (notify)
        emit assignmentChanged();
}

bool VideoCell::isLocalCapture() const
{
    return m_url.scheme() == QLatin1String("qcam");
}

// ---------------------------------------------------------------- пароль прямо в ячейке

void VideoCell::showAuthPrompt()
{
    if (!m_authBox) {
        m_authBox = new QWidget(this);
        m_authBox->setObjectName(QStringLiteral("cellAuthBox"));
        auto *lay = new QVBoxLayout(m_authBox);
        lay->setContentsMargins(14, 12, 14, 12);
        lay->setSpacing(6);

        auto *title = new QLabel(T("Камера требует пароль"), m_authBox);
        title->setObjectName(QStringLiteral("cellAuthTitle"));
        m_authHost = new QLabel(m_authBox);
        m_authHost->setWordWrap(true);

        m_authUser = new QLineEdit(m_authBox);
        m_authUser->setPlaceholderText(T("Логин"));
        QWidget *passRow = passwordFieldWithEye(m_authBox, &m_authPass);
        m_authPass->setPlaceholderText(T("Пароль"));
        connect(m_authPass, &QLineEdit::returnPressed, this, &VideoCell::submitAuthPrompt);
        connect(m_authUser, &QLineEdit::returnPressed, this, [this] { m_authPass->setFocus(); });

        m_authBtn = new QPushButton(T("Подключить"), m_authBox);
        m_authBtn->setObjectName(QStringLiteral("cellAuthBtn"));
        connect(m_authBtn, &QPushButton::clicked, this, &VideoCell::submitAuthPrompt);

        lay->addWidget(title);
        lay->addWidget(m_authHost);
        lay->addWidget(m_authUser);
        lay->addWidget(passRow);
        lay->addWidget(m_authBtn);
        lay->addStretch(1);
    }

    m_authHost->setText(m_url.host() + (m_url.port() > 0 ? QStringLiteral(":%1").arg(m_url.port()) : QString()));
    if (m_authUser->text().isEmpty())
        m_authUser->setText(m_url.userName());
    m_authShown = true;
    m_authBox->setGeometry(0, m_bar->height(), width(), height() - m_bar->height());
    m_authBox->show();
    m_authBox->raise();
    m_authUser->text().isEmpty() ? m_authUser->setFocus() : m_authPass->setFocus();
}

void VideoCell::hideAuthPrompt()
{
    if (m_authBox)
        m_authBox->hide();
    m_authShown = false;
}

void VideoCell::submitAuthPrompt()
{
    if (!m_authBox)
        return;
    QUrl newUrl = m_url;
    newUrl.setUserName(m_authUser->text().trimmed());
    newUrl.setPassword(m_authPass->text());
    hideAuthPrompt();
    m_url = newUrl;
    emit credentialsEntered(this, newUrl);   // пусть владелец сохранит пароль в настройках камеры
    if (m_started)
        restart();
}

void VideoCell::scheduleReconnect()
{
    if (!m_started || !m_retry || m_retry->isActive())
        return;

    writePsvmsLog(QStringLiteral("Видеопоток: переподключение камеры id=%1 через %2 мс")
        .arg(m_cameraId).arg(m_retryDelayMs));
    m_retry->start(m_retryDelayMs);
    m_retryDelayMs = qMin(m_retryDelayMs * 2, 30000);
}

void VideoCell::restart()
{
    hideAuthPrompt();
    // При смене основного/дополнительного потока не оставляем на экране
    // последний кадр старого источника и сбрасываем ограничитель FPS.
    if (m_surface) {
        m_surface->clearFrame();
        m_surface->setStatusText(T("Подключение…"));
        m_surface->setLinkQuality(VideoSurface::LinkQuality::Connecting);
    }
    if (isLocalCapture()) {
        if (m_connectTimeout) m_connectTimeout->stop();
        // Аналоговая камера через карту видеозахвата: обычный QCamera вместо RTSP-плеера
        m_player->stop();
        m_player->setSource(QUrl());
        if (!m_camera) {
            m_camera = new QCamera(this);
            m_capture = new QMediaCaptureSession(this);
            m_capture->setCamera(m_camera);
            m_capture->setVideoSink(m_surface->sink());
            connect(m_camera, &QCamera::activeChanged, this, [this](bool active) {
                if (!active && m_started) {
                    m_surface->setStatusText(T("Нет видеопотока"));
                    m_surface->setLinkQuality(VideoSurface::LinkQuality::None);
                }
            });
            connect(m_camera, &QCamera::errorOccurred, this, [this] {
                m_surface->setStatusText(T("Нет видеопотока"));
                m_surface->setLinkQuality(VideoSurface::LinkQuality::None);
            });
        }
        const QByteArray devId = QByteArray::fromBase64(m_url.path().toLatin1());
        for (const QCameraDevice &d : QMediaDevices::videoInputs()) {
            if (d.id() == devId) {
                m_camera->setCameraDevice(d);
                break;
            }
        }
        m_camera->start();
        return;
    }
    if (m_camera)
        m_camera->stop();
    m_player->stop();
    m_player->setSource(QUrl());
    m_firstFrameReceived = false;
    m_connectStartedMs = QDateTime::currentMSecsSinceEpoch();
    if (m_connectTimeout) m_connectTimeout->start(20000);
    writePsvmsLog(QStringLiteral("Видеопоток: установка RTSP-источника камеры id=%1 host=%2 timeoutFirstFrame=20000ms")
        .arg(m_cameraId, m_url.host()));
    m_player->setSource(m_url);
    m_player->play();
}

void VideoCell::setMuted(bool muted)
{
    m_muted = muted;
    if (muted) {
        m_player->setAudioOutput(nullptr);       // без звука аудиопоток не обрабатывается: меньше нагрузка
    } else {
        if (!m_audioOut)
            m_audioOut = new QAudioOutput(this);
        m_audioOut->setVolume(m_volumeSlider ? m_volumeSlider->value() / 100.0 : 0.70);
        m_audioOut->setMuted(false);
        m_player->setAudioOutput(m_audioOut);
    }
    if (m_volumeSlider) m_volumeSlider->setEnabled(true);
    m_audioBtn->setText(QString());
    m_audioBtn->setIcon(makeCellIcon(muted ? QStringLiteral("mute") : QStringLiteral("sound")));
}

QImage VideoCell::snapshot() const
{
    return m_surface->currentFrame();
}

void VideoCell::resetZoom()
{
    m_surface->resetZoom();
}

void VideoCell::updateFrameGlow()
{
    if (!m_frameGlow)
        return;
    const bool enabled = QSettings().value(QStringLiteral("ui/cameraGlow"), true).toBool();
    m_frameGlow->setEnabled(enabled);
    if (!enabled)
        return;
    const bool selected = property("selected").toBool();
    const bool dropTarget = property("dropTarget").toBool();
    const bool hovered = property("cellHover").toBool();
    const QColor color = dropTarget ? QColor(145, 247, 255, 210)
                       : selected ? QColor(99, 230, 238, 190)
                       : hovered ? QColor(58, 168, 189, 145)
                                 : QColor(45, 145, 170, 75);
    m_frameGlow->setColor(color);
    m_frameGlow->setBlurRadius(dropTarget ? 25 : selected ? 21 : hovered ? 17 : 10);
}

void VideoCell::setSelected(bool on)
{
    setProperty("selected", on);
    style()->unpolish(this);
    style()->polish(this);
    updateFrameGlow();
    update();
}

void VideoCell::refreshCellArtwork()
{
    // При смене простого/расширенного режима Qt может обновить стиль и
    // размеры родительской сетки позже, чем дочерние QLabel. Сначала
    // принудительно активируем оба уровня layout, затем пересчитываем artwork.
    if (layout())
        layout()->activate();
    if (m_stack)
        m_stack->activate();
    if (m_idle)
        m_idle->updateGeometry();
    fitHintFont();
    updateCellArtwork();
}

void VideoCell::setDropTarget(bool on)
{
    setProperty("dropTarget", on);
    style()->unpolish(this);
    style()->polish(this);
    updateFrameGlow();
    update();
}

// ---------------------------------------------------------------- «пустая» страница

void VideoCell::updateIdlePage()
{
    const bool empty = m_cameraId.isEmpty();
    m_play->setVisible(!empty && !m_started);
    if (m_logo)
        m_logo->setVisible(!m_started);
    updateCellArtwork();
    if (empty)
        m_hint->setText(T("Перетащите камеру сюда"));
    else
        m_hint->setText(m_titleText + QStringLiteral("\n") + T("Нажмите ▶, чтобы запустить"));
}

void VideoCell::updateCellArtwork()
{
    if (!m_logo || !m_idle)
        return;
    const int w = m_idle->width();
    const int h = m_idle->height();
    if (w <= 0 || h <= 0)
        return;

    // Размер рисунка сохраняем прежним. Сам QLabel растянут на всю область
    // пустой страницы, а Qt центрирует pixmap внутри его актуального rect.
    // Поэтому координаты значка больше не зависят от move()/старой геометрии
    // QLabel, которая могла переживать переключение simple/advanced mode.
    const int side = qBound(48, qMin(104, qMin((w - 20) / 3, (h - 20) / 3)), 104);
    m_logo->setAlignment(Qt::AlignCenter);
    m_logo->setPixmap(makeCameraCellArtwork(QSize(side, side)));
    const QRect overlayRect = m_idle->rect();
    if (m_logo->geometry() != overlayRect)
        m_logo->setGeometry(overlayRect);
    // Эмблема — нижний прозрачный слой; кнопка и подсказка остаются поверх.
    m_logo->lower();
    m_logo->update();
}

void VideoCell::enterEvent(QEnterEvent *e)
{
    m_hover = true;
    setProperty("cellHover", true);
    style()->unpolish(this);
    style()->polish(this);
    updateFrameGlow();
    update();
    updateIdlePage();
    QFrame::enterEvent(e);
}

void VideoCell::leaveEvent(QEvent *e)
{
    m_hover = false;
    setProperty("cellHover", false);
    style()->unpolish(this);
    style()->polish(this);
    updateFrameGlow();
    update();
    updateIdlePage();
    QFrame::leaveEvent(e);
}

// ---------------------------------------------------------------- вспышка после перестановки

void VideoCell::flash()
{
    if (!m_flash) {
        m_flash = new QLabel(this);
        m_flash->setObjectName(QStringLiteral("cellFlash"));
        m_flash->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_flashFx = new QGraphicsOpacityEffect(m_flash);
        m_flash->setGraphicsEffect(m_flashFx);
    }
    m_flash->setGeometry(rect());
    // В простом режиме отключаем не только плавную анимацию, но и сам
    // декоративный всплеск: он не должен мигать даже кратковременно.
    if (!Cfg::view().animations) {
        m_flash->hide();
        return;
    }
    m_flash->show();
    m_flash->raise();
    auto *a = new QPropertyAnimation(m_flashFx, "opacity", this);
    a->setDuration(620);
    a->setKeyValueAt(0.0, 0.0);
    a->setKeyValueAt(0.16, 0.92);
    a->setKeyValueAt(0.42, 0.18);
    a->setKeyValueAt(0.62, 0.58);
    a->setKeyValueAt(1.0, 0.0);
    a->setEasingCurve(QEasingCurve::OutCubic);
    connect(a, &QPropertyAnimation::finished, m_flash, &QWidget::hide);
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void VideoCell::resizeEvent(QResizeEvent *e)
{
    QFrame::resizeEvent(e);
    // При смене количества ячеек QGridLayout может сначала изменить рамку,
    // а геометрию страниц QStackedLayout пересчитать следующим проходом.
    // Синхронизируем вложенные layouts после завершения текущего resize.
    QPointer<VideoCell> safeThis(this);
    QTimer::singleShot(0, this, [safeThis] {
        if (safeThis)
            safeThis->refreshVideoGeometry();
    });
    if (m_flash && m_flash->isVisible())
        m_flash->setGeometry(rect());
    if (m_volumeSlider && m_volumeSlider->isVisible()) {
        const int x = qMax(6, width() - m_volumeSlider->width() - 14);
        const int y = m_bar ? m_bar->height() + 8 : 42;
        m_volumeSlider->move(x, y);
        m_volumeSlider->raise();
    }
    if (m_authBox && m_authShown)
        m_authBox->setGeometry(0, m_bar->height(), width(), height() - m_bar->height());
    fitHintFont();
    updateCellArtwork();
}

void VideoCell::refreshVideoGeometry()
{
    // Сначала обновляем внешний layout ячейки, затем QStackedLayout с idle/video.
    // Это не трогает поток, кадр, масштаб или привязку камеры — только геометрию.
    if (QLayout *cellLayout = layout()) {
        cellLayout->invalidate();
        cellLayout->activate();
    }
    if (m_stack) {
        m_stack->invalidate();
        m_stack->activate();
    }
    if (m_surface) {
        m_surface->updateGeometry();
        m_surface->update();
    }
    if (m_idle)
        m_idle->updateGeometry();

    // Layout activation above may have changed m_idle geometry. Reposition
    // the decorative camera glyph only after the final geometry is applied.
    updateCellArtwork();
}

// Подпись «Перетащите камеру сюда» подстраивается под размер ячейки: мелкая ячейка — мелкий шрифт,
// крупная — крупнее, в разумных пределах.
void VideoCell::fitHintFont()
{
    const int size = qBound(10, width() / 20, 18);
    QFont f = m_hint->font();
    if (f.pixelSize() != size) {
        f.setPixelSize(size);
        m_hint->setFont(f);
    }
}

// ---------------------------------------------------------------- перетаскивание

QPixmap VideoCell::dragPixmap() const
{
    QPixmap pm(260, 84);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setOpacity(0.92);
    p.setBrush(QColor(0x12, 0x13, 0x16, 210));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(0, 3, 260, 80), 14, 14);
    QLinearGradient shell(0, 2, 0, 82);
    shell.setColorAt(0.0, QColor(0x4a, 0x45, 0x35));
    shell.setColorAt(0.18, QColor(0x25, 0x25, 0x28));
    shell.setColorAt(1.0, QColor(0x12, 0x13, 0x16));
    p.setBrush(shell);
    p.setPen(QPen(QColor(0x28, 0xd5, 0xff), 2));
    p.drawRoundedRect(QRectF(2, 2, 256, 78), 12, 12);
    p.setPen(QPen(QColor(0xa4, 0xff, 0x3b, 150), 1));
    p.drawRoundedRect(QRectF(5, 5, 250, 72), 9, 9);

    const QImage shot = snapshot();
    int textX = 16;
    if (!shot.isNull()) {
        const QImage thumb = shot.scaled(QSize(96, 54), Qt::KeepAspectRatio, Qt::FastTransformation);
        p.drawImage(QPoint(14, 15 + (54 - thumb.height()) / 2), thumb);
        textX = 122;
    }
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(QColor(0x28, 0xd5, 0xff));
    const QString name = QFontMetrics(f).elidedText(m_titleText, Qt::ElideRight, 250 - textX);
    p.drawText(QRect(textX, 16, 250 - textX, 24), Qt::AlignVCenter | Qt::AlignLeft, name);
    f.setBold(false);
    f.setPixelSize(11);
    p.setFont(f);
    p.setPen(QColor(0xb4, 0xb4, 0xb8));
    p.drawText(QRect(textX, 42, 250 - textX, 30), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap,
               T("Перетащите на другую ячейку"));
    return pm;
}

void VideoCell::startDrag()
{
    auto *drag = new QDrag(this);
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(kCellMime), QByteArray::number(qulonglong(reinterpret_cast<quintptr>(this))));
    drag->setMimeData(mime);
    const QPixmap pm = dragPixmap();
    drag->setPixmap(pm);
    drag->setHotSpot(QPoint(pm.width() / 2, pm.height() / 2));
    setProperty("dragSource", true);
    style()->unpolish(this);
    style()->polish(this);
    drag->exec(Qt::MoveAction);
    setProperty("dragSource", false);
    style()->unpolish(this);
    style()->polish(this);
}

bool VideoCell::eventFilter(QObject *obj, QEvent *e)
{
    // Заголовок и само видео — полноценные зоны захвата. На видео не перехватываем
    // обычный клик/двойной клик: ими по-прежнему управляет VideoSurface.
    if (obj == m_title) {
        if (e->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() == Qt::LeftButton) {
                m_dragStart = me->pos();
                emit clicked(this);
            }
        } else if (e->type() == QEvent::MouseMove) {
            auto *me = static_cast<QMouseEvent *>(e);
            if ((me->buttons() & Qt::LeftButton)
                && (me->pos() - m_dragStart).manhattanLength() > QApplication::startDragDistance()) {
                startDrag();
                return true;
            }
        } else if (e->type() == QEvent::MouseButtonDblClick) {
            emit doubleClicked(this);
            return true;
        } else if (e->type() == QEvent::MouseButtonDblClick) {
            emit doubleClicked(this);
            return true;
        }
    } else if (obj == m_surface) {
        if (e->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() == Qt::LeftButton)
                m_dragStart = me->pos();
        } else if (e->type() == QEvent::MouseMove) {
            auto *me = static_cast<QMouseEvent *>(e);
            if ((me->buttons() & Qt::LeftButton)
                && (me->pos() - m_dragStart).manhattanLength() > QApplication::startDragDistance()) {
                startDrag();
                return true;
            }
        }
    } else if (obj == m_idle) {
        if (e->type() == QEvent::Resize) {
            updateCellArtwork();
            // QStackedLayout иногда задаёт окончательный размер страницы
            // следующим проходом после ResizeEvent. Повторяем центрирование
            // после завершения текущего прохода Qt.
            QPointer<VideoCell> safeThis(this);
            QTimer::singleShot(0, this, [safeThis] {
                if (safeThis) safeThis->updateCellArtwork();
            });
        } else if (e->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() == Qt::LeftButton) {
                m_dragStart = me->pos();
                emit clicked(this);
            }
        } else if (e->type() == QEvent::MouseMove) {
            auto *me = static_cast<QMouseEvent *>(e);
            if ((me->buttons() & Qt::LeftButton)
                && (me->pos() - m_dragStart).manhattanLength() > QApplication::startDragDistance()) {
                startDrag();
                return true;
            }
        }
    }
    return QFrame::eventFilter(obj, e);
}

void VideoCell::mousePressEvent(QMouseEvent *e)
{
    emit clicked(this);
    QFrame::mousePressEvent(e);
}

void VideoCell::contextMenuEvent(QContextMenuEvent *e)
{
    emit contextMenuRequested(this, e->globalPos());
    e->accept();
}

void VideoCell::dragEnterEvent(QDragEnterEvent *e)
{
    const QMimeData *m = e->mimeData();
    if (m->hasFormat(QString::fromLatin1(kCameraMime)) || m->hasFormat(QString::fromLatin1(kGroupMime)) || m->hasFormat(QString::fromLatin1(kCellMime))) {
        e->acceptProposedAction();
        setDropTarget(true);
    }
}

void VideoCell::dragLeaveEvent(QDragLeaveEvent *e)
{
    setDropTarget(false);
    QFrame::dragLeaveEvent(e);
}

void VideoCell::dropEvent(QDropEvent *e)
{
    setDropTarget(false);
    const QMimeData *m = e->mimeData();
    if (m->hasFormat(QString::fromLatin1(kCellMime))) {
        const quintptr src = quintptr(m->data(QString::fromLatin1(kCellMime)).toULongLong());
        e->acceptProposedAction();
        if (src != reinterpret_cast<quintptr>(this))
            emit cellDropped(this, src);
    } else if (m->hasFormat(QString::fromLatin1(kGroupMime))) {
        e->acceptProposedAction();
        const QStringList ids = QString::fromUtf8(m->data(QString::fromLatin1(kGroupMime))).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        emit groupDropped(this, ids);
    } else if (m->hasFormat(QString::fromLatin1(kCameraMime))) {
        e->acceptProposedAction();
        emit cameraDropped(this, QString::fromUtf8(m->data(QString::fromLatin1(kCameraMime))));
    }
}

// ======================================================================
// app/video_grid.cpp
// ======================================================================
namespace {

// Переносит логин и пароль из одного адреса в другой (тот же хост, другой путь/канал) —
// используется, когда пароль ввели для одного потока камеры, а применить нужно и ко второму.
QString withSameCredentials(const QString &target, const QUrl &credsFrom)
{
    QUrl u(target);
    if (!u.isValid())
        return target;
    u.setUserName(credsFrom.userName());
    u.setPassword(credsFrom.password());
    return u.toString(QUrl::FullyEncoded);
}

struct GridTemplate {
    QString name;
    int count = 4;
    QStringList cells;
};

QList<GridTemplate> readGridTemplates()
{
    QList<GridTemplate> out;
    QSettings s;
    const int n = s.beginReadArray(QStringLiteral("templates"));
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        GridTemplate t;
        t.name = s.value(QStringLiteral("name")).toString();
        t.count = s.value(QStringLiteral("count"), 4).toInt();
        t.cells = s.value(QStringLiteral("cells")).toStringList();
        if (!t.name.isEmpty())
            out.append(t);
    }
    s.endArray();
    return out;
}

void writeGridTemplates(const QList<GridTemplate> &v)
{
    QSettings s;
    s.remove(QStringLiteral("templates"));
    s.beginWriteArray(QStringLiteral("templates"), int(v.size()));
    for (int i = 0; i < v.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("name"), v.at(i).name);
        s.setValue(QStringLiteral("count"), v.at(i).count);
        s.setValue(QStringLiteral("cells"), v.at(i).cells);
    }
    s.endArray();
}

int gridColumnsFor(int count)
{
    for (const GridLayoutDef &d : VideoGrid::layouts())
        if (d.count == count)
            return d.cols;
    return 2;
}

template <typename F>
QAction *addGridAction(QMenu *menu, const QString &text, QObject *ctx, F fn)
{
    QAction *a = menu->addAction(text);
    QObject::connect(a, &QAction::triggered, ctx, fn);
    return a;
}

} // namespace

QList<GridLayoutDef> VideoGrid::layouts()
{
    // 6, 8, 12 и 20 ячеек удобны для широкоформатных экранов 16:9 и 21:9
    return {{1, 1}, {4, 2}, {6, 3}, {8, 4}, {9, 3}, {12, 4}, {16, 4}, {20, 5}, {25, 5}, {36, 6}, {49, 7}, {64, 8}};
}

QString VideoGrid::layoutLabel(int count)
{
    const int cols = gridColumnsFor(count);
    const int rows = (count + cols - 1) / cols;
    return QStringLiteral("%1 × %2").arg(cols).arg(rows);
}

VideoGrid::VideoGrid(CameraManager *mgr, QWidget *parent) : QWidget(parent), m_mgr(mgr)
{
    m_grid = new QGridLayout(this);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(3);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(400);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { saveLayoutNow(); });

    ensureCells(m_count);
    applyLayout();
    select(m_cells.first());

    connect(m_mgr, &CameraManager::camerasChanged, this, &VideoGrid::refreshCells);
}

void VideoGrid::ensureCells(int n)
{
    while (m_cells.size() < n) {
        auto *cell = new VideoCell(this);
        connect(cell, &VideoCell::clicked, this, &VideoGrid::select);
        connect(cell, &VideoCell::doubleClicked, this, &VideoGrid::openFullscreen);
        connect(cell, &VideoCell::cameraDropped, this, [this](VideoCell *c, const QString &id) {
            if (cameraIds().contains(id)) {
                emit message(T("Устройство уже добавлено в видеосетку."));
                return;
            }
            placeCamera(c, id, true);
            select(c);
            c->flash();
            saveSoon();
        });
        connect(cell, &VideoCell::groupDropped, this, [this](VideoCell *target, const QStringList &ids) {
            int start = m_cells.indexOf(target);
            if (start < 0) return;
            QSet<QString> already;
            for (const QString &id : cameraIds()) if (!id.isEmpty()) already.insert(id);
            int placed = 0, duplicates = 0;
            for (const QString &id : ids) {
                if (already.contains(id)) { ++duplicates; continue; }
                int dst = start + placed;
                while (dst < m_count && !m_cells.at(dst)->isEmpty()) ++dst;
                if (dst >= m_count) break;
                placeCamera(m_cells.at(dst), id, true);
                already.insert(id);
                m_cells.at(dst)->flash();
                ++placed;
            }
            if (duplicates) emit message(T("Часть камер уже размещена в ячейках и была пропущена: %1.").arg(duplicates));
            if (placed) { select(m_cells.at(qMin(start + placed - 1, m_count - 1))); saveLayoutNow(); }
        });
        connect(cell, &VideoCell::cellDropped, this, [this](VideoCell *c, quintptr src) {
            for (VideoCell *o : std::as_const(m_cells)) {
                if (reinterpret_cast<quintptr>(o) == src) {
                    swapCells(o, c);
                    saveLayoutNow();
                    return;
                }
            }
        });
        connect(cell, &VideoCell::contextMenuRequested, this, &VideoGrid::showCellMenu);
        connect(cell, &VideoCell::addRequested, this, [this](VideoCell *c) {
            select(c);
            showAddMenu(c, QCursor::pos());
        });
        connect(cell, &VideoCell::assignmentChanged, this, &VideoGrid::saveSoon);
        connect(cell, &VideoCell::credentialsEntered, this, [this](VideoCell *c, const QUrl &newUrl) {
            const CameraEntry *e = m_mgr->find(c->cameraId());
            if (!e)
                return;
            CameraEntry upd = *e;
            // Логин и пароль относятся к самой камере, поэтому подставляем их в оба адреса —
            // основной поток (запись) и подпоток (мозаика), если он задан
            upd.url = withSameCredentials(e->url, newUrl);
            if (!e->liveUrl.isEmpty())
                upd.liveUrl = withSameCredentials(e->liveUrl, newUrl);
            m_mgr->update(upd);
            emit message(T("Пароль камеры «%1» сохранён").arg(e->name));
        });
        m_cells.append(cell);
    }
}

void VideoGrid::applyLayout()
{
    for (VideoCell *c : std::as_const(m_cells))
        m_grid->removeWidget(c);
    for (int i = 0; i < 10; ++i) {
        m_grid->setRowStretch(i, 0);
        m_grid->setColumnStretch(i, 0);
    }
    const int cols = gridColumnsFor(m_count);
    const int rows = (m_count + cols - 1) / cols;
    for (int i = 0; i < m_cells.size(); ++i) {
        VideoCell *c = m_cells.at(i);
        if (i < m_count) {
            m_grid->addWidget(c, i / cols, i % cols);
            c->show();
        } else {
            c->hide();
        }
    }
    for (int i = 0; i < cols; ++i)
        m_grid->setColumnStretch(i, 1);
    for (int i = 0; i < rows; ++i)
        m_grid->setRowStretch(i, 1);

    // После перестройки сетки Qt применяет геометрию родительских ячеек
    // отложенно. Повторно активируем вложенные layouts на следующем цикле
    // событий, чтобы VideoSurface не оставалась со старым смещением/размером.
    QPointer<VideoGrid> safeGrid(this);
    QTimer::singleShot(0, this, [safeGrid] {
        if (!safeGrid || !safeGrid->m_grid)
            return;
        safeGrid->m_grid->invalidate();
        safeGrid->m_grid->activate();
        for (VideoCell *cell : std::as_const(safeGrid->m_cells)) {
            if (cell && cell->isVisible())
                cell->refreshVideoGeometry();
        }
    });
}

void VideoGrid::setLayoutCount(int n)
{
    bool valid = false;
    for (const GridLayoutDef &d : layouts())
        if (d.count == n)
            valid = true;
    if (!valid)
        return;

    m_count = n;
    ensureCells(n);

    // Не очищаем скрытые ячейки: назначение камеры должно переживать
    // переключение, например, с 4 × 4 на 1 × 1 и обратно.
    // Перед скрытием останавливаем поток, но запоминаем, был ли он запущен.
    for (int i = 0; i < m_cells.size(); ++i) {
        VideoCell *cell = m_cells.at(i);
        if (i >= n) {
            const bool shouldResume = cell->isStarted()
                || cell->property("resumeAfterLayoutHide").toBool();
            cell->setProperty("resumeAfterLayoutHide", shouldResume);
            if (cell->isStarted())
                cell->stopPlayback();
        }
    }

    applyLayout();

    // При возвращении ячейки в видимую область автоматически возобновляем
    // только те камеры, которые работали до временного скрытия.
    for (int i = 0; i < qMin(n, m_cells.size()); ++i) {
        VideoCell *cell = m_cells.at(i);
        const bool resume = cell->property("resumeAfterLayoutHide").toBool();
        if (resume && !cell->isEmpty()) {
            cell->setProperty("resumeAfterLayoutHide", false);
            cell->startPlayback();
        }
    }
    if (m_selected && m_cells.indexOf(m_selected) >= n)
        select(m_cells.first());
    refreshCells();                              // в раскладке 1 × 1 камера переключается на основной поток
    emit layoutCountChanged(n);
    saveSoon();
}

void VideoGrid::select(VideoCell *cell)
{
    if (m_selected == cell)
        return;
    if (m_selected)
        m_selected->setSelected(false);
    m_selected = cell;
    if (m_selected)
        m_selected->setSelected(true);
    emit selectionChanged(cell ? cell->cameraId() : QString());
}

QStringList VideoGrid::cameraIds() const
{
    QStringList ids;
    for (int i = 0; i < m_count && i < m_cells.size(); ++i)
        ids << m_cells.at(i)->cameraId();
    return ids;
}

// Основной поток или подпоток — по настройке качества камеры; «авто»: подпоток в мозаике, основной в 1 × 1
QUrl VideoGrid::urlFor(const CameraEntry &e, const VideoCell *) const
{
    bool wantMain;
    if (e.viewQuality == 1)
        wantMain = true;
    else if (e.viewQuality == 2)
        wantMain = false;
    else
        wantMain = (m_count == 1);
    return QUrl((wantMain || e.liveUrl.isEmpty()) ? e.url : e.liveUrl);
}

void VideoGrid::placeCamera(VideoCell *cell, const QString &cameraId, bool autoplay)
{
    const CameraEntry *e = m_mgr->find(cameraId);
    if (!e || !cell)
        return;
    // Защита на уровне общей операции назначения: один ID не может оказаться
    // одновременно в двух ячейках, независимо от способа добавления.
    for (VideoCell *other : std::as_const(m_cells)) {
        if (other != cell && other->cameraId() == cameraId) {
            emit message(T("Устройство уже добавлено в другую ячейку."));
            return;
        }
    }
    cell->setCamera(e->id, e->name, urlFor(*e, cell), autoplay, e->showTime, e->brightness, e->audio, e->fillCell);
}

void VideoGrid::openCamera(const QString &cameraId)
{
    VideoCell *target = nullptr;
    if (m_selected && m_selected->isVisible() && m_selected->isEmpty())
        target = m_selected;
    if (!target) {
        for (VideoCell *c : std::as_const(m_cells)) {
            if (c->isVisible() && c->isEmpty()) {
                target = c;
                break;
            }
        }
    }
    if (!target)
        target = (m_selected && m_selected->isVisible()) ? m_selected : m_cells.first();

    placeCamera(target, cameraId, true);

    // выделяем следующую свободную ячейку, чтобы можно было открывать камеры подряд
    VideoCell *next = target;
    for (VideoCell *c : std::as_const(m_cells)) {
        if (c->isVisible() && c->isEmpty()) {
            next = c;
            break;
        }
    }
    select(next);
}

void VideoGrid::clearAll()
{
    for (VideoCell *c : std::as_const(m_cells))
        c->clear(false);
    saveSoon();
}

void VideoGrid::startAllCells()
{
    for (int i = 0; i < m_count && i < m_cells.size(); ++i)
        if (!m_cells.at(i)->isEmpty())
            m_cells.at(i)->startPlayback();
}

void VideoGrid::stopAllCells()
{
    for (int i = 0; i < m_count && i < m_cells.size(); ++i)
        if (!m_cells.at(i)->isEmpty())
            m_cells.at(i)->stopPlayback();
}

void VideoGrid::refreshCells()
{
    for (VideoCell *c : std::as_const(m_cells)) {
        if (c->isEmpty())
            continue;
        const CameraEntry *e = m_mgr->find(c->cameraId());
        if (!e) {
            c->clear(false);                       // камера удалена
            saveSoon();
        } else if (urlFor(*e, c) != c->url()) {
            placeCamera(c, e->id, c->isStarted());  // сменился адрес или нужный поток
        } else {
            c->setViewOptions(e->name, e->showTime, e->brightness);
            c->setFillCell(e->fillCell);
        }
    }
}

// Обмен камер местами: ячейки подсвечиваются вспышкой
void VideoGrid::swapCells(VideoCell *a, VideoCell *b)
{
    const QString ia = a->cameraId(), ib = b->cameraId();
    const bool pa = a->isStarted(), pb = b->isStarted();
    a->clear(false);
    b->clear(false);
    if (!ib.isEmpty())
        placeCamera(a, ib, pb);
    if (!ia.isEmpty())
        placeCamera(b, ia, pa);
    a->flash();
    b->flash();
    select(b);
    // Перестановка фиксируется на диске сразу: перезапуск потока или приложения
    // не должен менять назначение камер ячейкам.
    saveLayoutNow();
}

// ---------------------------------------------------------------- шаблоны

void VideoGrid::saveSoon()
{
    if (!m_restoring)
        m_saveTimer->start();
}

void VideoGrid::saveLayoutNow()
{
    QSettings s;
    s.setValue(QStringLiteral("layout/count"), m_count);
    // Сохраняем все созданные ячейки, включая временно скрытые в простом режиме.
    // Иначе после перезапуска восстановится только первая страница сетки.
    QStringList allCellIds;
    allCellIds.reserve(m_cells.size());
    for (VideoCell *cell : std::as_const(m_cells))
        allCellIds << cell->cameraId();
    s.setValue(QStringLiteral("layout/cells"), allCellIds);
    s.sync(); // записать перестановку на диск сразу после debounce-сохранения
}

void VideoGrid::applyTemplateData(int count, const QStringList &ids, bool autoplay)
{
    m_restoring = true;                              // промежуточные состояния не сохраняем
    for (VideoCell *c : std::as_const(m_cells))
        c->clear(false);
    bool valid = false;
    for (const GridLayoutDef &d : layouts())
        if (d.count == count)
            valid = true;
    setLayoutCount(valid ? count : 4);
    // В файле настроек могут находиться назначения ячеек, скрытых текущим
    // простым режимом. Создаём их заранее и восстанавливаем без запуска потока.
    ensureCells(qMin(ids.size(), 64));
    QList<QPair<QPointer<VideoCell>, QString>> pendingStarts;
    for (int i = 0; i < ids.size() && i < m_cells.size(); ++i) {
        if (ids.at(i).isEmpty() || !m_mgr->find(ids.at(i))) continue;
        VideoCell *cell = m_cells.at(i);
        placeCamera(cell, ids.at(i), false);
        if (autoplay && i < m_count)
            pendingStarts.append(qMakePair(QPointer<VideoCell>(cell), ids.at(i)));
    }
    m_restoring = false;
    // Восстановление раскладки не должно создавать десятки RTSP-сессий
    // одновременно: запускаем их с небольшим интервалом.
    for (int i = 0; i < pendingStarts.size(); ++i) {
        const auto pending = pendingStarts.at(i);
        QTimer::singleShot(i * 400, this, [pending] {
            if (pending.first && !pending.first->isEmpty()
                && pending.first->cameraId() == pending.second)
                pending.first->startPlayback();
        });
    }
    saveSoon();
}

void VideoGrid::restoreLayout()
{
    QSettings s;
    const int count = s.value(QStringLiteral("layout/count"), 4).toInt();
    const QStringList ids = s.value(QStringLiteral("layout/cells")).toStringList();
    // при отключённом автозапуске камеры занимают свои ячейки, но не стартуют (кнопка ▶)
    applyTemplateData(count, ids, Cfg::autostartCameras());
}

QStringList VideoGrid::templateNames() const
{
    QStringList names;
    for (const GridTemplate &t : readGridTemplates())
        names << t.name;
    return names;
}

void VideoGrid::saveTemplate(const QString &name)
{
    QList<GridTemplate> all = readGridTemplates();
    GridTemplate t;
    t.name = name;
    t.count = m_count;
    t.cells = cameraIds();
    bool replaced = false;
    for (GridTemplate &x : all) {
        if (x.name == name) {
            x = t;
            replaced = true;
        }
    }
    if (!replaced)
        all.append(t);
    writeGridTemplates(all);
    emit message(T("Шаблон «%1» сохранён").arg(name));
}

void VideoGrid::applyTemplateNamed(const QString &name)
{
    for (const GridTemplate &t : readGridTemplates()) {
        if (t.name == name) {
            applyTemplateData(t.count, t.cells, true);
            emit message(T("Применён шаблон «%1»").arg(name));
            return;
        }
    }
}

void VideoGrid::removeTemplate(const QString &name)
{
    QList<GridTemplate> all = readGridTemplates();
    for (int i = 0; i < all.size(); ++i) {
        if (all.at(i).name == name) {
            all.removeAt(i);
            break;
        }
    }
    writeGridTemplates(all);
}

// ---------------------------------------------------------------- меню

void VideoGrid::showAddMenu(VideoCell *cell, const QPoint &globalPos)
{
    QMenu menu(this);
    const QStringList shown = cameraIds();
    QMap<QString, QList<const CameraEntry *>> byGroup;
    for (const CameraEntry &e : m_mgr->cameras())
        byGroup[e.group].append(&e);

    QPointer<VideoCell> pc(cell);
    auto addCam = [&](QMenu *m, const CameraEntry *e) {
        const QString id = e->id;
        const QString text = (shown.contains(id) ? QStringLiteral("✓ ") : QString()) + e->name;
        addGridAction(m, text, this, [this, id, pc] {
            if (!pc)
                return;
            placeCamera(pc, id, true);
            select(pc);
            pc->flash();
        });
    };
    for (auto it = byGroup.begin(); it != byGroup.end(); ++it) {
        if (it.key().isEmpty())
            continue;
        QMenu *sub = menu.addMenu(it.key());
        for (const CameraEntry *e : std::as_const(it.value()))
            addCam(sub, e);
    }
    for (const CameraEntry *e : byGroup.value(QString()))
        addCam(&menu, e);
    if (m_mgr->cameras().isEmpty())
        menu.addAction(T("Камер пока нет"))->setEnabled(false);

    menu.addSeparator();
    addGridAction(&menu, T("Добавить новую камеру…"), this, [this] { emit addCameraRequested(); });
    menu.exec(globalPos);
}

void VideoGrid::showCellMenu(VideoCell *cell, const QPoint &globalPos)
{
    if (cell->isEmpty()) {
        select(cell);
        showAddMenu(cell, globalPos);
        return;
    }
    const CameraEntry *found = m_mgr->find(cell->cameraId());
    if (!found)
        return;
    select(cell);
    const CameraEntry entry = *found;             // копия: список камер может измениться, пока открыто меню
    const QString id = entry.id;
    QPointer<VideoCell> pc(cell);

    QMenu menu(this);
    menu.addAction(entry.name)->setEnabled(false);
    menu.addSeparator();

    addGridAction(&menu, T("Настройки камеры…"), this, [this, id] { editCamera(id); });

    // качество картинки
    QMenu *quality = menu.addMenu(T("Качество картинки"));
    auto *qGroup = new QActionGroup(quality);
    const QString qNames[3] = {T("Авто (подпоток в мозаике)"), T("Высокое (основной поток)"), T("Низкое (подпоток, экономия)")};
    for (int q = 0; q < 3; ++q) {
        QAction *a = addGridAction(quality, qNames[q], this, [this, id, q] {
            if (const CameraEntry *e = m_mgr->find(id)) {
                CameraEntry c = *e;
                c.viewQuality = q;
                m_mgr->update(c);
            }
        });
        a->setCheckable(true);
        a->setChecked(entry.viewQuality == q);
        qGroup->addAction(a);
    }

    // поток для мозаики
    const QList<StreamOption> options = streamOptionsFor(entry.url);
    QMenu *streams = menu.addMenu(T("Поток для мозаики"));
    auto *sGroup = new QActionGroup(streams);
    auto addStream = [&](const QString &text, const QString &url) {
        QAction *a = addGridAction(streams, text, this, [this, id, url] { m_mgr->setLiveUrl(id, url); });
        a->setCheckable(true);
        a->setChecked(url == entry.liveUrl);
        sGroup->addAction(a);
    };
    addStream(T("Как у записи (без подпотока)"), QString());
    bool known = entry.liveUrl.isEmpty();
    for (const StreamOption &o : options) {
        addStream(o.label, o.url);
        if (o.url == entry.liveUrl)
            known = true;
    }
    if (!known)
        addStream(T("Текущий подпоток"), entry.liveUrl);

    // Масштабирование: режим заполнения не зависит от выбранного видеопотока.
    QAction *fill = addGridAction(&menu, T("Заполнить всю ячейку (обрезать края)"), this, [this, id] {
        if (const CameraEntry *e = m_mgr->find(id)) {
            CameraEntry c = *e;
            c.fillCell = !c.fillCell;
            m_mgr->update(c);
        }
    });
    fill->setCheckable(true);
    fill->setChecked(entry.fillCell);
    addGridAction(&menu, T("Применить заполнение ко всем камерам"), this, [this] {
        for (const CameraEntry &e : m_mgr->cameras()) {
            CameraEntry c = e;
            c.fillCell = true;
            m_mgr->update(c);
        }
    });
    addGridAction(&menu, T("Отключить заполнение у всех камер"), this, [this] {
        for (const CameraEntry &e : m_mgr->cameras()) {
            CameraEntry c = e;
            c.fillCell = false;
            m_mgr->update(c);
        }
    });

    // звук и время
    QAction *sound = addGridAction(&menu, T("Звук"), this, [pc] { if (pc) pc->setMuted(!pc->isMuted()); });
    sound->setCheckable(true);
    sound->setChecked(!cell->isMuted());
    QAction *time = addGridAction(&menu, T("Дата и время на картинке"), this, [this, id] {
        if (const CameraEntry *e = m_mgr->find(id)) {
            CameraEntry c = *e;
            c.showTime = !c.showTime;
            m_mgr->update(c);
        }
    });
    time->setCheckable(true);
    time->setChecked(entry.showTime);

    QAction *fps = addGridAction(&menu, T("Показывать FPS в ячейках камер"), this, [] {
        QSettings settings;
        const bool enabled = settings.value(QStringLiteral("view/showFps"), true).toBool();
        settings.setValue(QStringLiteral("view/showFps"), !enabled);
        Cfg::reload();
    });
    fps->setCheckable(true);
    fps->setChecked(Cfg::view().showFps);
    fps->setToolTip(T("Включает или отключает индикатор фактической частоты входящих кадров во всех ячейках"));

    menu.addSeparator();
    addGridAction(&menu, T("Снимок экрана"), this, [this, pc] { if (pc) saveSnapshot(pc); });
    addGridAction(&menu, T("На весь экран"), this, [this, pc] { if (pc) openFullscreen(pc); });
    addGridAction(&menu, T("Сбросить масштаб"), this, [pc] { if (pc) pc->resetZoom(); });

    menu.addSeparator();
    addGridAction(&menu, T("Архив камеры…"), this, [this, id] { emit archiveRequested(id); });
    if (m_mgr->isStarted(id))
        addGridAction(&menu, T("Остановить запись"), this, [this, id] { m_mgr->stopCamera(id); });
    else
        addGridAction(&menu, T("Запустить запись"), this, [this, id] { m_mgr->startCamera(id); });
    if (cell->isStarted())
        addGridAction(&menu, T("Остановить показ"), this, [pc] { if (pc) pc->stopPlayback(); });
    else
        addGridAction(&menu, T("Запустить показ"), this, [pc] { if (pc) pc->startPlayback(); });

    menu.addSeparator();
    addGridAction(&menu, T("Закрыть камеру"), this, [pc] { if (pc) pc->clear(true); });

    menu.exec(globalPos);
}

void VideoGrid::editCamera(const QString &cameraId)
{
    const CameraEntry *e = m_mgr->find(cameraId);
    if (!e)
        return;
    QStringList groups;
    for (const CameraEntry &c : m_mgr->cameras())
        if (!c.group.isEmpty() && !groups.contains(c.group))
            groups << c.group;
    CameraDialog dlg(*e, groups, window());
    if (dlg.exec() == QDialog::Accepted)
        m_mgr->update(dlg.entry());
}

void VideoGrid::saveSnapshot(VideoCell *cell)
{
    const QImage img = cell->snapshot();
    if (img.isNull()) {
        emit message(T("Нет кадра для снимка"));
        return;
    }
    const CameraEntry *e = m_mgr->find(cell->cameraId());
    QString safe = e ? e->name : QStringLiteral("camera");
    safe.replace(QRegularExpression(QStringLiteral("[^\\w\\- .]")), QStringLiteral("_"));

    QDir dir(Cfg::screenshotDir());
    dir.mkpath(QStringLiteral("."));
    const QString file = dir.filePath(QStringLiteral("%1_%2.png")
                                          .arg(safe, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    if (img.save(file, "PNG"))
        emit message(T("Снимок сохранён: %1").arg(QDir::toNativeSeparators(file)));
    else
        emit message(T("Не удалось сохранить снимок: %1").arg(QDir::toNativeSeparators(file)));
}

// Двойной щелчок: камера на весь экран (основной поток)
void VideoGrid::openFullscreen(VideoCell *cell)
{
    if (!cell || cell->isEmpty())
        return;
    const QString cameraId = cell->cameraId();
    const CameraEntry *e = m_mgr->find(cameraId);
    if (!e)
        return;

    // Smart Auto mode: use the main stream in a fullscreen view, then return
    // to the lighter substream when the view is closed and the grid is restored.
    const bool switchToMainForFullscreen = e->viewQuality == 0
        && !e->liveUrl.isEmpty() && m_count > 1;
    const QString title = e->name;
    if (switchToMainForFullscreen) {
        cell->setCamera(e->id, e->name, QUrl(e->url), true,
                        e->showTime, e->brightness, e->audio, e->fillCell);
    }

    auto *fs = new FullscreenView(cell);
    fs->setWindowTitle(title);
    if (switchToMainForFullscreen) {
        QPointer<VideoCell> safeCell(cell);
        connect(fs, &QObject::destroyed, this, [this, safeCell, cameraId] {
            if (!safeCell)
                return;
            const CameraEntry *current = m_mgr->find(cameraId);
            if (!current || safeCell->cameraId() != cameraId)
                return;
            safeCell->setCamera(current->id, current->name, urlFor(*current, safeCell), true,
                                current->showTime, current->brightness,
                                current->audio, current->fillCell);
        });
    }
    fs->openOnScreenOf(window());
}

void VideoGrid::closeSelectedCell()
{
    if (m_selected && !m_selected->isEmpty())
        m_selected->clear(true);
}

QString VideoGrid::selectedCameraId() const
{
    return m_selected ? m_selected->cameraId() : QString();
}

void VideoGrid::flushLayout()
{
    if (m_saveTimer->isActive())
        m_saveTimer->stop();
    saveLayoutNow();
}

// ======================================================================
// app/video_surface.cpp
// ======================================================================
CameraHoverPreview::CameraHoverPreview(QTableWidget *table, CameraManager *manager, QObject *parent)
    : QObject(parent), m_table(table), m_manager(manager)
{
    if (!table) return;
    m_delay = new QTimer(this);
    m_delay->setSingleShot(true);
    m_delay->setInterval(350);
    connect(m_delay, &QTimer::timeout, this, &CameraHoverPreview::showPreview);

    m_popup = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    m_popup->setAttribute(Qt::WA_ShowWithoutActivating);
    m_popup->setAttribute(Qt::WA_DeleteOnClose, false);
    m_popup->setObjectName(QStringLiteral("cameraHoverPreview"));
    m_popup->setStyleSheet(QStringLiteral("#cameraHoverPreview { background:#171b20; border:1px solid #c6a52b; border-radius:7px; }"));
    auto *layout = new QVBoxLayout(m_popup);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(6);
    m_video = new VideoSurface(m_popup);
    m_video->setMinimumSize(320, 180);
    m_video->setMaximumSize(400, 225);
    m_video->setStatusText(T("Предпросмотр камеры"));
    m_info = new QLabel(m_popup);
    m_info->setWordWrap(true);
    m_info->setStyleSheet(QStringLiteral("color:#e7e9ec; padding:3px 5px;"));
    layout->addWidget(m_video);
    layout->addWidget(m_info);

    m_player = new QMediaPlayer(m_popup);
    m_audio = new QAudioOutput(m_popup);
    m_audio->setMuted(true);
    m_player->setAudioOutput(m_audio);
    m_player->setVideoOutput(m_video->sink());
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &error) {
        if (m_video && !error.isEmpty() && m_currentId.size())
            m_video->setStatusText(T("Нет изображения: %1").arg(error));
    });

    table->viewport()->installEventFilter(this);
}

CameraHoverPreview::~CameraHoverPreview()
{
    if (m_player) m_player->stop();
    delete m_popup;
    m_popup = nullptr;
}

bool CameraHoverPreview::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_table || watched != m_table->viewport())
        return QObject::eventFilter(watched, event);
    if (event->type() == QEvent::MouseMove) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        const QModelIndex index = m_table->indexAt(mouse->pos());
        if (index.isValid() && index.column() == 0) {
            const QTableWidgetItem *item = m_table->item(index.row(), 0);
            const QString id = item ? item->data(Qt::UserRole).toString() : QString();
            if (!id.isEmpty())
                schedule(id, m_table->viewport()->mapToGlobal(mouse->pos()));
            else
                hidePreview();
        } else {
            hidePreview();
        }
    } else if (event->type() == QEvent::Leave) {
        hidePreview();
    } else if (event->type() == QEvent::Hide) {
        hidePreview();
    }
    return QObject::eventFilter(watched, event);
}

void CameraHoverPreview::schedule(const QString &cameraId, const QPoint &globalPos)
{
    m_globalPos = globalPos;
    if (cameraId == m_currentId && m_popup->isVisible())
        return;
    if (cameraId != m_pendingId) {
        m_pendingId = cameraId;
        m_delay->start();
    }
}

void CameraHoverPreview::showPreview()
{
    if (!m_manager || !m_popup || m_pendingId.isEmpty()) return;
    const CameraEntry *entry = m_manager->find(m_pendingId);
    if (!entry) return;
    m_currentId = entry->id;
    const QString stream = entry->liveUrl.trimmed().isEmpty() ? entry->url : entry->liveUrl;
    m_player->stop();
    m_video->clearFrame();
    m_video->setStatusText(T("Подключение к камере…"));
    m_player->setSource(QUrl(stream));
    m_player->play();

    const QString link = m_manager->isOnline(entry->id) ? T("В сети") : T("Не в сети");
    const QString record = m_manager->isStarted(entry->id) ? stateText(m_manager->stateOf(entry->id)) : T("Не запущена");
    const QString safeUrl = QUrl(entry->url).toString(QUrl::RemoveUserInfo);
    m_info->setText(QStringLiteral("<b>%1</b><br>%2<br>%3: %4<br>%5: %6")
        .arg(entry->name.toHtmlEscaped(), entry->group.toHtmlEscaped(), T("Адрес"), safeUrl.toHtmlEscaped(),
             T("Связь / запись"), (link + QStringLiteral(" / ") + record).toHtmlEscaped()));
    QPoint pos = m_globalPos + QPoint(18, 18);
    m_popup->adjustSize();
    m_popup->move(pos);
    m_popup->show();
}

void CameraHoverPreview::hidePreview()
{
    if (m_delay) m_delay->stop();
    m_pendingId.clear();
    m_currentId.clear();
    if (m_player) m_player->stop();
    if (m_video) m_video->clearFrame();
    if (m_popup) m_popup->hide();
}

VideoSurface::VideoSurface(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(40, 30);

    m_sink = new QVideoSink(this);
    connect(m_sink, &QVideoSink::videoFrameChanged, this, &VideoSurface::onFrame);

    updateInterval();
    connect(Cfg::notifier(), &CfgNotifier::changed, this, [this] {
        updateInterval();
        update();
    });

    // часы на картинке обновляются раз в секунду, даже если кадры не приходят
    m_clock = new QTimer(this);
    m_clock->setInterval(1000);
    connect(m_clock, &QTimer::timeout, this, [this] {
        if (m_showTime && Cfg::view().timestamp && isVisible())
            update();
    });
    m_clock->start();
}

void VideoSurface::updateInterval()
{
    m_minIntervalMs = 1000 / qMax(1, Cfg::view().maxFps);
}

void VideoSurface::setShowTime(bool on)
{
    m_showTime = on;
    update();
}

void VideoSurface::setBrightness(int percent)
{
    m_brightness = qBound(-50, percent, 50);
    update();
}

void VideoSurface::setFillCell(bool on)
{
    if (m_fillCell == on) return;
    m_fillCell = on;
    update();
}

void VideoSurface::setStatusText(const QString &text)
{
    m_status = text;
    update();
}

void VideoSurface::setCameraRunning(bool running)
{
    if (m_cameraRunning == running)
        return;
    m_cameraRunning = running;
    update();
}

void VideoSurface::setLinkQuality(LinkQuality q)
{
    if (m_quality == q)
        return;
    m_quality = q;
    update();
}

void VideoSurface::setCameraKey(const QString &cameraId)
{
    m_cameraKey = cameraId;
    const QRectF saved = QSettings().value(QStringLiteral("cameraZoom/") + m_cameraKey).toRectF();
    if (!m_cameraKey.isEmpty() && saved.isValid() && saved.width() > 0.0
        && saved.height() > 0.0 && saved.width() <= 1.0 && saved.height() <= 1.0
        && saved.x() >= 0.0 && saved.y() >= 0.0
        && saved.right() <= 1.0001 && saved.bottom() <= 1.0001) {
        m_view = saved;
    } else {
        m_view = QRectF(0.0, 0.0, 1.0, 1.0);
    }
    update();
}

void VideoSurface::resetZoom()
{
    m_view = QRectF(0.0, 0.0, 1.0, 1.0);
    if (!m_cameraKey.isEmpty()) {
        QSettings settings;
        settings.remove(QStringLiteral("cameraZoom/") + m_cameraKey);
        settings.sync();
    }
    update();
}

void VideoSurface::clearFrame()
{
    m_frame = QImage();
    m_view = QRectF(0.0, 0.0, 1.0, 1.0);
    m_lastFrameMs = 0;
    m_fps = 0;
    m_fpsFrameCount = 0;
    m_fpsWindowStartMs = 0;
    setLinkQuality(LinkQuality::None);
    update();
}

void VideoSurface::onFrame(const QVideoFrame &frame)
{
    // Лишние кадры отбрасываем до дорогого преобразования в картинку
    if (!frame.isValid() || !isVisible())
        return;
    const QWidget *w = window();
    if (w && w->isMinimized())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    setLinkQuality(LinkQuality::Good);   // кадр от камеры пришёл — связь есть, даже если он будет пропущен ниже
    if (!m_status.isEmpty()) {
        m_status.clear();
        update();
    }

    // Lightweight FPS counter for the bottom-left camera overlay only.
    if (m_fpsWindowStartMs == 0)
        m_fpsWindowStartMs = now;
    ++m_fpsFrameCount;
    if (now - m_fpsWindowStartMs >= 1000) {
        m_fps = m_fpsFrameCount;
        m_fpsFrameCount = 0;
        m_fpsWindowStartMs = now;
        update();
    }

    if (now - m_lastFrameMs < m_minIntervalMs)
        return;                          // отрисовываем реже, чем приходят кадры (экономия для слабых ПК)
    // QVideoFrame::toImage() возвращает самостоятельный QImage. Qt использует
    // implicit sharing, поэтому дополнительный .copy() здесь только дублировал
    // весь кадр в памяти на каждом FPS и создавал лишнюю нагрузку на CPU/память.
    QImage img = frame.toImage();
    if (img.isNull())
        return;
    if (img.format() != QImage::Format_ARGB32_Premultiplied)
        img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (img.isNull())
        return;

    // Убираем гребёнку у явно помеченных чересстрочных кадров: используем одно
    // поле и восстанавливаем пропущенные строки интерполяцией соседних строк.
    // Qt 6.8 QVideoFrame does not expose fieldType()/InterlacedFrame.
    // Keep the converted frame unchanged; this avoids calling an API unavailable in Qt 6.
    m_lastFrameMs = now;
    m_frame = std::move(img);
    m_status.clear();
    update();
}

QRectF VideoSurface::targetRect() const
{
    QSizeF s = QSizeF(m_frame.size());
    s.scale(QSizeF(size()), m_fillCell ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
    return QRectF((width() - s.width()) / 2.0, (height() - s.height()) / 2.0, s.width(), s.height());
}

void VideoSurface::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0x12, 0x12, 0x13));
    const Cfg::View &v = Cfg::view();

    if (!m_frame.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, v.smooth);
        const QRectF dst = targetRect();
        const QSizeF fs(m_frame.size());
        const QRectF src(m_view.x() * fs.width(), m_view.y() * fs.height(),
                         m_view.width() * fs.width(), m_view.height() * fs.height());
        p.drawImage(dst, m_frame, src);

        if (m_brightness != 0) {
            const int level = qAbs(m_brightness) * 255 / 100;
            if (m_brightness > 0) {
                p.setCompositionMode(QPainter::CompositionMode_Plus);
                p.fillRect(dst, QColor(level, level, level));
            } else {
                p.setCompositionMode(QPainter::CompositionMode_Multiply);
                p.fillRect(dst, QColor(255 - level, 255 - level, 255 - level));
            }
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
    } else if (!m_status.isEmpty()) {
        p.setPen(QColor(0x6a, 0x6a, 0x6e));
        p.drawText(rect(), Qt::AlignCenter, m_status);
    }

    QFont f = p.font();
    f.setPixelSize(12);
    f.setBold(true);
    p.setFont(f);

    // дата и время поверх картинки
    if (m_showTime && v.timestamp && width() >= 120) {
        const QString t = QDateTime::currentDateTime().toString(v.timeFormat);
        const QFontMetrics fm(f);
        const QSize ts = fm.size(Qt::TextSingleLine, t) + QSize(16, 8);
        const QRect box(width() - ts.width() - 8, height() - ts.height() - 8, ts.width(), ts.height());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(box, 4, 4);
        p.setPen(QColor(0xf2, 0xf2, 0xf2));
        p.drawText(box, Qt::AlignCenter, t);
    }

    // Разрешение и качество соединения — отдельный нижний левый индикатор.
    if (v.showInfo && width() >= 140) {
        QColor dot;
        switch (m_quality) {
        case LinkQuality::Good:       dot = QColor(0x3f, 0xb9, 0x50); break;
        case LinkQuality::Connecting: dot = QColor(0xe8, 0x83, 0x3a); break;
        case LinkQuality::None:       dot = QColor(0xd9, 0x53, 0x4f); break;
        }
        const QString info = m_frame.isNull()
            ? T("Нет видеосигнала")
            : QStringLiteral("%1×%2").arg(m_frame.width()).arg(m_frame.height());
        const QFontMetrics fm(f);
        const int textW = fm.horizontalAdvance(info);
        const int boxW = 12 + 10 + 8 + textW + 8;
        const QRect box(8, height() - 32, boxW, 24);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 165));
        p.drawRoundedRect(box, 4, 4);
        p.setBrush(dot);
        p.drawEllipse(QPointF(box.left() + 14, box.center().y()), 5, 5);
        p.setPen(QColor(0xf2, 0xf2, 0xf2));
        p.drawText(QRect(box.left() + 26, box.top(), textW + 4, box.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, info);
    }

    // FPS — независимый индикатор в левом верхнем углу: его можно скрыть,
    // не отключая разрешение и статус соединения. Считаются входящие кадры потока.
    if (v.showFps && width() >= 100 && height() >= 50 && m_cameraRunning) {
        const QString fpsText = m_fps > 0
            ? QStringLiteral("%1 FPS").arg(m_fps)
            : QStringLiteral("… FPS");
        const int fpsW = QFontMetrics(f).horizontalAdvance(fpsText) + 22;
        const QRect fpsBox(8, 8, fpsW, 24);
        p.setPen(QPen(QColor(0x75, 0xa8, 0x35, 220), 1));
        p.setBrush(QColor(8, 16, 6, 190));
        p.drawRoundedRect(fpsBox, 5, 5);
        p.setPen(QColor(0xd7, 0xff, 0x70));
        p.drawText(fpsBox, Qt::AlignCenter, fpsText);
    }

    // Небольшой фирменный индикатор работающей камеры — всегда в правом верхнем углу.
    // Рисуется на поверхности видео, поэтому остаётся на месте и в полноэкранном режиме.
    if (m_cameraRunning && width() >= 48 && height() >= 36) {
        const QRect badge(width() - 42, 8, 34, 27);
        p.setPen(QPen(QColor(0x28, 0xc8, 0xff, 230), 1));
        p.setBrush(QColor(24, 25, 27, 205));
        p.drawRoundedRect(badge, 5, 5);

        // Камера: серо-стальной корпус с яркими голубыми неоновыми гранями.
        const qreal cx = badge.center().x();
        const qreal cy = badge.center().y();
        p.setPen(QPen(QColor(0x8c, 0xf2, 0xff), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(QColor(0x08, 0x5d, 0x8b));
        QPainterPath body;
        body.moveTo(cx - 9, cy - 5);
        body.lineTo(cx + 3, cy - 5);
        body.lineTo(cx + 6, cy - 2);
        body.lineTo(cx + 6, cy + 5);
        body.lineTo(cx - 9, cy + 5);
        body.closeSubpath();
        p.drawPath(body);
        QPolygonF lens;
        lens << QPointF(cx + 6, cy - 3) << QPointF(cx + 11, cy - 6)
             << QPointF(cx + 11, cy + 6) << QPointF(cx + 6, cy + 3);
        p.setBrush(QColor(0x28, 0xd5, 0xff));
        p.drawPolygon(lens);
        p.setPen(QPen(QColor(0x05, 0x2a, 0x45), 1));
        p.setBrush(QColor(0x04, 0x1c, 0x30));
        p.drawEllipse(QPointF(cx - 3, cy), 2.2, 2.2);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x28, 0xd5, 0xff));
        p.drawEllipse(QPointF(badge.right() - 5, badge.top() + 5), 2.1, 2.1);
    }

    // значок масштаба — под индикатором камеры, чтобы элементы не перекрывались
    if (m_view.width() < 0.999) {
        const QString z = QStringLiteral("×%1").arg(1.0 / m_view.width(), 0, 'f', 1);
        const QRect box(width() - 62, 40, 54, 22);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x8c, 0xff, 0x00, 220));
        p.drawRoundedRect(box, 4, 4);
        p.setPen(QColor(0x1a, 0x1a, 0x1c));
        p.drawText(box, Qt::AlignCenter, z);
    }
}

void VideoSurface::wheelEvent(QWheelEvent *e)
{
    const double steps = e->angleDelta().y() / 120.0;
    if (m_frame.isNull() || steps == 0.0) {
        e->ignore();
        return;
    }
    const QRectF dst = targetRect();
    const QPointF pos = e->position();
    const double fx = qBound(0.0, (pos.x() - dst.x()) / dst.width(), 1.0);
    const double fy = qBound(0.0, (pos.y() - dst.y()) / dst.height(), 1.0);
    const double cx = m_view.x() + fx * m_view.width();     // точка кадра под курсором
    const double cy = m_view.y() + fy * m_view.height();

    double zoom = 1.0 / m_view.width();
    zoom = qBound(1.0, zoom * std::pow(1.2, steps), 8.0);
    const double w = 1.0 / zoom;
    const double x = qBound(0.0, cx - fx * w, 1.0 - w);     // точка под курсором остаётся на месте
    const double y = qBound(0.0, cy - fy * w, 1.0 - w);
    m_view = QRectF(x, y, w, w);
    if (!m_cameraKey.isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("cameraZoom/") + m_cameraKey, m_view);
        settings.sync();
    }
    update();
    e->accept();
}

void VideoSurface::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        emit clicked();
        if (m_view.width() < 0.999) {
            m_panning = true;
            m_panStart = e->pos();
            m_panView = m_view;
            setCursor(Qt::ClosedHandCursor);
        }
    } else if (e->button() == Qt::MiddleButton) {
        resetZoom();
    }
    QWidget::mousePressEvent(e);
}

void VideoSurface::mouseMoveEvent(QMouseEvent *e)
{
    if (m_panning && !m_frame.isNull()) {
        const QRectF dst = targetRect();
        const double dx = (e->pos().x() - m_panStart.x()) / dst.width() * m_panView.width();
        const double dy = (e->pos().y() - m_panStart.y()) / dst.height() * m_panView.height();
        const double w = m_panView.width();
        m_view = QRectF(qBound(0.0, m_panView.x() - dx, 1.0 - w), qBound(0.0, m_panView.y() - dy, 1.0 - w), w, w);
        // Сохраняем не только коэффициент увеличения, но и смещение кадра
        // после перемещения мышью. Настройки привязаны к ID камеры.
        if (!m_cameraKey.isEmpty()) {
            QSettings settings;
            settings.setValue(QStringLiteral("cameraZoom/") + m_cameraKey, m_view);
            settings.sync();
        }
        update();
    }
    QWidget::mouseMoveEvent(e);
}

void VideoSurface::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_panning) {
        m_panning = false;
        unsetCursor();
    }
    QWidget::mouseReleaseEvent(e);
}

void VideoSurface::mouseDoubleClickEvent(QMouseEvent *e)
{
    emit doubleClicked();
    QWidget::mouseDoubleClickEvent(e);
}

void VideoSurface::contextMenuEvent(QContextMenuEvent *e)
{
    emit contextMenuRequested(e->globalPos());
}

// ======================================================================
