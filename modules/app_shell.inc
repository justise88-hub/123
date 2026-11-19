// PSVMS module: Безопасность, вход, главное окно и импорт ONVIF
// This file is included by psvms.cpp to preserve the original single-TU build
// while the code is being separated safely.

// app/security_audit.cpp
// ======================================================================
namespace SecurityAudit {
void write(const QString &event, const QString &details)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    QFile f(dir + QStringLiteral("/security-audit.log"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    const QString safe = details; // caller must never pass credentials or tokens
    const QByteArray line = QStringLiteral("%1 | user=%2 | %3 | %4\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
             Users::current().name.isEmpty() ? QStringLiteral("anonymous") : Users::current().name,
             event, safe).toUtf8();
    f.write(line);
    f.close();
}
}

// app/login_dialog.cpp
// ======================================================================
namespace {

// Защита от подбора: после 5 неверных попыток вход блокируется на 30 секунд и дольше
int g_loginFailures = 0;
qint64 g_loginLockedUntilMs = 0;

QLabel *loginMessageLabel(QWidget *parent)
{
    auto *l = new QLabel(parent);
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color:#F5B700;"));
    return l;
}

class LoginDialog : public QDialog {
public:
    explicit LoginDialog(QWidget *parent) : QDialog(parent)
    {
        // Диалог входа временный: его закрытие не должно завершать QApplication
        // до того, как main() создаст и покажет MainWindow.
        setAttribute(Qt::WA_QuitOnClose, false);
        setWindowTitle(App::title() + QStringLiteral(" — ") + T("вход"));
        setObjectName(QStringLiteral("loginDialog"));
        setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        setMinimumWidth(500);
        // Убираем системную белую подложку на незаполненных участках диалога.
        setAutoFillBackground(true);
        QPalette loginPalette = palette();
        loginPalette.setColor(QPalette::Window, QColor(QStringLiteral("#0D0F12")));
        loginPalette.setColor(QPalette::Base, QColor(QStringLiteral("#1A1F28")));
        loginPalette.setColor(QPalette::Button, QColor(QStringLiteral("#1D222C")));
        loginPalette.setColor(QPalette::WindowText, QColor(QStringLiteral("#ECF0F5")));
        setPalette(loginPalette);
        setStyleSheet(QStringLiteral(R"LOGIN(
            QDialog#loginDialog { background:#0D0F12; color:#ECF0F5; font-family:"Segoe UI", sans-serif; }
            QDialog#loginDialog QWidget { color:#ECF0F5; background:transparent; }
            QDialog#loginDialog QLabel { color:#C8D0DC; background:transparent; }
            QDialog#loginDialog QFrame { background:transparent; }
            QFrame#loginHeader {
                background:#141820;
                border:1px solid #2A303C; border-left:4px solid #F5B700; border-radius:4px;
            }
            QFrame#loginHeader QLabel { color:#ECF0F5; }
            QLabel#loginEyebrow { color:#F5B700; font-family:"Bahnschrift", "Segoe UI", sans-serif; font-size:9pt; font-weight:700; letter-spacing:1.1px; }
            QLabel#loginTitle { color:#FFFFFF; font-family:"Segoe UI", sans-serif; font-size:11pt; font-weight:700; }
            QLabel#loginSubtitle { color:#9AA4B5; font-family:"Segoe UI", sans-serif; font-size:9.5pt; }
            QFrame#loginCard {
                background:#141820; border:1px solid #2A303C; border-radius:4px;
            }
            QLabel#loginSectionTitle { color:#ECF0F5; font-size:9.5pt; font-weight:700; letter-spacing:0.2px; }
            QLabel#loginSectionHint { color:#8D97A8; font-size:9pt; }
            QDialog#loginDialog QComboBox, QDialog#loginDialog QLineEdit#passwordInput {
                color:#ECF0F5; background:#101318; border:1px solid #2E3645;
                border-radius:4px; padding:6px 10px; min-height:34px; font-size:9.5pt;
                selection-background-color:#F5B700; selection-color:#111317;
            }
            QDialog#loginDialog QComboBox:hover, QDialog#loginDialog QLineEdit#passwordInput:focus {
                border:1px solid #F5B700; background:#151922;
            }
            QDialog#loginDialog QComboBox QAbstractItemView {
                color:#ECF0F5; background:#171B23; selection-color:#111317;
                selection-background-color:#F5B700; border:1px solid #323B4B;
                outline:0; padding:4px;
            }
            QDialog#loginDialog QFormLayout QLabel { color:#9AA4B5; font-weight:600; font-size:9.5pt; }
            QDialog#loginDialog QCheckBox { color:#C8D0DC; spacing:8px; font-size:9.5pt; }
            QDialog#loginDialog QCheckBox::indicator {
                width:16px; height:16px; border-radius:3px;
                background:#101318; border:1px solid #303847;
            }
            QDialog#loginDialog QCheckBox::indicator:checked {
                background:#F5B700; border:1px solid #F5B700;
            }
            QLabel#loginMessage { color:#F5B700; font-size:9pt; font-weight:600; padding:2px 2px; }
            QDialog#loginDialog QDialogButtonBox QPushButton {
                color:#D6DCE6; background:#1C212B; border:1px solid #303847;
                border-radius:4px; padding:6px 18px; min-width:98px; min-height:32px;
                font-family:"Segoe UI", sans-serif; font-size:9.5pt; font-weight:600;
            }
            QDialog#loginDialog QPushButton#loginPrimaryButton {
                color:#111317; background:#F5B700; border:1px solid #F5B700; font-weight:700;
            }
            QDialog#loginDialog QPushButton#loginPrimaryButton:hover {
                background:#FFD54F; border-color:#FFD54F;
            }
            QDialog#loginDialog QDialogButtonBox QPushButton:hover {
                color:#FFFFFF; background:#262D3A; border-color:#F5B700;
            }
        )LOGIN"));

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(22, 20, 22, 18);
        root->setSpacing(14);

        auto *header = new QFrame(this);
        header->setObjectName(QStringLiteral("loginHeader"));
        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(18, 15, 18, 15);
        headerLayout->setSpacing(14);

        auto *brandMark = new QLabel(QStringLiteral("P"), header);
        brandMark->setAlignment(Qt::AlignCenter);
        brandMark->setFixedSize(48, 48);
        brandMark->setStyleSheet(QStringLiteral(
            "background:#1A1F28; color:#F5B700; border:1px solid #F5B700;"
            "border-radius:4px; font-family:'Bahnschrift','Segoe UI',sans-serif; font-size:18pt; font-weight:700;"));
        auto *headerText = new QVBoxLayout;
        headerText->setSpacing(3);
        auto *eyebrow = new QLabel(T("СИСТЕМА ВИДЕОНАБЛЮДЕНИЯ"), header);
        eyebrow->setObjectName(QStringLiteral("loginEyebrow"));
        auto *title = new QLabel(T("PSVms — Вход в систему"), header);
        title->setObjectName(QStringLiteral("loginTitle"));
        auto *subtitle = new QLabel(T("Выберите учётную запись для начала работы"), header);
        subtitle->setObjectName(QStringLiteral("loginSubtitle"));
        subtitle->setWordWrap(true);
        headerText->addWidget(eyebrow);
        headerText->addWidget(title);
        headerText->addWidget(subtitle);
        headerLayout->addWidget(brandMark, 0, Qt::AlignVCenter);
        headerLayout->addLayout(headerText, 1);
        root->addWidget(header);

        auto *card = new QFrame(this);
        card->setObjectName(QStringLiteral("loginCard"));
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(18, 17, 18, 16);
        cardLayout->setSpacing(12);
        auto *sectionTitle = new QLabel(T("Вход в систему"), card);
        sectionTitle->setObjectName(QStringLiteral("loginSectionTitle"));
        auto *hint = new QLabel(T("Программа запускается в гостевом режиме. Выберите Гостя или другую учётную запись."), card);
        hint->setObjectName(QStringLiteral("loginSectionHint"));
        hint->setWordWrap(true);
        cardLayout->addWidget(sectionTitle);
        cardLayout->addWidget(hint);

        m_user = new QComboBox(card);
        m_user->setMinimumHeight(42);
        for (const UserInfo &u : Users::all()) {
            if (!u.enabled)
                continue; // отключённые учётные записи нельзя выбрать для входа
            const QString role = u.isDeveloper() ? T("разработчик")
                               : u.isGuest() ? T("гость")
                               : u.isAdmin() ? T("администратор") : T("оператор");
            m_user->addItem(u.name + QStringLiteral("   ·   ") + role, u.id);
        }
        if (m_user->count() == 0)
            m_user->setEnabled(false);
        // При смене пользователя показываем полный список и предварительно
        // выделяем последнюю учётную запись; пользователь может выбрать любую другую.
        const QString lastId = Users::lastUserId();
        int preferredIndex = -1;
        for (int i = 0; i < m_user->count(); ++i) {
            if (m_user->itemData(i).toString() == lastId) {
                preferredIndex = i;
                break;
            }
        }
        if (preferredIndex < 0) {
            for (int i = 0; i < m_user->count(); ++i) {
                if (Users::find(m_user->itemData(i).toString()).isGuest()) {
                    preferredIndex = i;
                    break;
                }
            }
        }
        if (preferredIndex >= 0)
            m_user->setCurrentIndex(preferredIndex);

        m_passRow = passwordFieldWithEye(card, &m_pass);
        m_remember = new QCheckBox(T("Запомнить меня на этом компьютере"), card);
        m_msg = loginMessageLabel(card);
        m_msg->setObjectName(QStringLiteral("loginMessage"));
        if (m_user->count() == 0)
            m_msg->setText(T("Нет активных учётных записей. Обратитесь к администратору."));

        m_form = new QFormLayout;
        m_form->setContentsMargins(0, 4, 0, 0);
        m_form->setHorizontalSpacing(14);
        m_form->setVerticalSpacing(11);
        m_form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_form->addRow(T("Пользователь"), m_user);
        m_form->addRow(T("Пароль"), m_passRow);
        m_form->addRow(QString(), m_remember);
        cardLayout->addLayout(m_form);
        cardLayout->addWidget(m_msg);
        root->addWidget(card);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        auto *loginButton = buttons->button(QDialogButtonBox::Ok);
        loginButton->setText(T("Войти"));
        loginButton->setObjectName(QStringLiteral("loginPrimaryButton"));
        buttons->button(QDialogButtonBox::Cancel)->setText(T("Выход"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(buttons);

        connect(m_user, &QComboBox::currentIndexChanged, this, [this] { updatePasswordRow(); });
        updatePasswordRow();
        resize(540, sizeHint().height());
    }

    void accept() override
    {
        const QString id = m_user->currentData().toString();
        const UserInfo u = Users::find(id);
        if (!u.valid() || !u.enabled) {
            m_msg->setText(T("Эта учётная запись отключена."));
            return;
        }

        if (u.hasPassword) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            QSettings security;
            const QString key = QStringLiteral("security/login/%1/").arg(id);
            const int failures = security.value(key + QStringLiteral("failures"), 0).toInt();
            const qint64 lockedUntil = security.value(key + QStringLiteral("lockedUntil"), 0).toLongLong();
            if (now < lockedUntil) {
                m_msg->setText(T("Слишком много попыток. Подождите %1 с.").arg((lockedUntil - now + 999) / 1000));
                SecurityAudit::write(QStringLiteral("login_blocked"), id);
                return;
            }
            if (!Users::verify(id, m_pass->text())) {
                const int nextFailures = failures + 1;
                const qint64 delay = nextFailures >= 5 ? qMin<qint64>(900000, 30000LL << qMin(4, nextFailures - 5)) : 0;
                security.setValue(key + QStringLiteral("failures"), nextFailures);
                if (delay) security.setValue(key + QStringLiteral("lockedUntil"), now + delay);
                security.sync();
                SecurityAudit::write(QStringLiteral("login_failed"), id);
                m_msg->setText(delay ? T("Неверный пароль. Вход временно заблокирован.") : T("Неверный пароль."));
                m_pass->clear();
                m_pass->setFocus();
                return;
            }
            security.remove(key);
            SecurityAudit::write(QStringLiteral("login_success"), id);
        }
        g_loginFailures = 0;
        Users::setCurrent(id);
        Users::setLastUserId(id);
        if (u.hasPassword && m_remember->isChecked())
            Users::issueRememberToken(id);       // ключ входа на этом компьютере (пароль не сохраняется)
        else
            Users::forgetRemembered();
        QDialog::accept();
    }

private:
    void updatePasswordRow()
    {
        const UserInfo u = Users::find(m_user->currentData().toString());
        m_form->setRowVisible(m_passRow, u.hasPassword);
        m_form->setRowVisible(m_remember, u.hasPassword);
        m_msg->clear();
        if (u.hasPassword)
            m_pass->setFocus();
    }

    QComboBox *m_user = nullptr;
    QLineEdit *m_pass = nullptr;
    QWidget *m_passRow = nullptr;
    QCheckBox *m_remember = nullptr;
    QLabel *m_msg = nullptr;
    QFormLayout *m_form = nullptr;
};

class ChangePasswordDialog : public QDialog {
public:
    explicit ChangePasswordDialog(QWidget *parent) : QDialog(parent)
    {
        // Настройка пароля также является промежуточным окном при первом входе.
        setAttribute(Qt::WA_QuitOnClose, false);
        setWindowTitle(T("Смена пароля"));
        setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        setMinimumWidth(420);

        m_has = Users::current().hasPassword;
        QWidget *oldRow = passwordFieldWithEye(this, &m_old);
        QWidget *newRow = passwordFieldWithEye(this, &m_new);
        QWidget *new2Row = passwordFieldWithEye(this, &m_new2);
        m_msg = loginMessageLabel(this);

        auto *hint = new QLabel(T("Оставьте новый пароль пустым, чтобы входить без пароля."), this);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color:#8a8a8e;"));

        auto *form = new QFormLayout;
        if (m_has)
            form->addRow(T("Текущий пароль"), oldRow);
        form->addRow(T("Новый пароль"), newRow);
        form->addRow(T("Повторите новый"), new2Row);

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
        const QString id = Users::current().id;
        if (m_has && !Users::verify(id, m_old->text())) {
            m_msg->setText(T("Текущий пароль указан неверно."));
            return;
        }
        if (m_new->text() != m_new2->text()) {
            m_msg->setText(T("Новые пароли не совпадают."));
            return;
        }
        QString err;
        if (!Users::setPassword(id, m_new->text(), &err)) {
            m_msg->setText(err);
            return;
        }
        Users::forgetRemembered();
        QDialog::accept();
    }

private:
    bool m_has = false;
    QLineEdit *m_old = nullptr;
    QLineEdit *m_new = nullptr;
    QLineEdit *m_new2 = nullptr;
    QLabel *m_msg = nullptr;
};

} // namespace

bool authenticate(QWidget *parent)
{
    Users::ensureDefault();
    Users::setCurrent(QString());
    LoginDialog dlg(parent);
    dlg.setWindowIcon(QIcon(QStringLiteral(":/branding/taskbar.png")));
    if (dlg.exec() != QDialog::Accepted)
        return false;
    const UserInfo selected = Users::current();
    if (!selected.valid() || !selected.enabled)
        return false;
    SecurityAudit::write(QStringLiteral("session_started"), selected.id);
    writePsvmsLog(QStringLiteral("Авторизация: пользователь выбран в диалоге = %1").arg(selected.name));
    return true;
}

void changeOwnPassword(QWidget *parent)
{
    ChangePasswordDialog dlg(parent);
    if (dlg.exec() == QDialog::Accepted)
        QMessageBox::information(parent, T("Смена пароля"), T("Пароль изменён."));
}

// ======================================================================
// app/main_window.cpp
// ======================================================================
namespace {

enum MainPage { PageLive = 0, PageDevices = 1, PageEvents = 2, PageSettings = 3 };

struct MainWinSize {
    int w;
    int h;
};
const MainWinSize kMainWinSizes[] = {{0, 0},       {1280, 720},  {1600, 900},  {1920, 1080},
                                     {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};

class ButtonMotionFilter final : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject *obj, QEvent *event) override
    {
        auto *w = qobject_cast<QWidget *>(obj);
        if (!w || !(qobject_cast<QPushButton *>(w) || qobject_cast<QToolButton *>(w)))
            return QObject::eventFilter(obj, event);

        qreal target = 1.0;
        int duration = 130;
        switch (event->type()) {
        case QEvent::Enter: target = 0.96; duration = 150; break;
        case QEvent::Leave: target = 1.0; duration = 170; break;
        case QEvent::MouseButtonPress: target = 0.86; duration = 75; break;
        case QEvent::MouseButtonRelease: target = 0.98; duration = 145; break;
        default: return QObject::eventFilter(obj, event);
        }

        // В простом режиме не создаём даже нулевые анимации: Qt-анимации
        // продолжают жить и потреблять события, даже если их duration == 0.
        if (!Cfg::view().animations) {
            if (auto *effect = qobject_cast<QGraphicsOpacityEffect *>(w->graphicsEffect()))
                effect->setOpacity(1.0);
            return QObject::eventFilter(obj, event);
        }

        auto *effect = qobject_cast<QGraphicsOpacityEffect *>(w->graphicsEffect());
        if (!effect) {
            effect = new QGraphicsOpacityEffect(w);
            effect->setOpacity(1.0);
            w->setGraphicsEffect(effect);
        }
        auto *animation = new QPropertyAnimation(effect, "opacity", this);
        animation->setDuration(duration);
        animation->setStartValue(effect->opacity());
        animation->setEndValue(target);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        animation->start(QAbstractAnimation::DeleteWhenStopped);
        return QObject::eventFilter(obj, event);
    }
};

} // namespace

static QPixmap makePsvmsBrandWallpaper();

class PsvmsBackgroundWidget final : public QWidget
{
public:
    explicit PsvmsBackgroundWidget(QWidget *parent = nullptr)
        : QWidget(parent), m_wallpaper(makePsvmsBrandWallpaper())
    {
        setObjectName(QStringLiteral("centralWidget"));
        setAttribute(Qt::WA_StyledBackground, true);
    }
protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        rebuildWallpaperCache();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        if (m_wallpaper.isNull()) { p.fillRect(rect(), QColor("#090a0c")); return; }
        if (!m_wallpaperCache.isNull())
            p.drawPixmap(m_wallpaperTarget, m_wallpaperCache);
        p.fillRect(rect(), QColor(5, 6, 8, 58));
    }

private:
    void rebuildWallpaperCache()
    {
        if (m_wallpaper.isNull() || size().isEmpty()) return;
        const QSize scaledSize = m_wallpaper.size().scaled(size(), Qt::KeepAspectRatioByExpanding);
        m_wallpaperCache = m_wallpaper.scaled(scaledSize, Qt::KeepAspectRatioByExpanding,
                                               Qt::FastTransformation);
        m_wallpaperTarget = QRect((width() - m_wallpaperCache.width()) / 2,
                                  (height() - m_wallpaperCache.height()) / 2,
                                  m_wallpaperCache.width(), m_wallpaperCache.height());
        update();
    }

    QPixmap m_wallpaper;
    QPixmap m_wallpaperCache;
    QRect m_wallpaperTarget;
};


namespace {
QWidget *buildAiHomePanel(QWidget *parent)
{
    auto *panel = new QFrame(parent);
    panel->setObjectName(QStringLiteral("aiHomePanel"));
    panel->setMinimumWidth(250);
    panel->setMaximumWidth(310);
    panel->setStyleSheet(QStringLiteral(
        "QFrame#aiHomePanel{background-color:#141820;background-image:url(:/branding/ai_background.png);"
        "background-position:center;background-repeat:no-repeat;border:1px solid #2A303C;"
        "border-radius:4px;} QLabel{color:#ECF0F5;background:rgba(13,15,18,175);} "
        "QPushButton{background:#1D222C;color:#E4EAF2;border:1px solid #303847;"
        "border-radius:4px;padding:8px 12px;text-align:left;font-weight:600;}"
        "QPushButton:hover{background:#262D3A;border:1px solid #F5B700;color:#FFFFFF;}"
        "QPushButton:pressed{background:#F5B700;border:1px solid #F5B700;color:#111317;font-weight:700;}"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto *heading = new QLabel(T("ИИ-ПОМОЩНИК"), panel);
    heading->setStyleSheet(QStringLiteral("font-family:'Bahnschrift','Segoe UI',sans-serif;font-size:10.5pt;font-weight:700;color:#F5B700;"));
    heading->setAlignment(Qt::AlignCenter);
    layout->addWidget(heading);

    auto *avatar = new QLabel(panel);
    avatar->setAlignment(Qt::AlignCenter);
    QPixmap avatarPixmap;
    const QString avatarPath = QStringLiteral(":/branding/avatar.png");
    avatarPixmap.load(avatarPath, "PNG");
    if (avatarPixmap.isNull()) {
        avatarPixmap = QPixmap(192, 192);
        avatarPixmap.fill(QColor("#202328"));
        QPainter avatarPainter(&avatarPixmap);
        avatarPainter.setRenderHint(QPainter::Antialiasing, true);
        avatarPainter.setPen(QPen(QColor("#F5B700"), 4));
        avatarPainter.setBrush(Qt::NoBrush);
        avatarPainter.drawEllipse(QRect(4, 4, 184, 184));
        avatarPainter.setPen(QColor("#F5B700"));
        avatarPainter.setFont(QFont(QStringLiteral("Segoe UI"), 54, QFont::Bold));
        avatarPainter.drawText(avatarPixmap.rect(), Qt::AlignCenter, QStringLiteral("AI"));
    }
    avatar->setPixmap(avatarPixmap.scaled(112, 112, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    avatar->setMinimumHeight(116);
    layout->addWidget(avatar);

    auto *intro = new QLabel(T("Привет! Я помогу контролировать камеры, искать события и решать технические проблемы PSVMS."), panel);
    intro->setWordWrap(true);
    intro->setAlignment(Qt::AlignCenter);
    intro->setStyleSheet(QStringLiteral("font-size:9.5pt;color:#C8D0DC;padding:6px;background:rgba(13,15,18,195);border-radius:4px;"));
    layout->addWidget(intro);

    auto addAction = [panel, layout](const QString &caption, const QString &prompt) {
        auto *button = new QPushButton(caption, panel);
        layout->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, panel, [panel, prompt] {
            panel->setProperty("psvms.aiPrompt", prompt);
            openYandexAssistant(panel);
        });
    };
    addAction(T("▥  Статистика камер"), T("Помоги оценить состояние камер и объясни, какие показатели нужно проверить."));
    addAction(T("⌕  Поиск событий"), T("Как быстро найти нужное событие в журнале PSVMS?"));
    addAction(T("✓  Проверка системы"), T("Дай пошаговый список проверки работоспособности системы видеонаблюдения."));
    addAction(T("?  Помощь по настройке"), T("Помоги настроить IP-камеру в PSVMS. С чего начать?"));

    auto *openChat = new QPushButton(T("Открыть чат с ИИ  →"), panel);
    openChat->setObjectName(QStringLiteral("aiOpenChatButton"));
    openChat->setStyleSheet(QStringLiteral(
        "QPushButton#aiOpenChatButton{background:#F5B700;"
        "color:#111317;border:1px solid #F5B700;font-size:9.5pt;font-weight:700;padding:8px 14px;border-radius:4px;text-align:center;}"
        "QPushButton#aiOpenChatButton:hover{background:#FFD54F;border-color:#FFD54F;}"));
    layout->addWidget(openChat);
    QObject::connect(openChat, &QPushButton::clicked, panel, [panel] {
        openYandexAssistant(panel);
    });

    // Нижняя подпись провайдера/технической поддержки убрана по запросу пользователя.
    layout->addStretch(1);
    return panel;
}
}

MainWindow::MainWindow(CameraManager *mgr, QWidget *parent) : QMainWindow(parent), m_mgr(mgr)
{
    setWindowTitle(App::title());
    setWindowIcon(makeAppIcon());
    if (qApp) qApp->setWindowIcon(makeAppIcon());
    // Минимальный размер зависит от доступной области текущего монитора.
    // Qt 6 уже работает в логических пикселях и учитывает системный DPI Windows.
    const QRect available = QGuiApplication::primaryScreen()
        ? QGuiApplication::primaryScreen()->availableGeometry() : QRect(0, 0, 1920, 1080);
    // Минимум зависит от монитора и не превышает доступную область.
    setMinimumSize(qMax(480, qMin(1024, available.width() - 48)),
                    qMax(360, qMin(640, available.height() - 72)));

    writePsvmsLog(QStringLiteral("MainWindow: создание SideNav — начало"));
    m_nav = new SideNav(this);
    // Восстанавливаем состояние панели навигации, сохранённое при прошлом выходе.
    m_nav->setExpanded(QSettings().value(QStringLiteral("ui/navExpanded"), true).toBool(), false);
    writePsvmsLog(QStringLiteral("MainWindow: SideNav создан"));
    m_top = new TopBar(mgr, this);
    writePsvmsLog(QStringLiteral("MainWindow: TopBar создан"));
    m_stack = new QStackedWidget(this);

    writePsvmsLog(QStringLiteral("MainWindow: создание страниц — начало"));
    m_live = new LiveViewPage(mgr, this);
    writePsvmsLog(QStringLiteral("MainWindow: LiveViewPage создан"));
    m_devices = new DevicesPage(mgr, this);
    writePsvmsLog(QStringLiteral("MainWindow: DevicesPage создан"));
    m_events = new EventsPage(this);
    writePsvmsLog(QStringLiteral("MainWindow: EventsPage создан"));
    m_settings = new SettingsPage(mgr, this);
    writePsvmsLog(QStringLiteral("MainWindow: SettingsPage создан"));
    m_stack->addWidget(m_live);
    m_stack->addWidget(m_devices);
    m_stack->addWidget(m_events);
    m_stack->addWidget(m_settings);

    m_nav->addPage(QStringLiteral("▣"), T("Главная"));
    m_nav->addPage(QStringLiteral("▤"), T("Камеры"));
    // Keep page indices aligned with QStackedWidget: Home=0, Devices=1, Events=2.
    // Archive is an action, so it must be added after the page items.
    m_nav->addPage(QStringLiteral("⚑"), T("События"));
    m_nav->addPage(QStringLiteral("⚙"), T("Настройки"));
    const int archiveNavItem = m_nav->addAction(QStringLiteral("▧"), T("Архив"));
    const int aboutNavItem = -1;
    const int exitNavItem = m_nav->addBottomAction(QStringLiteral("EXIT"), T("Выход"));
    m_settingsItem = PageSettings;
    m_langItem = -1;
    m_fullscreenItem = -1;

    // Подсказки с горячими клавишами делают навигацию понятной без отдельной инструкции.
    if (QWidget *item = m_nav->itemButton(PageLive))
        item->setToolTip(T("Главная / Мониторинг — Ctrl+1"));
    if (QWidget *item = m_nav->itemButton(PageDevices))
        item->setToolTip(T("Камеры — Ctrl+2"));
    if (QWidget *item = m_nav->itemButton(PageEvents))
        item->setToolTip(T("События — Ctrl+3"));
    if (QWidget *item = m_nav->itemButton(PageSettings))
        item->setToolTip(T("Настройки — Ctrl+4"));
    if (QWidget *item = m_nav->itemButton(archiveNavItem))
        item->setToolTip(T("Открыть архив выбранной камеры"));
    // Восстанавливаем пользовательский порядок страниц после создания всех пунктов.
    m_nav->restorePageOrder();

    const UserInfo user = Users::current();
    m_nav->setUser(user.name, user.isDeveloper() ? T("Разработчик")
                         : user.isGuest() ? T("Гость")
                         : user.isAdmin() ? T("Администратор") : T("Оператор"));
    if (!user.isAdmin()) {                         // Управление камерами остаётся административным
        m_nav->setItemVisible(PageDevices, false);
    }
    if (!isProVersion()) {                         // События доступны только в Pro-редакции
        m_nav->setItemVisible(PageEvents, false);
    }

    // Закрываем отдельное окно архива при переходе в любой другой раздел
    // главного окна, чтобы оно не оставалось поверх интерфейса.
    const auto closeArchiveWindow = [] {
        const auto windows = QApplication::topLevelWidgets();
        for (QWidget *window : windows) {
            if (window && window->property("psvms.singleton").toString() == QStringLiteral("archive")) {
                window->close();
                break;
            }
        }
    };

    connect(m_nav, &SideNav::currentChanged, this, [this, closeArchiveWindow](int pageId) {
        // Сохраняем именно постоянный ID страницы, а не её позицию в переставляемом меню.
        QSettings settings;
        settings.setValue(QStringLiteral("ui/navCurrentPage"), pageId);
        closeArchiveWindow();
        if (pageId >= 0 && pageId < m_stack->count())
            m_stack->setCurrentIndex(pageId);
        m_top->setTitle(pageTitle(pageId));
        if (m_aiHomePanel) m_aiHomePanel->setVisible(pageId == PageLive &&
            settings.value(QStringLiteral("ai/showHomePanel"), true).toBool());
    });
    connect(m_nav, &SideNav::actionTriggered, this, [this, closeArchiveWindow, archiveNavItem,
                                                        aboutNavItem, exitNavItem](int i) {
        if (i != archiveNavItem)
            closeArchiveWindow();
        if (i == archiveNavItem) {
            m_live->openArchiveForSelectedCamera();
            return;
        }
        if (i == aboutNavItem) {
            m_nav->setCurrent(PageSettings);
            m_top->setTitle(pageTitle(PageSettings));
            if (auto *tabs = m_settings->findChild<QTabWidget *>()) {
                for (int tab = 0; tab < tabs->count(); ++tab) {
                    if (tabs->tabText(tab).compare(T("О программе"), Qt::CaseInsensitive) == 0) {
                        tabs->setCurrentIndex(tab);
                        break;
                    }
                }
            }
            return;
        }
        if (i == exitNavItem) {
            close();
            return;
        }
    });
    connect(m_nav, &SideNav::changePasswordRequested, this, [this] { changeOwnPassword(this); });
    connect(m_nav, &SideNav::logoutRequested, this, [] {
        SecurityAudit::write(QStringLiteral("session_switch_requested"));
        App::requestLogout();
    });
    // Возвращаем пользователя на последний открытый раздел, если его роль и редакция
    // лицензии по-прежнему разрешают доступ. Иначе безопасно открываем мониторинг.
    int initialPage = QSettings().value(QStringLiteral("ui/navCurrentPage"), PageLive).toInt();
    if (initialPage == PageDevices && !user.isAdmin()) initialPage = PageLive;
    if (initialPage == PageSettings && !user.isAdmin()) initialPage = PageLive;
    if (initialPage == PageEvents && !isProVersion()) initialPage = PageLive;
    if (initialPage < PageLive || initialPage > PageSettings) initialPage = PageLive;
    m_nav->setCurrent(initialPage, false);

    // Периодически перепроверяем срок лицензии, чтобы Pro-функции отключались
    // и при истечении срока во время открытого сеанса.
    auto *licenseTimer = new QTimer(this);
    licenseTimer->setInterval(30000);
    const bool initialProState = isProVersion();
    connect(licenseTimer, &QTimer::timeout, this, [this, previousProState = initialProState]() mutable {
        const bool currentProState = isProVersion();
        if (previousProState && !currentProState) {
            m_mgr->enforceDemoLimit();
            // Сразу закрываем Pro-раздел при окончании лицензии: скрытия пункта
            // меню недостаточно, если пользователь уже находился на странице.
            m_nav->setItemVisible(PageEvents, false);
            if (m_nav->current() == PageEvents)
                m_nav->setCurrent(PageLive, false);
            if (m_settings) {
                if (auto *tabs = m_settings->findChild<QTabWidget *>())
                    for (int i = 1; i <= 4; ++i) tabs->setTabEnabled(i, false);
            }
            statusBar()->showMessage(T("Срок лицензии истёк. Включён демонстрационный режим."), 10000);
        }
        previousProState = currentProState;
    });
    licenseTimer->start();

    connect(m_top, &TopBar::fullscreenRequested, this, [this] { toggleMainFullscreen(); });

    // Быстрые клавиши навигации и обслуживания — доступны из любого раздела.
    auto addAppShortcut = [this](const QKeySequence &sequence, const QString &name,
                                 std::function<void()> action) {
        auto *shortcut = new QShortcut(sequence, this);
        shortcut->setContext(Qt::WindowShortcut);
        shortcut->setObjectName(name);
        connect(shortcut, &QShortcut::activated, this, [action] { action(); });
    };
    addAppShortcut(QKeySequence(QStringLiteral("Ctrl+1")), QStringLiteral("shortcut-monitoring"),
                   [this] { m_nav->setCurrent(PageLive); });
    addAppShortcut(QKeySequence(QStringLiteral("Ctrl+2")), QStringLiteral("shortcut-cameras"), [this] {
        if (Users::current().isAdmin()) m_nav->setCurrent(PageDevices);
        else statusBar()->showMessage(T("Управление камерами доступно только администратору"), 4000);
    });
    addAppShortcut(QKeySequence(QStringLiteral("Ctrl+3")), QStringLiteral("shortcut-events"), [this] {
        if (isProVersion()) m_nav->setCurrent(PageEvents);
        else statusBar()->showMessage(T("Раздел событий недоступен в этой редакции"), 4000);
    });
    addAppShortcut(QKeySequence(QStringLiteral("Ctrl+4")), QStringLiteral("shortcut-settings"), [this] {
        if (Users::current().isAdmin()) m_nav->setCurrent(PageSettings);
        else statusBar()->showMessage(T("Настройки доступны только администратору"), 4000);
    });
    addAppShortcut(QKeySequence(Qt::Key_F11), QStringLiteral("shortcut-fullscreen"),
                   [this] { toggleMainFullscreen(); });
    addAppShortcut(QKeySequence(QStringLiteral("Ctrl+R")), QStringLiteral("shortcut-refresh-cameras"), [this] {
        m_live->grid()->refreshCells();
        m_mgr->probeNow();
        statusBar()->showMessage(T("Обновление камер запущено"), 3000);
    });
    connect(m_top, &TopBar::statusRequested, this, [this] {
        // Не создаём дубликаты: если окно уже открыто, активируем существующее.
        if (m_cameraStatusDialog) {
            m_cameraStatusDialog->show();
            m_cameraStatusDialog->raise();
            m_cameraStatusDialog->activateWindow();
            return;
        }
        if (activateExistingPsvmsWindow(QStringLiteral("camera-status"))) return;
        m_cameraStatusDialog = new CameraStatusDialog(m_mgr, this);
        m_cameraStatusDialog->setProperty("psvms.singleton", QStringLiteral("camera-status"));
        m_cameraStatusDialog->show();
        m_cameraStatusDialog->raise();
        m_cameraStatusDialog->activateWindow();
    });
    connect(m_top, &TopBar::cloudRequested, this, [this] {
        CloudDialog dlg(this);
        dlg.exec();
    });
    connect(m_top, &TopBar::simpleModeToggled, this, [](bool simple) {
        // В простом режиме дополнительно отслеживаем вновь создаваемые UI-анимации:
        // некоторые панели создают их отложенно, уже после первичного прохода остановки.
        static QTimer *simpleModeAnimationGuard = nullptr;
        if (simple) {
            if (!simpleModeAnimationGuard) {
                simpleModeAnimationGuard = new QTimer(qApp);
                simpleModeAnimationGuard->setInterval(100);
                QObject::connect(simpleModeAnimationGuard, &QTimer::timeout, qApp, [] {
                    const auto currentWidgets = QApplication::allWidgets();
                    for (QWidget *widget : currentWidgets) {
                        if (!widget) continue;
                        const auto currentAnimations = widget->findChildren<QAbstractAnimation *>();
                        for (QAbstractAnimation *animation : currentAnimations) {
                            if (!animation || animation->state() == QAbstractAnimation::Stopped)
                                continue;
                            // Завершаем конечные переходы до целевого состояния, чтобы
                            // простой режим не оставлял панели/кнопки в промежуточной геометрии.
                            if (animation->loopCount() != -1 && animation->duration() > 0)
                                animation->setCurrentTime(animation->duration());
                            if (animation->state() != QAbstractAnimation::Stopped)
                                animation->stop();
                        }
                        if (auto *opacity = qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect()))
                            opacity->setOpacity(1.0);
                        if (auto *shadow = qobject_cast<QGraphicsDropShadowEffect *>(widget->graphicsEffect()))
                            shadow->setBlurRadius(5.0);
                    }
                });
            }
            if (!simpleModeAnimationGuard->isActive())
                simpleModeAnimationGuard->start();
        } else if (simpleModeAnimationGuard) {
            simpleModeAnimationGuard->stop();
            simpleModeAnimationGuard->deleteLater();
            simpleModeAnimationGuard = nullptr;
        }
        QSettings().setValue(QStringLiteral("perf/lowPower"), simple);
        Cfg::reload(); // CfgNotifier обновляет все подписанные компоненты сразу.
        // Переключение темы/плотности меняет геометрию idle-страниц без гарантированного
        // ResizeEvent у дочерних QLabel. Пересчитываем положение эмблемы после layout pass.
        const auto refreshCellArtworkAfterModeChange = [] {
            for (QWidget *widget : QApplication::allWidgets()) {
                if (auto *cell = qobject_cast<VideoCell *>(widget))
                    cell->refreshCellArtwork();
            }
        };
        // Qt может завершать перерасчёт вложенных layouts несколькими
        // проходами после смены режима. Повторно фиксируем центр уже после
        // каждого такого прохода; координаты считаются от m_idle каждой ячейки.
        QTimer::singleShot(0, qApp, refreshCellArtworkAfterModeChange);
        QTimer::singleShot(50, qApp, refreshCellArtworkAfterModeChange);
        QTimer::singleShot(150, qApp, refreshCellArtworkAfterModeChange);
        QTimer::singleShot(300, qApp, refreshCellArtworkAfterModeChange);

        // QApplication::findChildren() не охватывает объекты анимаций,
        // принадлежащие отдельным окнам/виджетам. Обходим дерево каждого
        // существующего виджета, собирая анимации без дубликатов.
        QList<QAbstractAnimation *> animations;
        const auto widgets = QApplication::allWidgets();
        for (QWidget *widget : widgets) {
            if (!widget) continue;
            const auto found = widget->findChildren<QAbstractAnimation *>();
            for (QAbstractAnimation *animation : found) {
                if (animation && !animations.contains(animation))
                    animations.append(animation);
            }
        }

        if (simple) {
            for (QAbstractAnimation *animation : animations) {
                if (!animation || animation->state() != QAbstractAnimation::Running)
                    continue;
                // Одноразовые переходы доводим до конечного состояния, чтобы
                // панели/кнопки не оставались в промежуточной геометрии.
                if (animation->loopCount() != -1 && animation->duration() > 0) {
                    animation->setCurrentTime(animation->duration());
                    if (animation->state() != QAbstractAnimation::Stopped)
                        animation->stop();
                } else {
                    animation->stop();
                }
            }
            for (QWidget *widget : widgets) {
                if (!widget) continue;
                if (auto *opacity = qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect()))
                    opacity->setOpacity(1.0);
                if (auto *glow = qobject_cast<QGraphicsDropShadowEffect *>(widget->graphicsEffect()))
                    glow->setBlurRadius(5.0);
            }
            writePsvmsLog(QStringLiteral("Режим простой: активные UI-анимации остановлены на всех окнах; новые анимации отключены"));
        } else {
            // Возвращаем только постоянную пульсацию логотипа. Одноразовые
            // переходы не запускаем повторно самопроизвольно.
            for (QWidget *widget : widgets) {
                if (!widget) continue;
                const auto brandAnimations = widget->findChildren<QAbstractAnimation *>(QStringLiteral("brandPulse"));
                for (QAbstractAnimation *animation : brandAnimations) {
                    if (animation && animation->state() == QAbstractAnimation::Stopped)
                        animation->start();
                }
            }
            writePsvmsLog(QStringLiteral("Расширенный режим: анимации интерфейса снова разрешены"));
        }
    });

    // «+» в пустой ячейке → «Добавить новую камеру…»
    connect(m_live, &LiveViewPage::addCameraRequested, this, [this] {
        if (!Users::current().isAdmin()) {
            statusBar()->showMessage(T("Добавлять камеры может только администратор"), 5000);
            return;
        }
        RtspQuickDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted)
            m_mgr->add(dlg.entry());
    });
    connect(m_live, &LiveViewPage::message, this, [this](const QString &text) { statusBar()->showMessage(text, 7000); });
    connect(m_settings, &SettingsPage::languageChosen, this, &MainWindow::applyLanguage);
    connect(m_settings, &SettingsPage::proSessionActivated, this, [this] {
        m_nav->setItemVisible(PageEvents, true);
        if (auto *ptz = m_live->findChild<PtzPanel *>()) {
            ptz->setEnabled(true);
            ptz->setToolTip(QString());
        }
    });
    connect(m_settings, &SettingsPage::proSessionDeactivated, this, [this] {
        // При завершении Pro-сеанса блокируем и саму страницу, и переходы к ней.
        m_nav->setItemVisible(PageEvents, false);
        if (m_nav->current() == PageEvents)
            m_nav->setCurrent(PageLive, false);
        if (auto *ptz = m_live->findChild<PtzPanel *>()) {
            ptz->setEnabled(false);
            ptz->setToolTip(T("Управление PTZ доступно в режиме разработчика"));
        }
    });

    // Метрики, редакция и голосовая кнопка создаются до компоновки нижней панели.
    m_sysLabel = new QLabel(this);
    m_sysLabel->setObjectName(QStringLiteral("sysStats"));
    m_clock = new QLabel(this);
    m_clock->setObjectName(QStringLiteral("clock"));
    auto *editionFooter = new QLabel(this);
    editionFooter->setObjectName(QStringLiteral("editionFooter"));
    editionFooter->setStyleSheet(QStringLiteral("color:#ff4d4f;font-size:10pt;font-weight:700;padding:0 4px;letter-spacing:0.2px;"));
    auto refreshEditionFooter = [editionFooter] {
        const bool pro = isProVersion();
        editionFooter->setText(pro ? T("Pro версия") : T("DEMO ВЕРСИЯ"));
        editionFooter->setStyleSheet(pro
            ? QStringLiteral("color:#d4af37;font-size:10pt;font-weight:800;padding:0 4px;")
            : QStringLiteral("color:#ff4548;font-size:10pt;font-weight:700;padding:0 4px;letter-spacing:0.2px;"));
    };
    refreshEditionFooter();
    QObject::connect(Cfg::notifier(), &CfgNotifier::changed, editionFooter, refreshEditionFooter);
    m_voiceButton = new QPushButton(T("🎙 Голос"), this);
    m_voiceButton->setObjectName(QStringLiteral("voiceControlButton"));
    m_voiceButton->setToolTip(T("Включить или выключить голосовое управление на русском языке"));
    m_voiceButton->setCursor(Qt::PointingHandCursor);
    m_voiceButton->setMinimumWidth(132);
    m_voiceButton->setStyleSheet(QStringLiteral(
        "QPushButton{background:#1D222C;color:#E4EAF2;border:1px solid #303847;border-radius:4px;"
        "padding:5px 12px;font-weight:600;}"
        "QPushButton:hover{background:#262D3A;border-color:#F5B700;color:#FFFFFF;}"
        "QPushButton:checked{background:#F5B700;color:#111317;border-color:#F5B700;font-weight:700;}"));
    m_voiceButton->setCheckable(true);
    connect(m_voiceButton, &QPushButton::clicked, this, &MainWindow::toggleVoiceControl);


    auto *central = new PsvmsBackgroundWidget(this);
    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *right = new QVBoxLayout;
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(0);
    right->addWidget(m_top);
    auto *mainContent = new QHBoxLayout;
    mainContent->setContentsMargins(10, 8, 10, 8);
    mainContent->setSpacing(10);
    mainContent->addWidget(m_stack, 1);
    m_aiHomePanel = buildAiHomePanel(central);
    mainContent->addWidget(m_aiHomePanel);
    m_aiHomePanel->setVisible(QSettings().value(QStringLiteral("ai/showHomePanel"), true).toBool());
    right->addLayout(mainContent, 1);

    // Нижняя панель встроена в рабочую область: CPU и часы находятся строго
    // в центре, а голосовое управление — у правого края независимо от ширины окна.
    auto *footer = new QWidget(central);
    footer->setObjectName(QStringLiteral("mainFooter"));
    footer->setMinimumHeight(38);
    auto *footerGrid = new QGridLayout(footer);
    footerGrid->setContentsMargins(12, 2, 12, 4);
    footerGrid->setHorizontalSpacing(10);
    footerGrid->setVerticalSpacing(0);
    auto *editionSlot = new QWidget(footer);
    editionSlot->setFixedWidth(160);
    auto *editionLay = new QHBoxLayout(editionSlot);
    editionLay->setContentsMargins(0, 0, 0, 0);
    editionLay->addWidget(editionFooter, 0, Qt::AlignLeft | Qt::AlignVCenter);
    auto *rightBalanceSlot = new QWidget(footer);
    rightBalanceSlot->setFixedWidth(160); // симметричная ширина для точного центра CPU/часов
    auto *centerStats = new QWidget(footer);
    auto *centerStatsLay = new QHBoxLayout(centerStats);
    centerStatsLay->setContentsMargins(0, 0, 0, 0);
    centerStatsLay->setSpacing(18);
    centerStatsLay->addWidget(m_sysLabel);
    centerStatsLay->addWidget(m_clock);
    footerGrid->addWidget(editionSlot, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
    footerGrid->addWidget(centerStats, 0, 1, Qt::AlignCenter | Qt::AlignVCenter);
    footerGrid->addWidget(rightBalanceSlot, 0, 2);
    footerGrid->setColumnStretch(0, 1);
    footerGrid->setColumnStretch(1, 0);
    footerGrid->setColumnStretch(2, 1);
    footer->setStyleSheet(QStringLiteral(
        "QWidget#mainFooter{background:#12151B;border-top:1px solid #2A303C;}"
        "QLabel#sysStats,QLabel#clock{color:#9AA4B5;font-size:9pt;font-weight:600;padding:2px 6px;}"));
    right->addWidget(footer, 0);
    root->addWidget(m_nav);
    root->addLayout(right, 1);
    setCentralWidget(central);
    statusBar()->addPermanentWidget(m_voiceButton); // правый нижний угол окна, рядом с системным краем
    statusBar()->setSizeGripEnabled(false);
    writePsvmsLog(QStringLiteral("MainWindow: центральный виджет установлен"));

    auto *clockTimer = new QTimer(this);
    auto tick = [this, editionFooter, refreshEditionFooter] {
        m_clock->setText(QDateTime::currentDateTime().toString(QStringLiteral("dd.MM.yyyy  HH:mm:ss")));
        refreshEditionFooter();
    };
    connect(clockTimer, &QTimer::timeout, this, tick);

    // Показатель CPU находится только в нижней строке состояния.
    auto *sysTimer = new QTimer(this);
    sysTimer->setInterval(2000);
    connect(sysTimer, &QTimer::timeout, this, [this] {
        const SysStats stats = readSysStats();
        m_sysLabel->setText(stats.cpuPercent < 0
            ? T("CPU: недоступно")
            : T("CPU: %1%").arg(qRound(stats.cpuPercent)));
    });
    sysTimer->start();
    clockTimer->start(1000);
    tick();

    // ---- журнал событий и статус реагируют на события рекордеров ----
    connect(mgr, &CameraManager::cameraError, this, [this](const QString &id, const QString &msg) {
        const CameraEntry *e = m_mgr->find(id);
        const QString cameraName = e ? e->name : id;
        m_events->addEvent(cameraName, T("ОШИБКА: %1").arg(msg));
        writePsvmsLog(QStringLiteral("ОШИБКА КАМЕРЫ [%1]: %2").arg(cameraName, msg));
    });
    connect(mgr, &CameraManager::cameraStateChanged, this, [this](const QString &id, CameraWorker::State s) {
        const CameraEntry *e = m_mgr->find(id);
        QString eventText = stateText(s);
        if (s == CameraWorker::State::Connecting) eventText = T("Подключение к камере");
        else if (s == CameraWorker::State::Reconnecting) eventText = T("Потеря связи — выполняется переподключение");
        else if (s == CameraWorker::State::Stopped) eventText = T("Камера отключена / поток остановлен");
        m_events->addEvent(e ? e->name : id, eventText);
    });
    auto cameraSnapshot = QSharedPointer<QHash<QString, QString>>::create();
    for (const CameraEntry &entry : m_mgr->cameras())
        cameraSnapshot->insert(entry.id, entry.name + QChar(0x1f) + entry.group + QChar(0x1f) + entry.url);
    connect(mgr, &CameraManager::camerasChanged, this, [this, cameraSnapshot] {
        QHash<QString, QString> current;
        for (const CameraEntry &entry : m_mgr->cameras())
            current.insert(entry.id, entry.name + QChar(0x1f) + entry.group + QChar(0x1f) + entry.url);
        for (auto it = current.cbegin(); it != current.cend(); ++it) {
            if (!cameraSnapshot->contains(it.key()))
                m_events->addEvent(it.value().section(QChar(0x1f), 0, 0), T("Камера добавлена"));
            else if (cameraSnapshot->value(it.key()) != it.value())
                m_events->addEvent(it.value().section(QChar(0x1f), 0, 0), T("Параметры камеры изменены"));
        }
        for (auto it = cameraSnapshot->cbegin(); it != cameraSnapshot->cend(); ++it)
            if (!current.contains(it.key()))
                m_events->addEvent(it.value().section(QChar(0x1f), 0, 0), T("Камера удалена из списка"));
        *cameraSnapshot = current;
    });

    // ---- геометрия окна: учитываем именно тот монитор, на котором оно окажется ----
    // Qt 6 использует логические пиксели и системный per-monitor DPI Windows.
    // Не привязываем размеры к primaryScreen(): на нескольких мониторах он может
    // иметь другую рабочую область и другой масштаб 100/125/150/200%.
    QSettings s;
    const bool restored = restoreGeometry(s.value(QStringLiteral("ui/geometry")).toByteArray());
    QScreen *targetScreen = nullptr;
    if (restored)
        targetScreen = QGuiApplication::screenAt(frameGeometry().center());
    if (!targetScreen)
        targetScreen = QGuiApplication::primaryScreen();
    const QRect screenArea = targetScreen
        ? targetScreen->availableGeometry() : QRect(0, 0, 1920, 1080);

    // Оставляем одинаковые пропорции окна на мониторах разного размера,
    // но не позволяем окну занимать всю рабочую область или выходить за неё.
    const int safeWidth = qMax(320, int(screenArea.width() * 0.92));
    const int safeHeight = qMax(240, int(screenArea.height() * 0.90));
    if (!restored) {
        const int idx = qBound(0, s.value(QStringLiteral("ui/windowSize"), 0).toInt(),
                               int(sizeof(kMainWinSizes) / sizeof(kMainWinSizes[0])) - 1);
        const QSize requested = idx > 0 ? QSize(kMainWinSizes[idx].w, kMainWinSizes[idx].h)
                                       : QSize(int(screenArea.width() * 0.88), int(screenArea.height() * 0.86));
        resize(qMin(requested.width(), safeWidth), qMin(requested.height(), safeHeight));
        move(screenArea.center() - rect().center());
    } else {
        // Если сохранённая геометрия пришла с другого монитора/DPI, подгоняем
        // её к доступной области целевого экрана и гарантируем видимость заголовка.
        resize(qMin(width(), safeWidth), qMin(height(), safeHeight));
        const QRect frame = frameGeometry();
        const bool mostlyVisible = screenArea.intersects(frame)
            && screenArea.intersected(frame).width() >= qMin(240, frame.width())
            && screenArea.intersected(frame).height() >= qMin(100, frame.height());
        if (!mostlyVisible) {
            move(screenArea.center() - rect().center());
        } else {
            const int maxX = qMax(screenArea.left(), screenArea.right() - width() + 1);
            const int maxY = qMax(screenArea.top(), screenArea.bottom() - height() + 1);
            move(qBound(screenArea.left(), x(), maxX), qBound(screenArea.top(), y(), maxY));
        }
    }

    // ---- горячие клавиши ----
    auto *fullscreenKey = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(fullscreenKey, &QShortcut::activated, this, [this] {
        toggleMainFullscreen();
    });
    auto *escapeMainFullscreen = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escapeMainFullscreen->setContext(Qt::WindowShortcut);
    connect(escapeMainFullscreen, &QShortcut::activated, this, [this] {
        if (isFullScreen())
            leaveMainFullscreen();
    });
    auto *refreshKey = new QShortcut(QKeySequence(Qt::Key_F5), this);
    connect(refreshKey, &QShortcut::activated, this, [this] {
        m_live->grid()->refreshCells();
        m_mgr->probeNow();
    });
    auto *closeCellKey = new QShortcut(QKeySequence(Qt::Key_Delete), m_live);
    closeCellKey->setContext(Qt::WidgetWithChildrenShortcut);   // не мешает Delete в текстовых полях
    connect(closeCellKey, &QShortcut::activated, this, [this] { m_live->grid()->closeSelectedCell(); });

    // 1 / 2 / 3 / 4 — быстрые раскладки 1, 2×2, 3×3, 4×4; действуют только когда открыт «Мониторинг»
    const int quickLayouts[4] = {1, 4, 9, 16};
    for (int i = 0; i < 4; ++i) {
        auto *key = new QShortcut(QKeySequence(QString::number(i + 1)), m_live);
        key->setContext(Qt::WidgetWithChildrenShortcut);
        const int count = quickLayouts[i];
        connect(key, &QShortcut::activated, this, [this, count] { m_live->grid()->setLayoutCount(count); });
    }

    // Быстрая навигация по модулям PSVms через Ctrl+1..Ctrl+5
    for (int p = 0; p < 5; ++p) {
        auto *modKey = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(p + 1)), this);
        modKey->setContext(Qt::WindowShortcut);
        connect(modKey, &QShortcut::activated, this, [this, p] {
            if (m_nav && p < m_nav->itemCount())
                m_nav->setCurrent(p, true);
        });
    }

    // Единая мягкая анимация наведения и нажатия для кнопок всех основных разделов.
    auto *buttonMotion = new ButtonMotionFilter(this);
    for (QPushButton *button : findChildren<QPushButton *>())
        button->installEventFilter(buttonMotion);
    for (QToolButton *button : findChildren<QToolButton *>())
        button->installEventFilter(buttonMotion);

    // Автоблокировка выключена по умолчанию. При включении любое действие мышью/клавиатурой
    // сбрасывает таймер; по истечении времени возвращаем пользователя к экрану входа,
    // не завершая процесс PSVMS.
    m_inactivityTimer = new QTimer(this);
    m_inactivityTimer->setSingleShot(true);
    connect(m_inactivityTimer, &QTimer::timeout, this, [this] {
        SecurityAudit::write(QStringLiteral("session_locked_idle"));
        statusBar()->showMessage(T("Сеанс заблокирован из-за бездействия."), 3000);
        App::requestLogout();
    });
    qApp->installEventFilter(this);
    resetInactivityTimer();

}


void MainWindow::resetInactivityTimer()
{
    if (!m_inactivityTimer)
        return;
    const bool enabled = QSettings().value(QStringLiteral("security/idleLockEnabled"), false).toBool();
    if (!enabled) {
        m_inactivityTimer->stop();
        return;
    }
    const int minutes = qBound(1, QSettings().value(QStringLiteral("security/idleLockMinutes"), 10).toInt(), 240);
    m_inactivityTimer->start(minutes * 60 * 1000);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    Q_UNUSED(obj)
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::User: // изменение настройки автоблокировки
        resetInactivityTimer();
        break;
    default:
        break;
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::toggleVoiceControl()
{
#ifdef Q_OS_WIN
    if (m_voiceProcess && m_voiceProcess->state() != QProcess::NotRunning) {
        stopVoiceControl();
        return;
    }

    if (!m_voiceProcess) {
        m_voiceProcess = new QProcess(this);
        m_voiceProcess->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_voiceProcess, &QProcess::readyReadStandardOutput, this, [this] {
            m_voiceOutputBuffer += m_voiceProcess->readAllStandardOutput();
            while (true) {
                const int newline = m_voiceOutputBuffer.indexOf('\n');
                if (newline < 0) break;
                QByteArray line = m_voiceOutputBuffer.left(newline).trimmed();
                m_voiceOutputBuffer.remove(0, newline + 1);
                if (!line.isEmpty())
                    handleVoiceCommand(QString::fromUtf8(line));
            }
        });
        connect(m_voiceProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            if (m_voiceButton) {
                m_voiceButton->setChecked(false);
                m_voiceButton->setText(T("🎙 Голос"));
            }
            if (m_voiceProcess && m_voiceProcess->error() == QProcess::FailedToStart)
                statusBar()->showMessage(T("Не удалось запустить распознавание речи Windows"), 7000);
        });
        connect(m_voiceProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int, QProcess::ExitStatus) {
            if (m_voiceButton) {
                m_voiceButton->setChecked(false);
                m_voiceButton->setText(T("🎙 Голос"));
            }
        });
    }

    // Используется системный распознаватель речи Windows. Дополнительные DLL и Qt-модули не нужны.
    const QString script = QString::fromUtf8(R"PS(
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
try {
  Add-Type -AssemblyName System.Speech
  $culture = [System.Globalization.CultureInfo]::GetCultureInfo('ru-RU')
  $recognizer = [System.Speech.Recognition.SpeechRecognitionEngine]::new($culture)
  $recognizer.SetInputToDefaultAudioDevice()
  $choices = [System.Speech.Recognition.Choices]::new()
  @(
    'главная','открой главную','покажи камеры','открой камеры','мониторинг',
    'устройства','открой устройства','события','открой события','настройки','открой настройки',
    'состояние камер','покажи состояние камер','покажи камеры не в сети','камеры не в сети','не в сети',
    'покажи все камеры','все камеры','переключи на четыре камеры','переключи на 4 камеры',
    'переключи на девять камер','переключи на 9 камер','переключи на шестнадцать камер','переключи на 16 камер',
    'полный экран','включи полный экран','включи полноэкранный режим',
    'выйди из полного экрана','выключи полный экран','обнови камеры','обновить камеры',
    'раскладка один','раскладка четыре','раскладка девять','раскладка шестнадцать',
    'одна камера','четыре камеры','девять камер','шестнадцать камер',
    'стоп','остановить голосовое управление'
  ) | ForEach-Object { [void]$choices.Add($_) }
  $grammar = [System.Speech.Recognition.Grammar]::new($choices)
  $recognizer.LoadGrammar($grammar)
  $recognizer.InitialSilenceTimeout = [TimeSpan]::FromSeconds(3)
  $recognizer.BabbleTimeout = [TimeSpan]::FromSeconds(2)
  while ($true) {
    $result = $recognizer.Recognize()
    if ($null -ne $result -and $null -ne $result.Text) {
      [Console]::WriteLine($result.Text)
      [Console]::Out.Flush()
      if ($result.Text -eq 'стоп' -or $result.Text -eq 'остановить голосовое управление') { break }
    }
  }
  $recognizer.Dispose()
} catch {
  [Console]::Error.WriteLine($_.Exception.Message)
  exit 2
}
)PS");
    m_voiceOutputBuffer.clear();
    m_voiceProcess->start(QStringLiteral("powershell.exe"),
                          {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                           QStringLiteral("Bypass"), QStringLiteral("-Command"), script});
    if (m_voiceButton) {
        m_voiceButton->setChecked(true);
        m_voiceButton->setText(T("🔴 Слушаю"));
    }
    statusBar()->showMessage(T("Голосовое управление включено. Скажите, например: «покажи камеры» или «полный экран»"), 9000);
#else
    QMessageBox::information(this, T("Голосовое управление"),
                             T("Голосовое управление в этой версии доступно в Windows."));
    if (m_voiceButton) m_voiceButton->setChecked(false);
#endif
}

void MainWindow::stopVoiceControl()
{
    if (m_voiceProcess && m_voiceProcess->state() != QProcess::NotRunning) {
        m_voiceProcess->terminate();
        if (!m_voiceProcess->waitForFinished(700))
            m_voiceProcess->kill();
    }
    if (m_voiceButton) {
        m_voiceButton->setChecked(false);
        m_voiceButton->setText(T("🎙 Голос"));
    }
    statusBar()->showMessage(T("Голосовое управление выключено"), 4000);
}

void MainWindow::handleVoiceCommand(const QString &rawCommand)
{
    QString command = rawCommand.trimmed().toLower();
    command.replace(QChar(0x0451), QChar(0x0435)); // ё -> е
    command.replace(QChar(0x00A0), QChar(' '));
    while (command.contains(QStringLiteral("  ")))
        command.replace(QStringLiteral("  "), QStringLiteral(" "));
    statusBar()->showMessage(T("Распознано: %1").arg(rawCommand), 3500);

    if (command == QStringLiteral("стоп") || command == QStringLiteral("остановить голосовое управление")) {
        stopVoiceControl();
    } else if (command == QStringLiteral("покажи все камеры") || command == QStringLiteral("все камеры") ||
               command == QStringLiteral("покажи камеры") || command == QStringLiteral("открой камеры") ||
               command == QStringLiteral("главная") || command == QStringLiteral("открой главную") ||
               command == QStringLiteral("мониторинг")) {
        const int total = int(m_mgr->cameras().size());
        if (total <= 0) {
            m_nav->setCurrent(PageLive);
            statusBar()->showMessage(T("Список камер пуст"), 5000);
            return;
        }
        int layout = 64;
        const int supported[] = {1, 4, 6, 8, 9, 12, 16, 20, 25, 36, 49, 64};
        for (int n : supported) {
            if (n >= total) { layout = n; break; }
        }
        if (total > 64) {
            statusBar()->showMessage(T("В раскладке можно одновременно показать не более 64 камер"), 6000);
            layout = 64;
        }
        m_nav->setCurrent(PageLive);
        m_live->grid()->clearAll();
        m_live->grid()->setLayoutCount(layout);
        const int shown = qMin(total, layout);
        for (int i = 0; i < shown; ++i)
            m_live->grid()->openCamera(m_mgr->cameras().at(i).id);
        statusBar()->showMessage(T("Показано камер: %1 из %2").arg(shown).arg(total), 5000);
    } else if (command == QStringLiteral("устройства") || command == QStringLiteral("открой устройства")) {
        m_nav->setCurrent(PageDevices);
    } else if (command == QStringLiteral("события") || command == QStringLiteral("открой события")) {
        if (isProVersion()) m_nav->setCurrent(PageEvents);
        else statusBar()->showMessage(T("Раздел событий недоступен в этой редакции"), 4000);
    } else if (command == QStringLiteral("настройки") || command == QStringLiteral("открой настройки")) {
        if (Users::current().isAdmin()) m_nav->setCurrent(PageSettings);
        else statusBar()->showMessage(T("Настройки доступны только администратору"), 4000);
    } else if (command == QStringLiteral("состояние камер") || command == QStringLiteral("покажи состояние камер")) {
        if (!m_cameraStatusDialog) {
            m_cameraStatusDialog = dynamic_cast<CameraStatusDialog *>(
                activateExistingPsvmsWindow(QStringLiteral("camera-status")));
        }
        if (!m_cameraStatusDialog) {
            m_cameraStatusDialog = new CameraStatusDialog(m_mgr, this);
            m_cameraStatusDialog->setProperty("psvms.singleton", QStringLiteral("camera-status"));
            m_cameraStatusDialog->show();
            m_cameraStatusDialog->raise();
            m_cameraStatusDialog->activateWindow();
        } else {
            m_cameraStatusDialog->showAllCameras();
        }
    } else if (command == QStringLiteral("покажи камеры не в сети") ||
               command == QStringLiteral("камеры не в сети") || command == QStringLiteral("не в сети")) {
        if (!m_cameraStatusDialog) {
            m_cameraStatusDialog = dynamic_cast<CameraStatusDialog *>(
                activateExistingPsvmsWindow(QStringLiteral("camera-status")));
        }
        if (!m_cameraStatusDialog) {
            m_cameraStatusDialog = new CameraStatusDialog(m_mgr, this);
            m_cameraStatusDialog->setProperty("psvms.singleton", QStringLiteral("camera-status"));
        }
        m_cameraStatusDialog->showOfflineOnly();
    } else if (command == QStringLiteral("полный экран") || command == QStringLiteral("включи полный экран") ||
               command == QStringLiteral("включи полноэкранный режим")) {
        if (!isFullScreen()) toggleMainFullscreen();
    } else if (command == QStringLiteral("выйди из полного экрана") || command == QStringLiteral("выключи полный экран")) {
        if (isFullScreen()) leaveMainFullscreen();
    } else if (command == QStringLiteral("обнови камеры") || command == QStringLiteral("обновить камеры")) {
        m_live->grid()->refreshCells();
        m_mgr->probeNow();
    } else if (command == QStringLiteral("раскладка один") || command == QStringLiteral("одна камера")) {
        m_nav->setCurrent(PageLive); m_live->grid()->setLayoutCount(1);
    } else if (command == QStringLiteral("раскладка четыре") || command == QStringLiteral("четыре камеры") ||
               command == QStringLiteral("переключи на четыре камеры") || command == QStringLiteral("переключи на 4 камеры")) {
        m_nav->setCurrent(PageLive); m_live->grid()->setLayoutCount(4);
    } else if (command == QStringLiteral("раскладка девять") || command == QStringLiteral("девять камер") ||
               command == QStringLiteral("переключи на девять камер") || command == QStringLiteral("переключи на 9 камер")) {
        m_nav->setCurrent(PageLive); m_live->grid()->setLayoutCount(9);
    } else if (command == QStringLiteral("раскладка шестнадцать") || command == QStringLiteral("шестнадцать камер") ||
               command == QStringLiteral("переключи на шестнадцать камер") || command == QStringLiteral("переключи на 16 камер")) {
        m_nav->setCurrent(PageLive); m_live->grid()->setLayoutCount(16);
    }
}

void MainWindow::toggleMainFullscreen()
{
    if (isFullScreen()) {
        leaveMainFullscreen();
        return;
    }

    // Запоминаем состояние именно перед первым входом в fullscreen.
    // Это позволяет вернуться к исходному размеру/положению, а не к размеру Qt по умолчанию.
    m_preFullscreenMaximized = isMaximized();
    m_preFullscreenGeometry = geometry();
    m_hasPreFullscreenState = true;
    showFullScreen();
}

void MainWindow::leaveMainFullscreen()
{
    if (!isFullScreen())
        return;

    // Выход из полноэкранного режима всегда возвращает главное окно
    // в развёрнутое состояние, независимо от прежней геометрии окна.
    showMaximized();
    m_hasPreFullscreenState = false;
}

bool MainWindow::startFullscreen() const
{
    return QSettings().value(QStringLiteral("ui/startFullscreen"), false).toBool();
}

QString MainWindow::pageTitle(int page) const
{
    switch (page) {
    case PageLive:     return T("Главная");
    case PageDevices:  return T("Камеры");
    case PageEvents:   return T("События");
    case PageSettings: return T("Настройки");
    }
    return QString();
}

void MainWindow::updateLanguageItem()
{
    m_nav->setItemText(m_langItem, QStringLiteral("🌐"), T("Язык") + QStringLiteral(": ") + I18n::code().toUpper());
}

void MainWindow::showLanguageMenu()
{
    QMenu menu(this);
    auto *group = new QActionGroup(&menu);
    const QString names[2] = {QString::fromUtf8("Русский"), QStringLiteral("English")};
    const QString codes[2] = {QStringLiteral("ru"), QStringLiteral("en")};
    for (int i = 0; i < 2; ++i) {
        QAction *a = menu.addAction(names[i]);
        a->setCheckable(true);
        a->setChecked(codes[i] == I18n::code());
        a->setData(codes[i]);
        group->addAction(a);
    }
    QWidget *b = m_nav->itemButton(m_langItem);
    const QPoint pos = b ? b->mapToGlobal(QPoint(b->width() + 4, 0)) : QCursor::pos();
    QAction *chosen = menu.exec(pos);
    if (chosen && chosen->data().toString() != I18n::code())
        applyLanguage(chosen->data().toString());
}

void MainWindow::applyLanguage(const QString &code)
{
    I18n::setLanguageByCode(code);
    QSettings s;
    s.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    s.setValue(QStringLiteral("ui/navExpanded"), m_nav->isExpanded());
    m_live->grid()->flushLayout();
    I18n::requestRestart();   // окно пересоздаётся на новом языке, запись камер не прерывается
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    stopVoiceControl();
    // При выходе пароль разработчика не запрашивается.
    // Он требуется только при запуске; неверный пароль там приводит к повторному запросу.
    m_live->grid()->flushLayout();
    m_mgr->persist();                 // cameras, recorders, groups and stream settings
    QSettings s;
    s.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    s.setValue(QStringLiteral("ui/navExpanded"), m_nav->isExpanded());
    s.sync();
    m_mgr->shutdown();   // корректно закрыть текущие сегменты записи
    QMainWindow::closeEvent(e);
}

// ======================================================================
// app/onvif_import_dialog.cpp
// ======================================================================
namespace {

enum Column_onvifdlg { ColCheck_onvifdlg, ColAddress_onvifdlg, ColName_onvifdlg, ColState_onvifdlg, ColCount_onvifdlg };

constexpr int kMaxRange_onvifdlg = 2048;

bool parseIpv4_onvifdlg(const QString &s, quint32 *out)
{
    QHostAddress a;
    if (!a.setAddress(s.trimmed()) || a.protocol() != QAbstractSocket::IPv4Protocol)
        return false;
    *out = a.toIPv4Address();
    return true;
}

} // namespace

OnvifImportDialog::OnvifImportDialog(CameraManager *mgr, QWidget *parent) : QDialog(parent), m_mgr(mgr)
{
    setWindowTitle(T("Импорт камер по ONVIF"));
    resize(940, 640);

    for (const CameraEntry &e : m_mgr->cameras())
        m_existingHosts.insert(QUrl(e.url).host());

    m_discovery = new OnvifDiscovery(this);
    m_scanner = new OnvifScanner(this);
    m_resolver = new OnvifResolver(this);

    // ---- поиск ----
    auto *findBox = new QGroupBox(T("1. Поиск камер"), this);
    auto *findLay = new QHBoxLayout(findBox);
    m_btnDiscover = new QPushButton(T("Найти в своей сети"), findBox);
    m_from = new QLineEdit(findBox);
    m_from->setPlaceholderText(QStringLiteral("192.168.1.1"));
    m_to = new QLineEdit(findBox);
    m_to->setPlaceholderText(QStringLiteral("192.168.1.254"));
    m_port = new QSpinBox(findBox);
    m_port->setRange(1, 65535);
    m_port->setValue(80);
    m_port->setToolTip(T("Порт ONVIF: обычно 80; у камер Xiongmai/iCSee — 8899"));
    m_btnScan = new QPushButton(T("Сканировать диапазон"), findBox);
    findLay->addWidget(m_btnDiscover);
    findLay->addSpacing(20);
    findLay->addWidget(new QLabel(T("Диапазон IP:"), findBox));
    findLay->addWidget(m_from);
    findLay->addWidget(new QLabel(QStringLiteral("—"), findBox));
    findLay->addWidget(m_to);
    findLay->addWidget(new QLabel(T("порт"), findBox));
    findLay->addWidget(m_port);
    findLay->addWidget(m_btnScan);

    // ---- доступ ----
    auto *authBox = new QGroupBox(T("2. Общие логин и пароль камер"), this);
    auto *authLay = new QHBoxLayout(authBox);
    m_user = new QLineEdit(QStringLiteral("admin"), authBox);
    m_pass = new QLineEdit(authBox);
    m_pass->setEchoMode(QLineEdit::Password);
    m_group = new QLineEdit(authBox);
    m_group->setPlaceholderText(T("например, Склад"));
    authLay->addWidget(new QLabel(T("Логин:"), authBox));
    authLay->addWidget(m_user);
    authLay->addWidget(new QLabel(T("Пароль:"), authBox));
    authLay->addWidget(m_pass);
    authLay->addWidget(new QLabel(T("Группа:"), authBox));
    authLay->addWidget(m_group);

    // ---- таблица ----
    m_table = new QTableWidget(0, ColCount_onvifdlg, this);
    m_table->setHorizontalHeaderLabels({QString(), T("Адрес"),
                                        T("Имя / модель"), T("Состояние")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setColumnWidth(ColCheck_onvifdlg, 34);
    m_table->setColumnWidth(ColAddress_onvifdlg, 150);
    m_table->setColumnWidth(ColName_onvifdlg, 300);
    m_table->horizontalHeader()->setSectionResizeMode(ColState_onvifdlg, QHeaderView::Stretch);

    auto *selAll = new QPushButton(T("Выбрать все"), this);
    auto *selNone = new QPushButton(T("Снять выбор"), this);
    auto *selRow = new QHBoxLayout;
    selRow->addWidget(new QLabel(T("3. Найденные устройства"), this));
    selRow->addStretch(1);
    selRow->addWidget(selAll);
    selRow->addWidget(selNone);

    // ---- низ ----
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_status = new QLabel(T("Нажмите «Найти в своей сети» или задайте диапазон IP."), this);
    m_btnImport = new QPushButton(T("Добавить выбранные"), this);
    auto *close = new QPushButton(T("Закрыть"), this);

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_progress, 1);
    bottom->addWidget(m_btnImport);
    bottom->addWidget(close);

    auto *root = new QVBoxLayout(this);
    root->addWidget(findBox);
    root->addWidget(authBox);
    root->addLayout(selRow);
    root->addWidget(m_table, 1);
    root->addWidget(m_status);
    root->addLayout(bottom);

    // ---- сигналы ----
    connect(m_btnDiscover, &QPushButton::clicked, this, &OnvifImportDialog::startDiscovery);
    connect(m_btnScan, &QPushButton::clicked, this, &OnvifImportDialog::startScan);
    connect(m_btnImport, &QPushButton::clicked, this, &OnvifImportDialog::importSelected);
    connect(selAll, &QPushButton::clicked, this, [this] { setAllChecked(true); });
    connect(selNone, &QPushButton::clicked, this, [this] { setAllChecked(false); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    connect(m_discovery, &OnvifDiscovery::deviceFound, this, &OnvifImportDialog::addDevice);
    connect(m_discovery, &OnvifDiscovery::finished, this, [this] {
        m_progress->setRange(0, 1);
        m_progress->setValue(1);
        m_status->setText(T("Поиск завершён. Найдено устройств: %1").arg(m_devices.size()));
        setBusy(false);
    });

    connect(m_scanner, &OnvifScanner::deviceFound, this, &OnvifImportDialog::addDevice);
    connect(m_scanner, &OnvifScanner::progress, this, [this](int done, int total) {
        m_progress->setRange(0, total);
        m_progress->setValue(done);
        m_status->setText(T("Сканирование: %1 из %2, найдено: %3")
                              .arg(done).arg(total).arg(m_devices.size()));
    });
    connect(m_scanner, &OnvifScanner::finished, this, [this] {
        m_status->setText(T("Сканирование завершено. Найдено устройств: %1").arg(m_devices.size()));
        setBusy(false);
    });

    connect(m_resolver, &OnvifResolver::progress, this, [this](int done, int total) {
        m_progress->setRange(0, total);
        m_progress->setValue(done);
        m_status->setText(T("Опрос камер: %1 из %2").arg(done).arg(total));
    });
    connect(m_resolver, &OnvifResolver::resolved, this, [this](int i, const OnvifStreams &s) {
        const int row = m_importRows.at(i);
        if (!s.ok) {
            ++m_failed;
            m_table->item(row, ColState_onvifdlg)->setText(s.error);
            return;
        }
        CameraEntry e;
        e.name = displayName(row);
        e.group = m_group->text().trimmed();
        e.url = s.mainUrl;
        e.liveUrl = s.subUrl;
        e.record = false;
        if (s.hasPtz) {
            e.ptzXAddr = s.ptzXAddr;
            e.ptzToken = s.ptzToken;
            e.ptzUser = m_user->text().trimmed();
            e.ptzPassword = m_pass->text();
        }
        m_pending.append(e);
        m_pendingRows.append(row);
        QString status = s.subUrl.isEmpty() ? T("Получен 1 поток") : T("Получены основной поток и подпоток");
        if (s.hasPtz)
            status += QStringLiteral("  ·  ") + T("PTZ поддерживается");
        m_table->item(row, ColState_onvifdlg)->setText(status);
    });
    connect(m_resolver, &OnvifResolver::finished, this, [this] {
        if (!m_pending.isEmpty())
            m_mgr->addMany(m_pending);   // одно сохранение и один пересбор интерфейса на всех
        for (int row : std::as_const(m_pendingRows)) {
            m_table->item(row, ColState_onvifdlg)->setText(T("Добавлена"));
            m_table->item(row, ColCheck_onvifdlg)->setCheckState(Qt::Unchecked);
            m_table->item(row, ColCheck_onvifdlg)->setFlags(Qt::NoItemFlags);
            m_existingHosts.insert(m_devices.at(row).address);
        }
        m_status->setText(T("Добавлено камер: %1, с ошибкой: %2%3")
                              .arg(m_pending.size()).arg(m_failed)
                              .arg(m_failed ? T(" (исправьте логин/пароль и повторите)") : QString()));
        m_pending.clear();
        m_pendingRows.clear();
        setBusy(false);
    });
}

QString OnvifImportDialog::displayName(int row) const
{
    const OnvifDevice &d = m_devices.at(row);
    QString base = d.name.isEmpty() ? d.hardware : d.name;
    return base.isEmpty() ? d.address : QStringLiteral("%1 (%2)").arg(base, d.address);
}

void OnvifImportDialog::addDevice(const OnvifDevice &dev)
{
    for (const OnvifDevice &d : std::as_const(m_devices))
        if (d.address == dev.address)
            return;

    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_devices.append(dev);

    const bool exists = m_existingHosts.contains(dev.address);

    auto *chk = new QTableWidgetItem;
    chk->setFlags(exists ? Qt::NoItemFlags : (Qt::ItemIsUserCheckable | Qt::ItemIsEnabled));
    chk->setCheckState(exists ? Qt::Unchecked : Qt::Checked);
    m_table->setItem(row, ColCheck_onvifdlg, chk);
    m_table->setItem(row, ColAddress_onvifdlg, new QTableWidgetItem(dev.address));

    QString title = dev.name;
    if (!dev.hardware.isEmpty())
        title += title.isEmpty() ? dev.hardware : QStringLiteral(" (%1)").arg(dev.hardware);
    m_table->setItem(row, ColName_onvifdlg, new QTableWidgetItem(title));
    m_table->setItem(row, ColState_onvifdlg,
                     new QTableWidgetItem(exists ? T("Уже добавлена") : T("Найдена")));
}

void OnvifImportDialog::setAllChecked(bool on)
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem *it = m_table->item(row, ColCheck_onvifdlg);
        if (it->flags() & Qt::ItemIsUserCheckable)
            it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
}

void OnvifImportDialog::setBusy(bool busy)
{
    m_btnDiscover->setEnabled(!busy);
    m_btnScan->setEnabled(!busy);
    m_btnImport->setEnabled(!busy);
}

void OnvifImportDialog::startDiscovery()
{
    setBusy(true);
    m_progress->setRange(0, 0);   // «бегущая» полоса
    m_status->setText(T("Поиск камер в сети…"));
    m_discovery->start(4000);
}

void OnvifImportDialog::startScan()
{
    quint32 a = 0, b = 0;
    if (!parseIpv4_onvifdlg(m_from->text(), &a) || !parseIpv4_onvifdlg(m_to->text(), &b) || b < a) {
        QMessageBox::warning(this, windowTitle(), T("Укажите корректный диапазон IPv4-адресов."));
        return;
    }
    if (quint64(b) - a + 1 > kMaxRange_onvifdlg) {
        QMessageBox::warning(this, windowTitle(),
            T("Слишком большой диапазон: максимум %1 адресов за раз.").arg(kMaxRange_onvifdlg));
        return;
    }
    setBusy(true);
    m_progress->setRange(0, int(b - a + 1));
    m_progress->setValue(0);
    m_scanner->start(a, b, quint16(m_port->value()), 64);
}

void OnvifImportDialog::importSelected()
{
    if (m_user->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), T("Укажите логин камер."));
        return;
    }

    m_importRows.clear();
    m_pending.clear();
    m_pendingRows.clear();
    m_failed = 0;

    QList<OnvifRequest> requests;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *chk = m_table->item(row, ColCheck_onvifdlg);
        if (!(chk->flags() & Qt::ItemIsUserCheckable) || chk->checkState() != Qt::Checked)
            continue;
        requests.append({m_devices.at(row).xaddr, m_user->text().trimmed(), m_pass->text()});
        m_importRows.append(row);
        m_table->item(row, ColState_onvifdlg)->setText(T("Запрос…"));
    }
    if (requests.isEmpty()) {
        QMessageBox::information(this, windowTitle(), T("Отметьте камеры для добавления."));
        return;
    }

    setBusy(true);
    m_progress->setRange(0, int(requests.size()));
    m_progress->setValue(0);
    m_resolver->start(requests, 8);   // одновременно опрашиваем 8 камер
}

// ======================================================================
