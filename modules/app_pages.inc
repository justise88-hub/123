// PSVms module: Страницы устройств, событий, поиска и просмотра
// This file is included by psvms.cpp to preserve the original single-TU build
// while the code is being separated safely.

// app/devices_page.cpp
// ======================================================================
namespace {

enum DevColumn { DevColName, DevColGroup, DevColAddress, DevColLink, DevColRecord, DevColCount };

QStringList cameraGroups(const CameraManager *mgr)
{
    QStringList g;
    for (const CameraEntry &e : mgr->cameras())
        if (!e.group.isEmpty() && !g.contains(e.group))
            g << e.group;
    return g;
}

// Подгоняет размер шрифта всех кнопок в разделах управления камерами
// под фактическую ширину виджета. Пересчитывается при каждом изменении размера.
class CameraButtonAutoFit final : public QObject {
public:
    explicit CameraButtonAutoFit(QObject *parent = nullptr) : QObject(parent) {}

    void install(QWidget *root)
    {
        if (!root) return;
        const auto buttons = root->findChildren<QAbstractButton *>();
        for (QAbstractButton *button : buttons) {
            if (!button || button->property("psvms.autoFitInstalled").toBool()) continue;
            button->setProperty("psvms.autoFitInstalled", true);
            button->setProperty("psvms.autoFitBaseFont", button->font());
            button->installEventFilter(this);
            fit(button);
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
            event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
            event->type() == QEvent::LayoutRequest) {
            if (auto *button = qobject_cast<QAbstractButton *>(watched)) {
                // setFont() ниже генерирует FontChange. Не запускаем fit() повторно
                // из этого вложенного события: иначе при первом show() возможна
                // бесконечная рекурсия и Windows SEH 0xC00000FD (Stack Overflow).
                if (!button->property("psvms.autoFitInProgress").toBool())
                    fit(button);
            }
        } else if (event->type() == QEvent::Paint) {
            if (auto *button = qobject_cast<QAbstractButton *>(watched)) {
                if (button->property("psvms.autoFitLastText").toString() != button->text())
                    fit(button);
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    static void fit(QAbstractButton *button)
    {
        if (!button || button->width() < 24 ||
            button->property("psvms.autoFitInProgress").toBool())
            return;

        button->setProperty("psvms.autoFitInProgress", true);
        [[maybe_unused]] const auto fitGuard = qScopeGuard([button] {
            if (button)
                button->setProperty("psvms.autoFitInProgress", false);
        });

        const QFont base = button->property("psvms.autoFitBaseFont").value<QFont>();
        if (base.family().isEmpty() && base.pointSizeF() <= 0) return;
        QFont font = base;
        const int margins = button->contentsMargins().left() + button->contentsMargins().right();
        int iconWidth = button->icon().isNull() ? 0 : button->iconSize().width() + 6;
        const int available = qMax(12, button->width() - margins - iconWidth - 18);
        const QString label = button->text().replace('&', "");
        button->setProperty("psvms.autoFitLastText", button->text());
        const qreal minimumPointSize = button->property("psvms.keepCommonFont").toBool()
            ? 10.0 : 8.0;
        for (qreal point = base.pointSizeF() > 0 ? base.pointSizeF() : 9.0;
             point >= minimumPointSize; point -= 0.5) {
            font.setPointSizeF(point);
            if (QFontMetricsF(font).horizontalAdvance(label) <= available) break;
        }
        // Не посылаем FontChange без необходимости. В паре с флагом выше это
        // исключает повторный вход при первом показе и при изменении размеров.
        if (button->font() != font)
            button->setFont(font);
    }
};

void installCameraButtonAutoFit(QWidget *root)
{
    auto *fit = new CameraButtonAutoFit(root);
    fit->install(root);
}

} // namespace

DevicesPage::DevicesPage(CameraManager *mgr, QWidget *parent) : QWidget(parent), m_mgr(mgr)
{
    // ---- «Добавить» с выпадающим меню ----
    m_addBtn = new QToolButton(this);
    m_addBtn->setText(T("Добавить"));
    m_addBtn->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(m_addBtn);
    menu->setToolTipsVisible(true);
    QMenu *guided = menu->addMenu(T("Быстрое добавление"));
    guided->setIcon(makeCellIcon(QStringLiteral("plus")));
    QMenu *network = guided->addMenu(T("Обнаружение в сети"));
    network->setToolTip(T("Поиск камер и регистраторов в вашей сети"));
    connect(network->addAction(T("Автоматический поиск (SADP + ONVIF)…")), &QAction::triggered, this, [this] {
        LanSearchDialog dlg(m_mgr, this);
        dlg.exec();
    });
    connect(network->addAction(T("Добавить по IP / производителю…")), &QAction::triggered, this, [this] {
        HikIpDialog dlg(m_mgr, this);
        dlg.exec();
    });
    connect(guided->addAction(T("По RTSP-адресу…")), &QAction::triggered, this, [this] {
        RtspQuickDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted)
            m_mgr->add(dlg.entry());
    });
    connect(guided->addAction(T("Аналоговую камеру (карта видеозахвата)…")), &QAction::triggered, this, [this] {
        AnalogCameraDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted)
            m_mgr->add(dlg.entry());
    });
    menu->addSeparator();
    QMenu *importMenu = menu->addMenu(T("Импорт и совместимость"));
    connect(importMenu->addAction(T("Импорт по ONVIF…")), &QAction::triggered, this, [this] {
        OnvifImportDialog dlg(m_mgr, this);
        dlg.exec();
    });
    connect(importMenu->addAction(T("Камеры iCSee…")), &QAction::triggered, this, [this] {
        IcseeImportDialog dlg(m_mgr, this);
        dlg.exec();
    });
    m_addBtn->setMenu(menu);

    m_edit = new QPushButton(T("Изменить"), this);
    auto *openWebBtn = new QPushButton(T("Веб-интерфейс"), this);
    openWebBtn->setToolTip(T("Открыть веб-страницу выбранной камеры или регистратора в браузере"));
    auto *renameGroupBtn = new QPushButton(T("Переименовать группу…"), this);
    auto *removeGroupBtn = new QPushButton(T("Удалить группу…"), this);
    m_remove = new QPushButton(T("Удалить камеру"), this);
    m_removeAll = new QPushButton(T("Удалить все камеры"), this);
    m_removeAll->setToolTip(T("Удалить все камеры и каналы регистраторов из списка PSVms"));
    auto *start = new QPushButton(T("Запустить запись"), this);
    auto *stop = new QPushButton(T("Остановить запись"), this);
    auto *deviceFilter = new QLineEdit(this);
    deviceFilter->setPlaceholderText(T("Поиск по названию, IP или группе…"));
    deviceFilter->setClearButtonEnabled(true);
    deviceFilter->setMinimumWidth(210);
    start->setToolTip(T("Для выделенных камер; если ничего не выделено — для всех"));
    stop->setToolTip(T("Для выделенных камер; если ничего не выделено — для всех"));

    // Разносим длинные подписи по отдельным строкам: кнопки не сжимаются
    // при узком окне, а текст остаётся полностью читаемым.
    auto *toolbar = new QVBoxLayout;
    toolbar->setSpacing(8);

    m_addBtn->setText(T("＋  Добавить устройство"));
    m_addBtn->setToolTip(T("Добавить IP-камеру, регистратор или устройство видеозахвата"));
    m_addBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);

    const QList<QAbstractButton *> deviceButtons{
        m_addBtn, m_edit, openWebBtn, renameGroupBtn, removeGroupBtn,
        m_remove, m_removeAll, start, stop
    };
    for (QAbstractButton *button : deviceButtons) {
        button->setMinimumHeight(38);
        // Тот же шрифт, что и в левом меню: Segoe UI 10 pt DemiBold.
        // Для надписей кнопок записи размер не уменьшается отдельно.
        QFont commonUiFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold);
        button->setFont(commonUiFont);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // Не фиксируем широкие минимумы: две кнопки в строке должны
        // делить доступное место, а CameraButtonAutoFit уменьшит подпись
        // до читаемого предела при узкой ширине окна.
        button->setMinimumWidth(0);
    }

    renameGroupBtn->setText(T("Переименовать группу"));
    removeGroupBtn->setText(T("Удалить группу"));
    // Ширина распределяется layout-ом; длинные подписи подгоняются
    // автоматически и не выталкивают соседние кнопки за границы панели.
    deviceFilter->setMinimumWidth(170);

    auto *row1 = new QHBoxLayout;
    row1->setSpacing(8);
    row1->addWidget(m_addBtn, 1);
    row1->addWidget(m_edit, 1);
    row1->addWidget(openWebBtn, 1);
    connect(openWebBtn, &QPushButton::clicked, this, [this] {
        const QString id = currentId();
        const CameraEntry *camera = m_mgr->find(id);
        if (!camera) {
            QMessageBox::information(this, T("Веб-интерфейс"), T("Сначала выберите камеру или регистратор."));
            return;
        }
        QString address = camera->webUrl.trimmed();
        if (address.isEmpty()) {
            const QUrl stream(camera->url);
            if (!stream.host().isEmpty()) address = QStringLiteral("http://%1").arg(stream.host());
        }
        const QUrl web(address);
        if (!web.isValid() || (web.scheme() != QLatin1String("http") && web.scheme() != QLatin1String("https"))) {
            QMessageBox::information(this, T("Веб-интерфейс"), T("Укажите HTTP/HTTPS-адрес в настройках камеры или регистратора."));
            return;
        }
        QDesktopServices::openUrl(web);
    });

    auto *row2 = new QHBoxLayout;
    row2->setSpacing(8);
    row2->addWidget(renameGroupBtn, 1);
    row2->addWidget(removeGroupBtn, 1);

    auto *row3 = new QHBoxLayout;
    row3->setSpacing(8);
    row3->addWidget(m_remove, 1);
    row3->addWidget(m_removeAll, 1);

    // Фильтр занимает отдельную строку, чтобы две длинные кнопки записи
    // не делили с ним ширину и всегда показывали подписи целиком.
    auto *row4 = new QHBoxLayout;
    row4->setSpacing(8);
    row4->addWidget(deviceFilter, 1);

    auto *row5 = new QHBoxLayout;
    row5->setSpacing(8);
    start->setMinimumHeight(40);
    stop->setMinimumHeight(40);
    start->setMinimumWidth(140);
    stop->setMinimumWidth(140);
    start->setProperty("psvms.keepCommonFont", true);
    stop->setProperty("psvms.keepCommonFont", true);
    row5->addWidget(start, 1);
    row5->addWidget(stop, 1);

    toolbar->addLayout(row1);
    toolbar->addLayout(row2);
    toolbar->addLayout(row3);
    toolbar->addLayout(row4);
    toolbar->addLayout(row5);
    auto *retentionButton = new QPushButton(T("Расчёт срока хранения архива по группам…"), this);
    retentionButton->setMinimumHeight(36);
    toolbar->addWidget(retentionButton);
    connect(retentionButton, &QPushButton::clicked, this, [this] {
        QDialog dlg(this);
        dlg.setWindowTitle(T("Расчёт срока хранения архива"));
        dlg.resize(760, 460);
        auto *table = new QTableWidget(0, 6, &dlg);
        table->setHorizontalHeaderLabels({T("Группа"), T("Камер"), T("Битрейт, Мбит/с"), T("Кодеки"), T("Расход, ГБ/сутки"), T("Срок на общем HDD")});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->verticalHeader()->setVisible(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        QMap<QString, QList<const CameraEntry *>> groups;
        for (const CameraEntry &camera : m_mgr->cameras())
            groups[camera.group.trimmed().isEmpty() ? T("Без группы") : camera.group].append(&camera);
        const QStorageInfo storage(m_mgr->storageRoot());
        const qint64 freeBytes = storage.isValid() && storage.isReady() ? storage.bytesAvailable() : -1;
        qint64 totalKbps = 0;
        for (auto it = groups.cbegin(); it != groups.cend(); ++it)
            for (const CameraEntry *camera : it.value()) totalKbps += qMax(64, camera->bitrateKbps);
        const auto retentionText = [freeBytes, totalKbps](qint64) {
            if (freeBytes <= 0 || totalKbps <= 0) return T("Нет данных");
            const long double bytesPerSecond = static_cast<long double>(totalKbps) * 1000.0L / 8.0L;
            const qint64 seconds = qint64(static_cast<long double>(freeBytes) / bytesPerSecond);
            const qint64 days = seconds / 86400;
            const qint64 hours = (seconds % 86400) / 3600;
            return T("%1 дн. %2 ч.").arg(days).arg(hours);
        };
        auto *disk = new QLabel(T("Свободно на диске локального архива: %1 ГБ").arg(freeBytes > 0 ? QString::number(freeBytes / 1073741824.0, 'f', 2) : T("не определено")), &dlg);
        int row = 0;
        for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
            qint64 sum = 0;
            QSet<QString> codecs;
            for (const CameraEntry *camera : it.value()) {
                sum += qMax(64, camera->bitrateKbps);
                codecs.insert(camera->codec.isEmpty() ? T("Не указан") : camera->codec);
            }
            const long double gbPerDay = static_cast<long double>(sum) * 1000.0L / 8.0L * 86400.0L / 1073741824.0L;
            table->insertRow(row);
            table->setItem(row, 0, new QTableWidgetItem(it.key()));
            table->setItem(row, 1, new QTableWidgetItem(QString::number(it.value().size())));
            table->setItem(row, 2, new QTableWidgetItem(QString::number(sum / 1000.0, 'f', 2)));
            QStringList codecNames;
            for (const QString &codecName : codecs) codecNames << codecName;
            table->setItem(row, 3, new QTableWidgetItem(codecNames.join(QStringLiteral(", "))));
            table->setItem(row, 4, new QTableWidgetItem(QString::number(double(gbPerDay), 'f', 2)));
            table->setItem(row, 5, new QTableWidgetItem(retentionText(sum)));
            ++row;
        }
        const long double totalGbPerDay = static_cast<long double>(totalKbps) * 1000.0L / 8.0L * 86400.0L / 1073741824.0L;
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(T("ВСЕ КАМЕРЫ (общий архив)")));
        table->setItem(row, 1, new QTableWidgetItem(QString::number(m_mgr->cameras().size())));
        table->setItem(row, 2, new QTableWidgetItem(QString::number(totalKbps / 1000.0, 'f', 2)));
        table->setItem(row, 3, new QTableWidgetItem(T("Суммарно")));
        table->setItem(row, 4, new QTableWidgetItem(QString::number(double(totalGbPerDay), 'f', 2)));
        table->setItem(row, 5, new QTableWidgetItem(retentionText(totalKbps)));
        auto *note = new QLabel(T("Срок в последнем столбце рассчитан для общего свободного места HDD при одновременной записи ВСЕХ камер. Поэтому срок одинаков для групп, использующих общий диск; расход каждой группы в ГБ/сутки показан отдельно. Если у регистратора отдельные HDD-квоты/группы дисков, укажите соответствующую ёмкость отдельно. Битрейт — заданный или считанный верхний предел, фактический срок зависит от VBR, аудио, движения, расписания и служебных данных. Кодек не умножается на условный коэффициент экономии."), &dlg);
        note->setWordWrap(true);
        auto *close = new QPushButton(T("Закрыть"), &dlg);
        QObject::connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
        auto *layout = new QVBoxLayout(&dlg);
        layout->addWidget(disk);
        layout->addWidget(table, 1);
        layout->addWidget(note);
        layout->addWidget(close, 0, Qt::AlignRight);
        dlg.exec();
    });

    m_table = new QTableWidget(0, DevColCount, this);
    m_table->setHorizontalHeaderLabels({T("Название"), T("Группа"), T("Адрес"), T("Связь"), T("Запись")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->verticalHeader()->setDefaultSectionSize(38);
    m_table->horizontalHeader()->setSectionResizeMode(DevColAddress, QHeaderView::Stretch);
    m_table->setColumnWidth(DevColName, 240);
    m_table->setColumnWidth(DevColGroup, 200);
    m_table->setColumnWidth(DevColLink, 110);
    m_table->setColumnWidth(DevColRecord, 220);
    m_table->setMouseTracking(true);
    m_table->viewport()->setMouseTracking(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    new CameraHoverPreview(m_table, m_mgr, m_table);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 12, 16, 12);
    lay->addLayout(toolbar);
    lay->addWidget(m_table, 1);

    connect(m_edit, &QPushButton::clicked, this, &DevicesPage::editCamera);
    connect(m_remove, &QPushButton::clicked, this, &DevicesPage::removeCamera);
    connect(m_removeAll, &QPushButton::clicked, this, [this] {
        if (!Users::current().isAdmin())
            return;
        const QStringList ids = [&] {
            QStringList all;
            for (const CameraEntry &entry : m_mgr->cameras())
                all << entry.id;
            return all;
        }();
        if (ids.isEmpty())
            return;
        QMessageBox box(QMessageBox::Warning, T("Удалить все камеры"),
            T("Удалить ВСЕ камеры и каналы регистраторов (%1) из PSVms?\nВсе потоки и запись будут остановлены. Уже сохранённые файлы архива останутся на диске. Это действие нельзя отменить.").arg(ids.size()),
            QMessageBox::NoButton, this);
        QPushButton *removeButton = box.addButton(T("Удалить все"), QMessageBox::DestructiveRole);
        box.addButton(T("Отмена"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() != removeButton)
            return;
        for (const QString &id : ids)
            m_mgr->remove(id);
    });
    connect(removeGroupBtn, &QPushButton::clicked, this, &DevicesPage::removeGroup);
    connect(renameGroupBtn, &QPushButton::clicked, this, [this] {
        const QStringList groups = cameraGroups(m_mgr);
        if (groups.isEmpty()) {
            QMessageBox::information(this, T("Переименовать группу"), T("Групп пока нет: укажите группу хотя бы у одной камеры."));
            return;
        }
        bool ok = false;
        const QString oldName = QInputDialog::getItem(this, T("Переименовать группу"), T("Группа:"), groups, 0, false, &ok);
        if (!ok || oldName.isEmpty())
            return;
        QString newName;
        {
            QInputDialog dlg(this);
            dlg.setWindowTitle(T("Переименовать группу"));
            dlg.setLabelText(T("Новое название группы «%1»:").arg(oldName));
            dlg.setTextValue(oldName);
            dlg.setOkButtonText(T("ОК"));
            dlg.setCancelButtonText(T("Отмена"));
            if (dlg.exec() != QDialog::Accepted)
                return;
            newName = dlg.textValue().trimmed();
        }
        if (newName.isEmpty() || newName == oldName)
            return;
        for (const CameraEntry &e : m_mgr->cameras()) {
            if (e.group == oldName) {
                CameraEntry c = e;
                c.group = newName;
                m_mgr->update(c);
            }
        }
    });
    connect(start, &QPushButton::clicked, this, &DevicesPage::startSelected);
    connect(stop, &QPushButton::clicked, this, &DevicesPage::stopSelected);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this] { editCamera(); });
    connect(deviceFilter, &QLineEdit::textChanged, this, [this](const QString &query) {
        const QString needle = query.trimmed();
        for (int row = 0; row < m_table->rowCount(); ++row) {
            bool match = needle.isEmpty();
            for (int col = 0; !match && col < m_table->columnCount(); ++col) {
                const QTableWidgetItem *item = m_table->item(row, col);
                match = item && item->text().contains(needle, Qt::CaseInsensitive);
            }
            m_table->setRowHidden(row, !match);
        }
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &DevicesPage::updateButtons);
    connect(m_table, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex index = m_table->indexAt(pos);
        if (!index.isValid()) return;
        if (!m_table->selectionModel()->isRowSelected(index.row(), QModelIndex()))
            m_table->selectRow(index.row());
        const QString id = m_table->item(index.row(), DevColName)->data(Qt::UserRole).toString();
        const CameraEntry *entry = m_mgr->find(id);
        if (!entry) return;
        QMenu menu(m_table);
        QAction *editAction = menu.addAction(T("Изменить камеру…"));
        QAction *webAction = menu.addAction(T("Открыть веб-интерфейс устройства"));
        QAction *deleteAction = menu.addAction(T("Удалить камеру"));
        QAction *deleteGroupAction = nullptr;
        if (!entry->group.trimmed().isEmpty()) {
            menu.addSeparator();
            deleteGroupAction = menu.addAction(T("Удалить группу «%1»…").arg(entry->group));
        }
        editAction->setEnabled(Users::current().isAdmin());
        deleteAction->setEnabled(Users::current().isAdmin());
        if (deleteGroupAction) deleteGroupAction->setEnabled(Users::current().isAdmin());
        QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
        if (chosen == editAction) editCamera();
        else if (chosen == webAction) {
            QString address = entry->webUrl.trimmed();
            if (address.isEmpty()) {
                const QUrl stream(entry->url);
                if (!stream.host().isEmpty())
                    address = QStringLiteral("http://%1").arg(stream.host());
            }
            const QUrl web(address);
            if (!web.isValid() || (web.scheme() != QLatin1String("http") && web.scheme() != QLatin1String("https"))) {
                QMessageBox::information(this, T("Веб-интерфейс"),
                    T("Укажите HTTP/HTTPS-адрес в настройках камеры или регистратора."));
            } else {
                QDesktopServices::openUrl(web);
            }
        }
        else if (chosen == deleteAction) removeCamera();
        else if (chosen == deleteGroupAction) removeGroup();
    });
    connect(m_mgr, &CameraManager::camerasChanged, this, &DevicesPage::rebuild);
    connect(m_mgr, &CameraManager::cameraStateChanged, this, &DevicesPage::refreshStates);
    connect(m_mgr, &CameraManager::onlineChanged, this, &DevicesPage::refreshStates);

    rebuild();
    installCameraButtonAutoFit(this);
}

QString DevicesPage::currentId() const
{
    const int row = m_table->currentRow();
    if (row < 0)
        return QString();
    const QTableWidgetItem *it = m_table->item(row, DevColName);
    return it ? it->data(Qt::UserRole).toString() : QString();
}

QStringList DevicesPage::selectedIds() const
{
    QStringList ids;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows(DevColName);
    for (const QModelIndex &idx : rows)
        ids << m_table->item(idx.row(), DevColName)->data(Qt::UserRole).toString();
    return ids;
}

void DevicesPage::updateButtons()
{
    const bool has = !selectedIds().isEmpty();
    m_edit->setEnabled(has);
    m_remove->setEnabled(has && Users::current().isAdmin());
    m_removeAll->setEnabled(!m_mgr->cameras().isEmpty() && Users::current().isAdmin());
}

void DevicesPage::rebuild()
{
    const QStringList keep = selectedIds();
    m_table->setUpdatesEnabled(false);
    m_table->setRowCount(0);

    for (const CameraEntry &e : m_mgr->cameras()) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        auto *name = new QTableWidgetItem(e.name);
        name->setData(Qt::UserRole, e.id);
        m_table->setItem(row, DevColName, name);
        m_table->setItem(row, DevColGroup, new QTableWidgetItem(e.group));

        // логин и пароль на экране не показываем (у iCSee пароль передаётся в самом пути)
        QString addr = QUrl(e.url).toString(QUrl::RemoveUserInfo);
        addr.replace(QRegularExpression(QStringLiteral("password=[^&]*")), QStringLiteral("password=***"));
        m_table->setItem(row, DevColAddress, new QTableWidgetItem(addr));
        m_table->setItem(row, DevColLink, new QTableWidgetItem());
        m_table->setItem(row, DevColRecord, new QTableWidgetItem());
        for (int col = 0; col < DevColCount; ++col) {
            if (auto *item = m_table->item(row, col)) item->setToolTip(item->text());
        }
        m_table->setRowHeight(row, 38);
        if (keep.contains(e.id))
            m_table->selectRow(row);
    }
    m_table->setUpdatesEnabled(true);
    refreshStates();
    updateButtons();
}

void DevicesPage::refreshStates()
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QString id = m_table->item(row, DevColName)->data(Qt::UserRole).toString();
        const CameraEntry *e = m_mgr->find(id);
        if (!e)
            continue;
        const bool online = m_mgr->isOnline(id);
        QTableWidgetItem *link = m_table->item(row, DevColLink);
        link->setText(online ? T("● В сети") : T("● Не в сети"));
        link->setForeground(QBrush(QColor(online ? "#63e6ee" : "#63e6ee")));

        QString rec;
        if (!e->record)
            rec = T("Запись выключена");
        else if (!m_mgr->isStarted(id))
            rec = T("Не запущена");
        else
            rec = stateText(m_mgr->stateOf(id));
        m_table->item(row, DevColRecord)->setText(rec);
    }
}

void DevicesPage::editCamera()
{
    const CameraEntry *cur = m_mgr->find(currentId());
    if (!cur)
        return;
    CameraDialog dlg(*cur, cameraGroups(m_mgr), this);
    if (dlg.exec() == QDialog::Accepted)
        m_mgr->update(dlg.entry());
}

void DevicesPage::removeCamera()
{
    const QStringList ids = selectedIds();
    if (ids.isEmpty())
        return;
    // кнопки подписываем сами: стандартные Yes/No у Qt всегда английские
    const QString text = ids.size() == 1
        ? T("Удалить камеру «%1»?\nУже записанные файлы архива останутся на диске.").arg(m_mgr->find(ids.first()) ? m_mgr->find(ids.first())->name : QString())
        : T("Удалить выбранные камеры (%1)?\nУже записанные файлы архива останутся на диске.").arg(ids.size());
    QMessageBox box(QMessageBox::Question, T("Удаление камеры"), text, QMessageBox::NoButton, this);
    QPushButton *yes = box.addButton(T("Удалить"), QMessageBox::AcceptRole);
    box.addButton(T("Отмена"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != yes)
        return;
    for (const QString &id : ids)
        m_mgr->remove(id);
}

void DevicesPage::removeGroup()
{
    if (!Users::current().isAdmin()) return;
    const QStringList groups = cameraGroups(m_mgr);
    if (groups.isEmpty()) {
        QMessageBox::information(this, T("Удалить группу"), T("Групп для удаления нет."));
        return;
    }
    bool ok = false;
    const QString group = QInputDialog::getItem(this, T("Удалить группу"), T("Группа:"), groups, 0, false, &ok);
    if (!ok || group.isEmpty()) return;
    int count = 0;
    for (const CameraEntry &e : m_mgr->cameras()) if (e.group == group) ++count;
    QMessageBox box(QMessageBox::Warning, T("Удаление группы"),
        T("Удалить группу «%1» и все камеры в ней (%2)? Это остановит потоки; файлы архива останутся на диске.").arg(group).arg(count),
        QMessageBox::NoButton, this);
    auto *yes = box.addButton(T("Удалить группу и камеры"), QMessageBox::DestructiveRole);
    box.addButton(T("Отмена"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != yes) return;
    QStringList ids;
    for (const CameraEntry &e : m_mgr->cameras()) if (e.group == group) ids << e.id;
    for (const QString &id : ids) m_mgr->remove(id);
}

void DevicesPage::startSelected()
{
    const QStringList ids = selectedIds();
    if (ids.isEmpty()) {
        m_mgr->startAll();
        return;
    }
    for (const QString &id : ids)
        m_mgr->startCamera(id);
}

void DevicesPage::stopSelected()
{
    const QStringList ids = selectedIds();
    if (ids.isEmpty()) {
        m_mgr->stopAll();
        return;
    }
    for (const QString &id : ids)
        m_mgr->stopCamera(id);
}

// ======================================================================
// app/events_page.cpp
// ======================================================================
EventsPage::EventsPage(QWidget *parent) : QWidget(parent)
{
    auto *ackSelected = new QPushButton(T("Подтвердить выбранную тревогу"), this);
    auto *ackAll = new QPushButton(T("Подтвердить все"), this);
    auto *clear = new QPushButton(T("Очистить журнал"), this);
    for (QPushButton *button : {ackSelected, ackAll, clear}) {
        button->setMinimumHeight(34);
        button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    }
    auto *filterEdit = new QLineEdit(this);
    filterEdit->setPlaceholderText(T("Фильтр по камере, событию или статусу…"));
    filterEdit->setClearButtonEnabled(true);
    filterEdit->setMinimumHeight(34);
    filterEdit->setMaximumWidth(320);

    auto *ackCommentEdit = new QLineEdit(this);
    ackCommentEdit->setPlaceholderText(T("Комментарий оператора при подтверждении…"));
    ackCommentEdit->setMinimumHeight(34);
    ackCommentEdit->setMaximumWidth(300);

    auto *top = new QHBoxLayout;
    top->addWidget(ackSelected);
    top->addWidget(ackAll);
    top->addWidget(ackCommentEdit);
    top->addWidget(filterEdit);
    top->addStretch(1);
    top->addWidget(clear);

    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels(
        {T("Время"), T("Камера"), T("Событие"), T("Обработка")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 170);
    m_table->setColumnWidth(1, 190);
    m_table->setColumnWidth(3, 150);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->addLayout(top);
    lay->addWidget(m_table, 1);

    connect(ackSelected, &QPushButton::clicked, this, [this, ackCommentEdit] {
        const int row = m_table->currentRow();
        if (row < 0) return;
        const QString note = ackCommentEdit->text().trimmed();
        if (!note.isEmpty()) {
            if (auto *evItem = m_table->item(row, 2)) {
                const QString marker = QStringLiteral(" [") + note + QStringLiteral("]");
                if (!evItem->text().endsWith(marker)) {
                    evItem->setText(evItem->text() + marker);
                    evItem->setToolTip(evItem->text());
                }
            }
            ackCommentEdit->clear();
        }
        acknowledgeRow(row);
    });
    connect(filterEdit, &QLineEdit::textChanged, this, [this](const QString &q) {
        const QString needle = q.trimmed();
        for (int r = 0; r < m_table->rowCount(); ++r) {
            bool match = needle.isEmpty();
            if (!match) {
                for (int c = 0; c < m_table->columnCount(); ++c) {
                    const auto *it = m_table->item(r, c);
                    if (it && it->text().contains(needle, Qt::CaseInsensitive)) {
                        match = true;
                        break;
                    }
                }
            }
            m_table->setRowHidden(r, !match);
        }
    });
    connect(ackAll, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < m_table->rowCount(); ++row)
            acknowledgeRow(row);
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        m_table->setRowCount(0);
        m_history.clear();
        m_historyDirty = false;
        m_persistTimer->stop();
        QSettings().remove(QStringLiteral("events/history"));
    });
    m_history = QSettings().value(QStringLiteral("events/history")).toStringList();
    while (m_history.size() > 500) m_history.removeLast();
    for (const QString &line : m_history) {
        const QStringList parts = line.split(QChar(0x1f));
        if (parts.size() != 3) continue;
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(QDateTime::fromString(parts.at(0), Qt::ISODate).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        m_table->setItem(row, 1, new QTableWidgetItem(parts.at(1)));
        m_table->setItem(row, 2, new QTableWidgetItem(parts.at(2)));
        const bool acknowledged = parts.size() >= 4 && parts.at(3) == QStringLiteral("1");
        m_table->setItem(row, 3, new QTableWidgetItem(acknowledged ? T("Подтверждено") : T("Не обработано")));
        const QString msg = parts.at(2);
        if (msg.contains(QStringLiteral("ошиб"), Qt::CaseInsensitive) || msg.contains(QStringLiteral("не в сети"), Qt::CaseInsensitive) || msg.contains(QStringLiteral("нет связи"), Qt::CaseInsensitive) || msg.contains(QStringLiteral("отключ"), Qt::CaseInsensitive) || msg.contains(QStringLiteral("error"), Qt::CaseInsensitive))
            for (int col = 0; col < 3; ++col) m_table->item(row, col)->setForeground(QColor(QStringLiteral("#ff626d")));
    }
    // Запись журнала выполняется пакетно, а не на каждый сетевой сигнал:
    // это исключает постоянные синхронные обращения к диску при потере связи.
    m_persistTimer = new QTimer(this);
    m_persistTimer->setSingleShot(true);
    m_persistTimer->setInterval(700);
    connect(m_persistTimer, &QTimer::timeout, this, [this] {
        if (!m_historyDirty) return;
        QSettings settings;
        settings.setValue(QStringLiteral("events/history"), m_history);
        settings.sync();
        m_historyDirty = false;
    });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
        if (!m_historyDirty) return;
        QSettings settings;
        settings.setValue(QStringLiteral("events/history"), m_history);
        settings.sync();
        m_historyDirty = false;
    });
}

void EventsPage::addEvent(const QString &camera, const QString &text)
{
    // Ограничиваем размер видимой таблицы и истории, чтобы журнал не разрастался.
    if (m_table->rowCount() >= 500) m_table->removeRow(m_table->rowCount() - 1);
    m_table->insertRow(0);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_table->setItem(0, 0, new QTableWidgetItem(stamp));
    m_table->setItem(0, 1, new QTableWidgetItem(camera));
    m_table->setItem(0, 2, new QTableWidgetItem(text));
    m_table->setItem(0, 3, new QTableWidgetItem(T("Не обработано")));
    for (int col = 0; col < 4; ++col) {
        if (auto *item = m_table->item(0, col)) {
            item->setToolTip(item->text());
            item->setTextAlignment(col == 0 ? Qt::AlignCenter : Qt::AlignVCenter | Qt::AlignLeft);
        }
    }
    const bool error = text.contains(QStringLiteral("ошиб"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("error"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("не в сети"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("нет связи"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("потеря связи"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("отключ"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("failed"), Qt::CaseInsensitive);
    if (error) for (int col = 0; col < 3; ++col)
        m_table->item(0, col)->setForeground(QColor(QStringLiteral("#ff626d")));

    m_history.prepend(QDateTime::currentDateTime().toString(Qt::ISODate)
                      + QChar(0x1f) + camera + QChar(0x1f) + text + QChar(0x1f) + QStringLiteral("0"));
    while (m_history.size() > 500) m_history.removeLast();
    m_historyDirty = true;
    m_persistTimer->start();
}

void EventsPage::acknowledgeRow(int row)
{
    if (!canCurrentUserPerformAction(QStringLiteral("events.ack")))
        return;
    if (row < 0 || row >= m_table->rowCount())
        return;
    auto *status = m_table->item(row, 3);
    if (!status || status->text() == T("Подтверждено"))
        return;
    status->setText(T("Подтверждено"));
    status->setForeground(QColor(QStringLiteral("#78d7a5")));
    saveHistoryFromTable();
}

void EventsPage::saveHistoryFromTable()
{
    m_history.clear();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto *time = m_table->item(row, 0);
        const auto *camera = m_table->item(row, 1);
        const auto *event = m_table->item(row, 2);
        const auto *status = m_table->item(row, 3);
        if (!time || !camera || !event)
            continue;
        const QDateTime dt = QDateTime::fromString(time->text(), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        const QString iso = dt.isValid() ? dt.toUTC().toString(Qt::ISODate) : QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        const bool acknowledged = status && status->text() == T("Подтверждено");
        m_history.append(iso + QChar(0x1f) + camera->text() + QChar(0x1f) + event->text()
                         + QChar(0x1f) + (acknowledged ? QStringLiteral("1") : QStringLiteral("0")));
    }
    while (m_history.size() > 500) m_history.removeLast();
    m_historyDirty = true;
    m_persistTimer->start();
}

// ======================================================================
// app/fullscreen_view.cpp
// ======================================================================
FullscreenView::FullscreenView(VideoCell *cell, QWidget *parent)
    : QWidget(parent, Qt::Window), m_cell(cell)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(cell ? cell->windowTitle() : QStringLiteral("PSVms"));
    setStyleSheet(QStringLiteral("background: #000000;"));

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->setSizeConstraint(QLayout::SetNoConstraint);
    if (cell) {
        m_surface = cell->takeSurfaceForFullscreen();
        if (m_surface) {
            // Reparenting a live QWidget between top-level windows can leave
            // stale native geometry on Windows. Reparent explicitly, then let
            // the fullscreen layout own its geometry (do not fight it with a
            // manually assigned size before the first layout pass).
            m_surface->hide();
            m_surface->setParent(this);
            m_surface->setMinimumSize(1, 1);
            m_surface->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            m_surface->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            lay->addWidget(m_surface, 1);
            m_surface->show();
            m_surface->updateGeometry();
        }
        connect(cell, &QObject::destroyed, this, [this] { m_cell = nullptr; });
    }

    m_hint = new QLabel(T("Esc или двойной щелчок — выйти"), this);
    m_hint->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_hint->setStyleSheet(QStringLiteral("color:#f2f2f2; background: rgba(0,0,0,160); padding: 8px 14px; border-radius: 6px;"));
    m_hint->adjustSize();
    m_hint->move(24, 20);
    m_hint->raise();
    QTimer::singleShot(3500, m_hint, &QWidget::hide);

    m_close = new QToolButton(this);
    m_close->setText(QStringLiteral("✕"));
    m_close->setToolTip(T("Закрыть"));
    m_close->setStyleSheet(QStringLiteral(
        "QToolButton { color:#ffffff; background: rgba(0,0,0,160); border: none; border-radius: 6px; padding: 8px 14px; font-size:10pt; }"
        "QToolButton:hover { background: #a02b2b; }"));
    m_close->adjustSize();
    m_close->raise();
    connect(m_close, &QToolButton::clicked, this, &QWidget::close);

    // Esc должно срабатывать даже когда фокус находится на видеоповерхности.
    auto *escapeKey = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escapeKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escapeKey, &QShortcut::activated, this, &QWidget::close);

    if (m_surface) {
        connect(m_surface, &VideoSurface::doubleClicked, this, &QWidget::close);
        // Повторный двойной щелчок по изображению всегда возвращает видеопоток в мозаику.
        m_surface->setFocusPolicy(Qt::StrongFocus);
    }
}

void FullscreenView::openOnScreenOf(QWidget *w)
{
    if (w && w->screen())
        setGeometry(w->screen()->geometry());   // тот экран, где открыта программа
    showFullScreen();
    if (m_surface) {
        // Activate the layout only after the native fullscreen window exists.
        // This avoids the common Windows symptom: a tiny/black video surface
        // in the corner while the rest of the fullscreen window stays black.
        if (layout())
            layout()->activate();
        m_surface->show();
        m_surface->raise();
        if (m_hint) m_hint->raise();
        if (m_close) m_close->raise();
        m_surface->updateGeometry();
        m_surface->repaint();
        QPointer<VideoSurface> surface(m_surface);
        QTimer::singleShot(0, this, [this, surface] {
            if (surface) {
                if (layout())
                    layout()->activate();
                surface->setGeometry(rect());
                surface->show();
                surface->raise();
                if (m_hint) m_hint->raise();
                if (m_close) m_close->raise();
                surface->updateGeometry();
                surface->repaint();
                if (surface->currentFrame().isNull()) {
                    surface->setStatusText(T("Ожидание видеопотока…"));
                    surface->update();
                }
            }
        });
    }
    activateWindow();
    raise();
}

void FullscreenView::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape)
        close();
    else
        QWidget::keyPressEvent(e);
}

void FullscreenView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    m_close->move(width() - m_close->width() - 24, 20);
}

void FullscreenView::closeEvent(QCloseEvent *e)
{
    if (m_cell && m_surface) {
        if (layout())
            layout()->removeWidget(m_surface);
        m_cell->restoreSurfaceFromFullscreen(m_surface);
        m_surface = nullptr;
    }
    QWidget::closeEvent(e);
}

// ======================================================================
// app/hik_ip_dialog.cpp
// ======================================================================
HikIpDialog::HikIpDialog(CameraManager *mgr, QWidget *parent) : QDialog(parent), m_mgr(mgr)
{
    setWindowTitle(T("Добавить по IP: Hikvision, HiWatch, Dahua"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(560);

    m_adder = new DeviceAdder(this);

    m_ip = new QLineEdit(this);
    m_ip->setPlaceholderText(QStringLiteral("192.168.1.64"));
    m_httpPort = new QSpinBox(this);
    m_httpPort->setRange(1, 65535);
    m_httpPort->setValue(80);
    m_rtspPort = new QSpinBox(this);
    m_rtspPort->setRange(1, 65535);
    m_rtspPort->setValue(554);
    m_user = new QLineEdit(QStringLiteral("admin"), this);
    m_pass = new QLineEdit(this);
    m_pass->setEchoMode(QLineEdit::Password);
    auto *wholeRecorderCheck = new QCheckBox(T("Добавить как целый регистратор (все каналы в одну группу)"), this);
    wholeRecorderCheck->setChecked(true);
    wholeRecorderCheck->setToolTip(T("Добавляет сразу все каналы регистратора и объединяет их в единую группу устройства"));

    m_channels = new QSpinBox(this);
    m_channels->setRange(0, 128);
    m_channels->setSpecialValueText(T("авто"));
    m_channels->setValue(16);
    m_channels->setToolTip(T("0 — автоопределение каналов, либо укажите число каналов регистратора (4, 8, 16, 32, 64 и т.д.)"));

    auto *presetRow = new QHBoxLayout;
    presetRow->setSpacing(6);
    presetRow->addWidget(m_channels);
    const int kChannelPresets[6] = {0, 4, 8, 16, 32, 64};
    for (int p : kChannelPresets) {
        const QString btnText = (p == 0) ? T("Авто") : QString::number(p);
        auto *pBtn = new QPushButton(btnText, this);
        pBtn->setMinimumWidth(42);
        pBtn->setToolTip(p == 0 ? T("Автоопределить число каналов") : T("Установить %1 каналов регистратора").arg(p));
        connect(pBtn, &QPushButton::clicked, this, [this, p, wholeRecorderCheck] {
            m_channels->setValue(p);
            if (p > 1)
                wholeRecorderCheck->setChecked(true);
        });
        presetRow->addWidget(pBtn);
    }
    presetRow->addStretch(1);

    m_group = new QLineEdit(this);
    m_group->setPlaceholderText(T("для регистратора — по умолчанию имя регистратора и IP"));
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(T("имя регистратора или камеры (необязательно)"));

    auto *portRow = new QHBoxLayout;
    portRow->addWidget(new QLabel(T("HTTP-порт"), this));
    portRow->addWidget(m_httpPort);
    portRow->addSpacing(12);
    portRow->addWidget(new QLabel(T("RTSP-порт"), this));
    portRow->addWidget(m_rtspPort);
    portRow->addStretch(1);

    auto *form = new QFormLayout;
    form->addRow(T("IP-адрес"), m_ip);
    form->addRow(T("Порты"), portRow);
    form->addRow(T("Логин"), m_user);
    form->addRow(T("Пароль"), m_pass);
    form->addRow(QString(), wholeRecorderCheck);
    form->addRow(T("Каналов в регистраторе"), presetRow);
    form->addRow(T("Название"), m_name);
    form->addRow(T("Группа"), m_group);

    auto *hint = new QLabel(T("Работает с новыми и старыми моделями камер и регистраторов Hikvision и HiWatch. "
                              "Регистраторы Dahua и совместимые определяются автоматически по RTSP-адресу, "
                              "без списка моделей и каналов — укажите число каналов регистратора вручную, если оно известно. "
                              "Если устройство потребует другой логин или пароль, программа спросит."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#8a8a8e;"));

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_btn = new QPushButton(T("Проверить и добавить"), this);
    auto *close = new QPushButton(T("Закрыть"), this);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_progress, 1);
    bottom->addWidget(m_btn);
    bottom->addWidget(close);

    auto *root = new QVBoxLayout(this);
    root->addWidget(hint);
    root->addLayout(form);
    root->addWidget(m_status);
    root->addLayout(bottom);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_btn, &QPushButton::clicked, this, [this] {
        const QString ip = m_ip->text().trimmed();
        if (ip.isEmpty()) {
            QMessageBox::warning(this, windowTitle(), T("Укажите IP-адрес устройства."));
            return;
        }
        m_address = ip;
        start(m_user->text().trimmed(), m_pass->text());
    });
    connect(m_adder, &DeviceAdder::resolved, this, [this](int, const AddResult &res) { onResolved(res); });
}

void HikIpDialog::start(const QString &user, const QString &password)
{
    m_curUser = user;
    AddRequest r;
    r.address = m_address;
    r.httpPort = quint16(m_httpPort->value());
    r.rtspPort = quint16(m_rtspPort->value());
    r.user = user;
    r.password = password;
    r.channels = m_channels->value();

    m_btn->setEnabled(false);
    m_progress->setRange(0, 0);
    m_status->setText(T("Проверка устройства %1…").arg(m_address));
    m_adder->start({r}, 1);
}

void HikIpDialog::onResolved(const AddResult &res)
{
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    m_btn->setEnabled(true);

    if (res.authFailed) {
        // устройство требует пароль: спрашиваем и повторяем
        QString user = m_curUser;
        QString pass = m_pass->text();
        if (askCredentials(this, m_address, &user, &pass)) {
            m_user->setText(user);
            m_pass->setText(pass);
            start(user, pass);
        } else {
            m_status->setText(T("Добавление отменено: нужен пароль."));
        }
        return;
    }
    if (!res.ok) {
        m_status->setText(T("Не удалось добавить: %1").arg(res.error));
        return;
    }

    auto *chkWhole = findChild<QCheckBox *>();
    const bool treatAsWholeRecorder = res.recorder
        || (chkWhole && chkWhole->isChecked() && (res.channels.size() > 1 || m_channels->value() > 1));
    const QString customTitle = m_name->text().trimmed();
    const QString base = !customTitle.isEmpty()
        ? customTitle
        : (!res.name.isEmpty() ? res.name : (!res.model.isEmpty() ? res.model : m_address));
    QString group = m_group->text().trimmed();
    if (group.isEmpty() && treatAsWholeRecorder)
        group = QStringLiteral("%1 (%2)").arg(base, m_address);

    QList<CameraEntry> entries;
    for (const AddedChannel &c : res.channels) {
        const QString chName = c.name.isEmpty() ? T("Канал %1").arg(c.number) : c.name;
        CameraEntry e;
        if (treatAsWholeRecorder)
            e.name = QStringLiteral("%1 — %2").arg(base, chName);
        else
            e.name = !customTitle.isEmpty() ? customTitle
                     : (base == m_address ? m_address : QStringLiteral("%1 (%2)").arg(base, m_address));
        e.group = group;
        e.url = c.mainUrl;
        e.liveUrl = c.subUrl;
        e.record = false;
        entries.append(e);
    }
    m_mgr->addMany(entries);
    m_status->setText(treatAsWholeRecorder
        ? T("Добавлен целый регистратор «%1»: каналов %2 (%3). Способ: %4")
            .arg(base).arg(entries.size()).arg(res.model.isEmpty() ? base : res.model).arg(res.method)
        : T("Добавлено каналов: %1 (%2). Способ определения: %3")
            .arg(entries.size()).arg(res.model.isEmpty() ? base : res.model).arg(res.method));
}

// ======================================================================
// app/icsee_import_dialog.cpp
// ======================================================================
namespace {

enum Column_icseedlg { ColCheck_icseedlg, ColAddress_icseedlg, ColState_icseedlg, ColCount_icseedlg };
constexpr int kMaxRange_icseedlg = 2048;

bool parseIpv4_icseedlg(const QString &s, quint32 *out)
{
    QHostAddress a;
    if (!a.setAddress(s.trimmed()) || a.protocol() != QAbstractSocket::IPv4Protocol)
        return false;
    *out = a.toIPv4Address();
    return true;
}

} // namespace

IcseeImportDialog::IcseeImportDialog(CameraManager *mgr, QWidget *parent) : QDialog(parent), m_mgr(mgr)
{
    setWindowTitle(T("Добавить камеры iCSee"));
    resize(820, 600);

    for (const CameraEntry &e : m_mgr->cameras())
        m_existing.insert(QUrl(e.url).host());

    m_scanner = new PortScanner(this);

    // ---- поиск ----
    auto *findBox = new QGroupBox(T("1. Адреса камер"), this);
    auto *findLay = new QHBoxLayout(findBox);
    m_from = new QLineEdit(findBox);
    m_from->setPlaceholderText(QStringLiteral("192.168.1.1"));
    m_to = new QLineEdit(findBox);
    m_to->setPlaceholderText(QStringLiteral("192.168.1.254"));
    m_port = new QSpinBox(findBox);
    m_port->setRange(1, 65535);
    m_port->setValue(554);
    m_port->setToolTip(T("RTSP-порт камеры, по умолчанию 554"));
    m_btnScan = new QPushButton(T("Найти камеры"), findBox);
    findLay->addWidget(new QLabel(T("Диапазон IP:"), findBox));
    findLay->addWidget(m_from);
    findLay->addWidget(new QLabel(QStringLiteral("—"), findBox));
    findLay->addWidget(m_to);
    findLay->addWidget(new QLabel(T("RTSP-порт"), findBox));
    findLay->addWidget(m_port);
    findLay->addWidget(m_btnScan);

    // ---- доступ ----
    auto *authBox = new QGroupBox(T("2. Логин и пароль камер"), this);
    auto *authLay = new QHBoxLayout(authBox);
    m_user = new QLineEdit(QStringLiteral("admin"), authBox);
    m_pass = new QLineEdit(authBox);
    m_pass->setEchoMode(QLineEdit::Password);
    m_pass->setPlaceholderText(T("у новых камер iCSee часто пустой"));
    m_group = new QLineEdit(authBox);
    m_group->setPlaceholderText(T("например, Двор"));
    authLay->addWidget(new QLabel(T("Логин:"), authBox));
    authLay->addWidget(m_user);
    authLay->addWidget(new QLabel(T("Пароль:"), authBox));
    authLay->addWidget(m_pass);
    authLay->addWidget(new QLabel(T("Группа:"), authBox));
    authLay->addWidget(m_group);

    // ---- таблица ----
    m_table = new QTableWidget(0, ColCount_icseedlg, this);
    m_table->setHorizontalHeaderLabels({QString(), T("Адрес"), T("Состояние")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setColumnWidth(ColCheck_icseedlg, 34);
    m_table->setColumnWidth(ColAddress_icseedlg, 200);
    m_table->horizontalHeader()->setSectionResizeMode(ColState_icseedlg, QHeaderView::Stretch);

    auto *selAll = new QPushButton(T("Выбрать все"), this);
    auto *selNone = new QPushButton(T("Снять выбор"), this);
    auto *selRow = new QHBoxLayout;
    selRow->addWidget(new QLabel(T("3. Найденные адреса с открытым RTSP-портом"), this));
    selRow->addStretch(1);
    selRow->addWidget(selAll);
    selRow->addWidget(selNone);

    // ---- низ ----
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_status = new QLabel(
        T("Камеры должны быть в вашей сети (по IP). Облачное подключение по серийному номеру не используется."),
        this);
    m_status->setWordWrap(true);
    m_btnAdd = new QPushButton(T("Добавить выбранные"), this);
    auto *close = new QPushButton(T("Закрыть"), this);

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_progress, 1);
    bottom->addWidget(m_btnAdd);
    bottom->addWidget(close);

    auto *root = new QVBoxLayout(this);
    root->addWidget(findBox);
    root->addWidget(authBox);
    root->addLayout(selRow);
    root->addWidget(m_table, 1);
    root->addWidget(m_status);
    root->addLayout(bottom);

    // ---- сигналы ----
    connect(m_btnScan, &QPushButton::clicked, this, &IcseeImportDialog::startScan);
    connect(m_btnAdd, &QPushButton::clicked, this, &IcseeImportDialog::addSelected);
    connect(selAll, &QPushButton::clicked, this, [this] { setAllChecked(true); });
    connect(selNone, &QPushButton::clicked, this, [this] { setAllChecked(false); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    connect(m_scanner, &PortScanner::found, this, &IcseeImportDialog::addHost);
    connect(m_scanner, &PortScanner::progress, this, [this](int done, int total) {
        m_progress->setRange(0, total);
        m_progress->setValue(done);
        m_status->setText(T("Проверено %1 из %2, найдено: %3").arg(done).arg(total).arg(m_hosts.size()));
    });
    connect(m_scanner, &PortScanner::finished, this, [this] {
        m_status->setText(T("Поиск завершён. Найдено адресов: %1").arg(m_hosts.size()));
        m_btnScan->setEnabled(true);
        m_btnAdd->setEnabled(true);
    });
}

void IcseeImportDialog::startScan()
{
    quint32 a = 0, b = 0;
    if (!parseIpv4_icseedlg(m_from->text(), &a) || !parseIpv4_icseedlg(m_to->text(), &b) || b < a) {
        QMessageBox::warning(this, windowTitle(), T("Укажите корректный диапазон IPv4-адресов."));
        return;
    }
    if (quint64(b) - a + 1 > kMaxRange_icseedlg) {
        QMessageBox::warning(this, windowTitle(),
            T("Слишком большой диапазон: максимум %1 адресов за раз.").arg(kMaxRange_icseedlg));
        return;
    }
    m_btnScan->setEnabled(false);
    m_btnAdd->setEnabled(false);
    m_progress->setRange(0, int(b - a + 1));
    m_progress->setValue(0);
    m_scanner->start(a, b, quint16(m_port->value()), 64, 800);
}

void IcseeImportDialog::addHost(const QString &ip)
{
    if (m_hosts.contains(ip))
        return;
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_hosts.append(ip);

    const bool exists = m_existing.contains(ip);
    auto *chk = new QTableWidgetItem;
    chk->setFlags(exists ? Qt::NoItemFlags : (Qt::ItemIsUserCheckable | Qt::ItemIsEnabled));
    chk->setCheckState(exists ? Qt::Unchecked : Qt::Checked);
    m_table->setItem(row, ColCheck_icseedlg, chk);
    m_table->setItem(row, ColAddress_icseedlg, new QTableWidgetItem(ip));
    m_table->setItem(row, ColState_icseedlg,
                     new QTableWidgetItem(exists ? T("Уже добавлена")
                                                 : T("RTSP-порт открыт")));
}

void IcseeImportDialog::setAllChecked(bool on)
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem *it = m_table->item(row, ColCheck_icseedlg);
        if (it->flags() & Qt::ItemIsUserCheckable)
            it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
}

void IcseeImportDialog::addSelected()
{
    const QString user = m_user->text().trimmed();
    const QString pass = m_pass->text();
    const QString group = m_group->text().trimmed();
    const quint16 port = quint16(m_port->value());

    QList<CameraEntry> entries;
    QList<int> rows;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *chk = m_table->item(row, ColCheck_icseedlg);
        if (!(chk->flags() & Qt::ItemIsUserCheckable) || chk->checkState() != Qt::Checked)
            continue;
        const QString ip = m_hosts.at(row);

        CameraEntry e;
        e.name = QStringLiteral("iCSee %1").arg(ip);
        e.group = group;
        e.url = icseeRtspUrl(ip, port, user, pass, 1, 0);       // основной поток — в архив
        e.liveUrl = icseeRtspUrl(ip, port, user, pass, 1, 1);   // подпоток — в мозаику
        e.record = false;
        entries.append(e);
        rows.append(row);
    }

    if (entries.isEmpty()) {
        QMessageBox::information(this, windowTitle(), T("Отметьте камеры для добавления."));
        return;
    }

    m_mgr->addMany(entries);
    for (int row : rows) {
        m_table->item(row, ColState_icseedlg)->setText(T("Добавлена"));
        m_table->item(row, ColCheck_icseedlg)->setCheckState(Qt::Unchecked);
        m_table->item(row, ColCheck_icseedlg)->setFlags(Qt::NoItemFlags);
        m_existing.insert(m_hosts.at(row));
    }
    m_status->setText(T("Добавлено камер: %1. Состояние записи смотрите на вкладке «Устройства» и в «Журнале».")
                          .arg(entries.size()));
}

// ======================================================================
// app/lan_search_dialog.cpp
// ======================================================================
namespace {

enum LanColumn { LanColCheck, LanColAddress, LanColVendor, LanColModel, LanColState, LanColCount };
constexpr int kLanMaxRange = 2048;

bool lanParseIpv4(const QString &s, quint32 *out)
{
    QHostAddress a;
    if (!a.setAddress(s.trimmed()) || a.protocol() != QAbstractSocket::IPv4Protocol)
        return false;
    *out = a.toIPv4Address();
    return true;
}

} // namespace

LanSearchDialog::LanSearchDialog(CameraManager *mgr, QWidget *parent) : QDialog(parent), m_mgr(mgr)
{
    setWindowTitle(T("Поиск камер в сети: Hikvision, HiWatch, Polyvision, Dahua"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(1020, 700);

    for (const CameraEntry &e : m_mgr->cameras())
        m_existing.insert(QUrl(e.url).host());

    m_sadp = new HikDiscovery(this);
    m_onvif = new OnvifDiscovery(this);
    m_scan = new PortScanner(this);
    m_adder = new DeviceAdder(this);

    // ---- поиск ----
    auto *findBox = new QGroupBox(T("1. Поиск устройств"), this);
    auto *findLay = new QHBoxLayout(findBox);
    m_btnFind = new QPushButton(T("Найти в своей сети"), findBox);
    m_from = new QLineEdit(findBox);
    m_from->setPlaceholderText(QStringLiteral("192.168.1.1"));
    m_to = new QLineEdit(findBox);
    m_to->setPlaceholderText(QStringLiteral("192.168.1.254"));
    // Подставляем IPv4-подсеть активного адаптера, если она достаточно мала для безопасного сканирования.
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            bool ipOk = false, maskOk = false;
            const quint32 ip = entry.ip().toIPv4Address(&ipOk);
            const quint32 mask = entry.netmask().toIPv4Address(&maskOk);
            if (!ipOk || !maskOk || mask == 0xFFFFFFFFu)
                continue;
            const quint32 network = ip & mask;
            const quint32 broadcast = network | ~mask;
            if (broadcast <= network + 1)
                continue;
            const quint32 first = network + 1;
            const quint32 last = broadcast - 1;
            if (quint64(last) - first + 1 > kLanMaxRange)
                continue;
            m_from->setText(QHostAddress(first).toString());
            m_to->setText(QHostAddress(last).toString());
            break;
        }
        if (!m_from->text().isEmpty())
            break;
    }
    m_btnScan = new QPushButton(T("Сканировать диапазон"), findBox);
    m_filter = new QComboBox(findBox);
    m_filter->addItems({T("Все производители"), QStringLiteral("Hikvision"), QStringLiteral("HiWatch"),
                        QStringLiteral("Polyvision"), QStringLiteral("Dahua")});
    findLay->addWidget(m_btnFind);
    findLay->addSpacing(12);
    findLay->addWidget(new QLabel(T("Диапазон IP:"), findBox));
    findLay->addWidget(m_from);
    findLay->addWidget(new QLabel(QStringLiteral("—"), findBox));
    findLay->addWidget(m_to);
    findLay->addWidget(m_btnScan);
    findLay->addSpacing(12);
    findLay->addWidget(m_filter);

    // ---- доступ ----
    auto *authBox = new QGroupBox(T("2. Общие логин и пароль"), this);
    auto *authLay = new QHBoxLayout(authBox);
    m_user = new QLineEdit(QStringLiteral("admin"), authBox);
    m_pass = new QLineEdit(authBox);
    m_pass->setEchoMode(QLineEdit::Password);
    m_group = new QLineEdit(authBox);
    m_group->setPlaceholderText(T("для регистратора — по умолчанию его имя"));
    m_rtsp = new QSpinBox(authBox);
    m_rtsp->setRange(1, 65535);
    m_rtsp->setValue(554);
    m_channels = new QSpinBox(authBox);
    m_channels->setRange(0, 128);
    m_channels->setSpecialValueText(T("авто"));
    m_channels->setToolTip(T("Число каналов регистратора, если автоопределение не сработало"));
    authLay->addWidget(new QLabel(T("Логин:"), authBox));
    authLay->addWidget(m_user);
    authLay->addWidget(new QLabel(T("Пароль:"), authBox));
    authLay->addWidget(m_pass);
    authLay->addWidget(new QLabel(T("Группа:"), authBox));
    authLay->addWidget(m_group, 1);
    authLay->addWidget(new QLabel(T("RTSP-порт"), authBox));
    authLay->addWidget(m_rtsp);
    authLay->addWidget(new QLabel(T("Каналов в регистраторе:"), authBox));
    authLay->addWidget(m_channels);

    // ---- таблица ----
    m_table = new QTableWidget(0, LanColCount, this);
    m_table->setHorizontalHeaderLabels({QString(), T("Адрес"), T("Производитель"), T("Модель / имя"), T("Состояние")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setColumnWidth(LanColCheck, 34);
    m_table->setColumnWidth(LanColAddress, 140);
    m_table->setColumnWidth(LanColVendor, 120);
    m_table->setColumnWidth(LanColModel, 320);
    m_table->horizontalHeader()->setSectionResizeMode(LanColState, QHeaderView::Stretch);

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

    connect(m_btnFind, &QPushButton::clicked, this, &LanSearchDialog::startSearch);
    connect(m_btnScan, &QPushButton::clicked, this, &LanSearchDialog::startScan);
    connect(m_btnImport, &QPushButton::clicked, this, &LanSearchDialog::importSelected);
    connect(m_filter, &QComboBox::currentIndexChanged, this, &LanSearchDialog::applyFilter);
    connect(selAll, &QPushButton::clicked, this, [this] { setAllChecked(true); });
    connect(selNone, &QPushButton::clicked, this, [this] { setAllChecked(false); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    // ---- поиск: SADP и ONVIF работают одновременно ----
    connect(m_sadp, &HikDiscovery::deviceFound, this, [this](const HikDevice &d) {
        addCandidate(d.address, d.httpPort, d.model, QString());
    });
    connect(m_onvif, &OnvifDiscovery::deviceFound, this, [this](const OnvifDevice &d) {
        const QUrl endpoint(d.xaddr);
        const int endpointPort = endpoint.port(80);
        addCandidate(d.address, quint16(qBound(1, endpointPort, 65535)), d.hardware, d.name);
    });
    auto searchDone = [this] {
        if (--m_pendingSearches > 0)
            return;
        m_progress->setRange(0, 1);
        m_progress->setValue(1);
        m_status->setText(T("Поиск SADP/ONVIF завершён. Найдено: %1. Дополнительно проверяем порты устройств в локальной подсети…")
                              .arg(m_candidates.size()));
        quint32 first = 0, last = 0;
        if (lanParseIpv4(m_from->text(), &first) && lanParseIpv4(m_to->text(), &last) && last >= first
            && quint64(last) - first + 1 <= kLanMaxRange) {
            QTimer::singleShot(0, this, &LanSearchDialog::startScan);
        } else {
            setBusy(false);
            m_status->setText(T("Поиск завершён. Найдено устройств: %1. Укажите диапазон IPv4 для дополнительного сканирования.")
                                  .arg(m_candidates.size()));
        }
    };
    connect(m_sadp, &HikDiscovery::finished, this, searchDone);
    connect(m_onvif, &OnvifDiscovery::finished, this, searchDone);

    connect(m_scan, &PortScanner::foundPort, this, [this](const QString &ip, quint16 port) {
        // RTSP и SDK-порты подтверждают наличие устройства, но не являются HTTP-портом ISAPI.
        // Для HTTP-опроса используем 80, либо 8080, если стандартный HTTP-порт не найден.
        const quint16 httpPort = (port == 80 || port == 8080) ? port : 0;
        addCandidate(ip, httpPort, QString(), QString());
    });
    connect(m_scan, &PortScanner::progress, this, [this](int done, int total) {
        const int overall = m_scanDoneBase + done;
        m_progress->setRange(0, qMax(1, m_scanTotal));
        m_progress->setValue(overall);
        m_status->setText(T("Проверка TCP-порта %1: %2 из %3, найдено устройств: %4")
                              .arg(m_scanPorts.value(m_scanPortIndex))
                              .arg(overall).arg(m_scanTotal).arg(m_candidates.size()));
    });
    connect(m_scan, &PortScanner::finished, this, [this] {
        m_scanDoneBase += int(quint64(m_scanLastIp) - m_scanFirstIp + 1);
        ++m_scanPortIndex;
        scanNextPort();
    });

    // ---- добавление ----
    connect(m_adder, &DeviceAdder::progress, this, [this](int done, int total) {
        m_progress->setRange(0, total);
        m_progress->setValue(done);
        m_status->setText(T("Опрос устройств: %1 из %2").arg(done).arg(total));
    });
    connect(m_adder, &DeviceAdder::resolved, this, [this](int i, const AddResult &res) {
        const int row = m_roundRows.at(i);
        if (res.ok) {
            addEntries(row, res);
        } else if (res.authFailed) {
            m_authRetry.append(row);
            setState(row, T("Нужен пароль"));
        } else {
            ++m_failed;
            setState(row, res.error);
        }
    });
    connect(m_adder, &DeviceAdder::finished, this, &LanSearchDialog::onRoundFinished);
}

void LanSearchDialog::addCandidate(const QString &ip, quint16 httpPort, const QString &model, const QString &name)
{
    const QString title = name.isEmpty() || name == model ? model : (model.isEmpty() ? name : model + QStringLiteral(" · ") + name);
    const QString vendor = guessVendor(model, name);

    for (int i = 0; i < m_candidates.size(); ++i) {
        if (m_candidates.at(i).address != ip)
            continue;
        if ((httpPort == 80 || httpPort == 8080)
            && (!m_candidates[i].httpPortConfirmed || httpPort == 80)) {
            m_candidates[i].httpPort = httpPort;
            m_candidates[i].httpPortConfirmed = true;
        }
        if (m_candidates.at(i).model.isEmpty() && !title.isEmpty()) {   // уточняем данные, найденные другим способом
            m_candidates[i].model = title;
            m_candidates[i].vendor = vendor;
            m_table->item(i, LanColModel)->setText(title);
            m_table->item(i, LanColVendor)->setText(vendor.isEmpty() ? QStringLiteral("—") : vendor);
            applyFilter();
        }
        return;
    }

    const int row = m_table->rowCount();
    m_table->insertRow(row);
    Candidate c;
    c.address = ip;
    c.httpPort = httpPort ? httpPort : 80;
    c.httpPortConfirmed = (httpPort == 80 || httpPort == 8080);
    c.model = title;
    c.vendor = vendor;
    m_candidates.append(c);

    const bool exists = m_existing.contains(ip);
    auto *chk = new QTableWidgetItem;
    chk->setFlags(exists ? Qt::NoItemFlags : (Qt::ItemIsUserCheckable | Qt::ItemIsEnabled));
    chk->setCheckState(exists ? Qt::Unchecked : Qt::Checked);
    m_table->setItem(row, LanColCheck, chk);
    m_table->setItem(row, LanColAddress, new QTableWidgetItem(ip));
    m_table->setItem(row, LanColVendor, new QTableWidgetItem(vendor.isEmpty() ? QStringLiteral("—") : vendor));
    m_table->setItem(row, LanColModel, new QTableWidgetItem(title));
    m_table->setItem(row, LanColState, new QTableWidgetItem(exists ? T("Уже добавлено") : T("Найдено")));
    applyFilter();
}

void LanSearchDialog::applyFilter()
{
    const int f = m_filter->currentIndex();   // 0 — все, 1 — Hikvision, 2 — HiWatch, 3 — Polyvision, 4 — Dahua
    const QString want = f == 1 ? QStringLiteral("Hikvision") : f == 2 ? QStringLiteral("HiWatch")
                       : f == 3 ? QStringLiteral("Polyvision") : f == 4 ? QStringLiteral("Dahua") : QString();
    for (int row = 0; row < m_table->rowCount(); ++row)
        m_table->setRowHidden(row, !want.isEmpty() && m_candidates.at(row).vendor != want);
}

void LanSearchDialog::setAllChecked(bool on)
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem *it = m_table->item(row, LanColCheck);
        if ((it->flags() & Qt::ItemIsUserCheckable) && !m_table->isRowHidden(row))
            it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
}

void LanSearchDialog::setState(int row, const QString &text)
{
    if (row >= 0 && row < m_table->rowCount())
        m_table->item(row, LanColState)->setText(text);
}

void LanSearchDialog::setBusy(bool busy)
{
    m_btnFind->setEnabled(!busy);
    m_btnScan->setEnabled(!busy);
    m_btnImport->setEnabled(!busy);
}

void LanSearchDialog::startSearch()
{
    setBusy(true);
    m_pendingSearches = 2;
    m_progress->setRange(0, 0);
    m_status->setText(T("Поиск устройств в сети…"));
    m_sadp->start(4000);
    m_onvif->start(4000);
}

void LanSearchDialog::startScan()
{
    quint32 a = 0, b = 0;
    if (!lanParseIpv4(m_from->text(), &a) || !lanParseIpv4(m_to->text(), &b) || b < a) {
        QMessageBox::warning(this, windowTitle(), T("Укажите корректный диапазон IPv4-адресов."));
        return;
    }
    if (quint64(b) - a + 1 > kLanMaxRange) {
        QMessageBox::warning(this, windowTitle(), T("Слишком большой диапазон: максимум %1 адресов за раз.").arg(kLanMaxRange));
        return;
    }
    setBusy(true);
    m_scanFirstIp = a;
    m_scanLastIp = b;
    m_scanDoneBase = 0;
    m_scanPortIndex = 0;
    m_scanPorts = {80, 81, 443, 554, 8000, 8080, 8443, 8899, 34567, 37777};
    const quint16 customRtsp = quint16(m_rtsp->value());
    if (!m_scanPorts.contains(customRtsp))
        m_scanPorts.append(customRtsp);
    m_scanTotal = int(quint64(b) - a + 1) * m_scanPorts.size();
    m_progress->setRange(0, qMax(1, m_scanTotal));
    m_progress->setValue(0);
    scanNextPort();
}

void LanSearchDialog::scanNextPort()
{
    if (m_scanPortIndex >= m_scanPorts.size()) {
        m_progress->setValue(m_scanTotal);
        m_status->setText(T("Сканирование завершено. Найдено устройств: %1. Проверено портов: %2")
                              .arg(m_candidates.size()).arg(m_scanPorts.size()));
        setBusy(false);
        return;
    }
    const quint16 port = m_scanPorts.at(m_scanPortIndex);
    m_status->setText(T("Сканирование сети: TCP-порт %1 (%2 из %3)…")
                          .arg(port).arg(m_scanPortIndex + 1).arg(m_scanPorts.size()));
    m_scan->start(m_scanFirstIp, m_scanLastIp, port, 128, 600);
}

void LanSearchDialog::importSelected()
{
    if (m_user->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), T("Укажите логин."));
        return;
    }
    m_commonUser = m_user->text().trimmed();
    m_commonPass = m_pass->text();
    m_groupName = m_group->text().trimmed();
    m_rtspPort = quint16(m_rtsp->value());
    m_fallbackChannels = m_channels->value();

    m_roundRows.clear();
    m_rowUser.clear();
    m_rowPass.clear();
    m_authRetry.clear();
    m_entries.clear();
    m_addedRows.clear();
    m_failed = 0;
    m_devicesOk = 0;

    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *chk = m_table->item(row, LanColCheck);
        if (!(chk->flags() & Qt::ItemIsUserCheckable) || chk->checkState() != Qt::Checked || m_table->isRowHidden(row))
            continue;
        m_roundRows.append(row);
        setState(row, T("Запрос…"));
    }
    if (m_roundRows.isEmpty()) {
        QMessageBox::information(this, windowTitle(), T("Отметьте устройства для добавления."));
        return;
    }
    setBusy(true);
    runRound();
}

void LanSearchDialog::runRound()
{
    QList<AddRequest> reqs;
    for (int row : std::as_const(m_roundRows)) {
        AddRequest r;
        r.address = m_candidates.at(row).address;
        r.httpPort = m_candidates.at(row).httpPort;
        r.rtspPort = m_rtspPort;
        r.user = m_rowUser.value(row, m_commonUser);
        r.password = m_rowPass.value(row, m_commonPass);
        r.channels = m_fallbackChannels;
        reqs.append(r);
    }
    m_progress->setRange(0, int(reqs.size()));
    m_progress->setValue(0);
    m_adder->start(reqs, 6);
}

void LanSearchDialog::addEntries(int row, const AddResult &res)
{
    const QString address = m_candidates.at(row).address;
    const QString base = !res.name.isEmpty() ? res.name : (!res.model.isEmpty() ? res.model : address);
    QString group = m_groupName;
    if (group.isEmpty() && res.recorder)
        group = QStringLiteral("%1 (%2)").arg(base, address);

    for (const AddedChannel &c : res.channels) {
        const QString chName = c.name.isEmpty() ? T("Канал %1").arg(c.number) : c.name;
        CameraEntry e;
        e.name = res.recorder ? QStringLiteral("%1 — %2").arg(address, chName)
                              : (base == address ? address : QStringLiteral("%1 (%2)").arg(base, address));
        e.group = group;
        e.url = c.mainUrl;
        e.liveUrl = c.subUrl;
        e.record = false;
        m_entries.append(e);
    }
    m_addedRows.append(row);
    ++m_devicesOk;

    QString kind = res.recorder ? T("регистратор") : T("камера");
    QString model = res.model.isEmpty() ? base : res.model;
    m_table->item(row, LanColModel)->setText(T("%1 — %2, каналов: %3").arg(model, kind).arg(res.channels.size()));
    setState(row, T("Готово к добавлению") + QStringLiteral(" (") + res.method + QStringLiteral(")"));
}

void LanSearchDialog::onRoundFinished()
{
    // Устройствам, которые отказали во входе, запрашиваем логин и пароль и повторяем опрос
    if (!m_authRetry.isEmpty()) {
        QList<int> next;
        const QList<int> pending = m_authRetry;
        m_authRetry.clear();
        for (int row : pending) {
            QString user = m_rowUser.value(row, m_commonUser);
            QString pass = m_rowPass.value(row, m_commonPass);
            if (askCredentials(this, m_candidates.at(row).address, &user, &pass)) {
                m_rowUser[row] = user;
                m_rowPass[row] = pass;
                next.append(row);
                setState(row, T("Запрос…"));
            } else {
                ++m_failed;
                setState(row, T("Пропущено: нужен пароль"));
            }
        }
        if (!next.isEmpty()) {
            m_roundRows = next;
            runRound();
            return;
        }
    }
    finishImport();
}

void LanSearchDialog::finishImport()
{
    if (!m_entries.isEmpty())
        m_mgr->addMany(m_entries);
    for (int row : std::as_const(m_addedRows)) {
        setState(row, T("Добавлено"));
        m_table->item(row, LanColCheck)->setCheckState(Qt::Unchecked);
        m_table->item(row, LanColCheck)->setFlags(Qt::NoItemFlags);
        m_existing.insert(m_candidates.at(row).address);
    }
    m_status->setText(T("Добавлено каналов: %1 (устройств: %2), с ошибкой: %3%4")
                          .arg(m_entries.size()).arg(m_devicesOk).arg(m_failed)
                          .arg(m_failed ? T(" — исправьте логин/пароль и повторите") : QString()));
    m_entries.clear();
    m_addedRows.clear();
    setBusy(false);
}

// ======================================================================
// app/live_view_page.cpp
// ======================================================================
namespace {

QIcon liveDotIcon(const QColor &c)
{
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(1, 1, 10, 10);
    return QIcon(pm);
}

// Только два состояния в списке групп: зелёный — камера доступна/записывает,
// оранжевый — подключается, остановлена или недоступна. Синие точки исключены.
QColor liveStateColor(const CameraManager *mgr, const QString &id)
{
    switch (mgr->stateOf(id)) {
    case CameraWorker::State::Recording:
        return QColor("#63e6ee");
    case CameraWorker::State::Connecting:
    case CameraWorker::State::Reconnecting:
        return QColor("#63e6ee");
    case CameraWorker::State::Stopped:
        return mgr->isOnline(id) ? QColor("#63e6ee") : QColor("#63e6ee");
    }
    return QColor("#63e6ee");
}

} // namespace

static QIcon makeArchiveToolbarIcon()
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor("#63e6ee"), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(4, 6, 24, 22), 4, 4);
    p.drawLine(4, 12, 28, 12);
    p.drawLine(11, 17, 21, 17);
    p.drawLine(11, 21, 21, 21);
    p.drawLine(16, 2, 16, 8);
    p.drawPolyline(QPolygonF{QPointF(12, 5), QPointF(16, 9), QPointF(20, 5)});
    return QIcon(pm);
}

LiveViewPage::LiveViewPage(CameraManager *mgr, QWidget *parent) : QWidget(parent), m_mgr(mgr)
{
    // Верхняя строка счётчиков удалена по запросу: видеостена начинается сразу,
    // без подписей «Камер всего», «В сети», «Нет связи» и «Запись».
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 8);
    root->setSpacing(9);

    auto *split = new QSplitter(Qt::Horizontal, this);
    root->addWidget(split, 1);

    // ---- левая панель: поиск, дерево камер, PTZ ----
    auto *left = new QWidget;
    m_leftPanel = left;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(0);

    m_search = new QLineEdit(left);
    m_search->setPlaceholderText(T("Поиск по названию, IP или URL…"));
    m_search->setClearButtonEnabled(true);
    m_search->setClearButtonEnabled(true);

    m_tree = new CameraTree(left);
    m_tree->setHeaderHidden(true);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setIndentation(14);
    m_tree->setTextElideMode(Qt::ElideRight);
    m_tree->setWordWrap(false);
    m_tree->setUniformRowHeights(true);

    auto wrap = [left](QWidget *w, int l, int t, int r, int b) {
        auto *c = new QWidget(left);
        auto *cl = new QVBoxLayout(c);
        cl->setContentsMargins(l, t, r, b);
        cl->addWidget(w);
        return c;
    };
    auto *title = new QLabel(T("Камеры"), left);
    title->setObjectName(QStringLiteral("panelTitle"));
    lv->addWidget(title);
    lv->addWidget(wrap(m_search, 8, 8, 8, 6));
    auto *camSummary = new QLabel(left);
    camSummary->setObjectName(QStringLiteral("camSummaryBadge"));
    camSummary->setStyleSheet(QStringLiteral(
        "QLabel#camSummaryBadge {"
        "  color: #C5CDD8;"
        "  background: #141820;"
        "  border: 1px solid #262E3D;"
        "  border-radius: 4px;"
        "  padding: 5px 8px;"
        "  margin: 0 8px 6px 8px;"
        "  font-size: 11px;"
        "}"
    ));
    auto updateCamSummary = [this, camSummary] {
        const auto all = m_mgr->cameras();
        int online = 0, recording = 0;
        for (const CameraEntry &c : all) {
            if (m_mgr->isOnline(c.id)) ++online;
            if (m_mgr->isStarted(c.id)) ++recording;
        }
        camSummary->setText(T("Всего: %1  •  В сети: %2  •  Запись: %3")
            .arg(all.size()).arg(online).arg(recording));
    };
    updateCamSummary();
    connect(m_mgr, &CameraManager::camerasChanged, this, updateCamSummary);
    connect(m_mgr, &CameraManager::cameraStateChanged, this, [updateCamSummary](const QString &, CameraWorker::State) { updateCamSummary(); });
    connect(m_mgr, &CameraManager::onlineChanged, this, updateCamSummary);
    lv->addWidget(camSummary);
    auto *statusFilter = new QComboBox(left);
    statusFilter->addItem(T("Все камеры"), 0);
    statusFilter->addItem(T("Только в сети"), 1);
    statusFilter->addItem(T("Только не в сети"), 2);
    statusFilter->addItem(T("На записи"), 3);
    statusFilter->setStyleSheet(QStringLiteral(
        "QComboBox { background:#141820; color:#D5DCE6; border:1px solid #262E3D; border-radius:4px; padding:4px 8px; margin:0 8px 6px 8px; font-size:11px; }"
        "QComboBox QAbstractItemView { background:#141820; color:#D5DCE6; selection-background-color:#262E3D; }"
    ));
    lv->addWidget(statusFilter);

    auto *exportDiagBtn = new QPushButton(T("Отчёт диагностики…"), left);
    exportDiagBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background:#181D27; color:#F5B700; border:1px solid #2E384B; border-radius:4px; padding:5px 8px; margin:0 8px 6px 8px; font-size:11px; font-weight:600; }"
        "QPushButton:hover { background:#202735; border-color:#F5B700; }"
    ));
    lv->addWidget(exportDiagBtn);
    connect(exportDiagBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, T("Сохранить отчёт диагностики камер"),
            QStringLiteral("psvms_cameras_diagnostics.csv"),
            T("CSV-таблица (*.csv);;Текстовый файл (*.txt)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            emit message(T("Не удалось сохранить отчёт: %1").arg(f.errorString()));
            return;
        }
        QTextStream out(&f);
        out.setEncoding(QStringConverter::Utf8);
        out << QStringLiteral("Name;Group;Online;Recording;Quality;MainStreamUrl;SubStreamUrl\n");
        for (const CameraEntry &c : m_mgr->cameras()) {
            QString safeName = c.name;
            safeName.replace(QLatin1Char(';'), QLatin1Char(','));
            QString safeGroup = c.group;
            safeGroup.replace(QLatin1Char(';'), QLatin1Char(','));
            QString safeMain = maskCredentialsInUrl(c.url);
            safeMain.replace(QLatin1Char(';'), QLatin1Char(','));
            QString safeSub = maskCredentialsInUrl(c.liveUrl.trimmed().isEmpty() ? c.url : c.liveUrl);
            safeSub.replace(QLatin1Char(';'), QLatin1Char(','));
            out << safeName << QLatin1Char(';')
                << safeGroup << QLatin1Char(';')
                << (m_mgr->isOnline(c.id) ? QStringLiteral("online") : QStringLiteral("offline")) << QLatin1Char(';')
                << (m_mgr->isStarted(c.id) ? QStringLiteral("recording") : QStringLiteral("stopped")) << QLatin1Char(';')
                << c.viewQuality << QLatin1Char(';')
                << safeMain << QLatin1Char(';')
                << safeSub << QLatin1Char('\n');
        }
        emit message(T("Отчёт диагностики сохранён: %1").arg(path));
    });
    lv->addWidget(m_tree, 1);
    const int leftPct = qBound(70, QSettings().value(QStringLiteral("ui/leftPanelWidthPercent"), 100).toInt(), 140);
    const int minLeftW = qMax(140, qRound(190 * leftPct / 100.0));
    const int maxLeftW = qMax(minLeftW + 30, qRound(260 * leftPct / 100.0));
    left->setMinimumWidth(minLeftW);
    left->setMaximumWidth(maxLeftW);

    // ---- центр: мозаика и нижняя панель ----
    auto *center = new QWidget;
    auto *cv = new QVBoxLayout(center);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);

    m_grid = new VideoGrid(mgr, center);
    const int gapPct = qBound(25, QSettings().value(QStringLiteral("ui/gridGapPercent"), 100).toInt(), 200);
    const int gridMargin = qMax(1, qRound(4 * gapPct / 100.0));
    m_grid->setContentsMargins(gridMargin, gridMargin, gridMargin, gridMargin);

    auto *toolbar = new QWidget(center);
    toolbar->setObjectName(QStringLiteral("videoToolbar"));
    auto *tools = new QHBoxLayout(toolbar);
    tools->setContentsMargins(8, 6, 8, 6);
    tools->setSpacing(6);

    auto *panelBtn = new QToolButton(center);
    panelBtn->setText(T("☰ Камеры"));
    panelBtn->setCheckable(true);
    panelBtn->setChecked(true);
    panelBtn->setToolTip(T("Показать или скрыть список камер"));
    tools->addWidget(panelBtn);
    tools->addSpacing(8);

    // раскладка экрана: от 1 до 64 ячеек
    m_layoutBtn = new QToolButton(center);
    m_layoutBtn->setPopupMode(QToolButton::InstantPopup);
    m_layoutBtn->setToolTip(T("Раскладка экрана"));
    auto *layoutMenu = new QMenu(m_layoutBtn);
    auto *layoutGroup = new QActionGroup(layoutMenu);
    for (const GridLayoutDef &d : VideoGrid::layouts()) {
        QAction *a = layoutMenu->addAction(VideoGrid::layoutLabel(d.count));
        a->setCheckable(true);
        a->setData(d.count);
        layoutGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, d] { m_grid->setLayoutCount(d.count); });
    }
    m_layoutBtn->setMenu(layoutMenu);
    auto updateLayoutBtn = [this, layoutGroup](int count) {
        m_layoutBtn->setText(QStringLiteral("▦ ") + VideoGrid::layoutLabel(count));
        for (QAction *a : layoutGroup->actions())
            a->setChecked(a->data().toInt() == count);
    };
    connect(m_grid, &VideoGrid::layoutCountChanged, this, updateLayoutBtn);
    updateLayoutBtn(m_grid->layoutCount());
    tools->addWidget(m_layoutBtn);

    auto *streamBtn = new QToolButton(center);
    streamBtn->setPopupMode(QToolButton::InstantPopup);
    streamBtn->setText(T("⇄ Поток сетки"));
    streamBtn->setToolTip(T("Качество потока сетки (Ctrl+S — подпоток для всех активных камер)"));
    auto *streamMenu = new QMenu(streamBtn);
    connect(streamMenu->addAction(T("Авто (подпоток в сетке, основной на весь экран)")),
            &QAction::triggered, this, [this] { m_grid->setGridStreamQuality(0); });
    connect(streamMenu->addAction(T("Основной поток для всех ячеек")),
            &QAction::triggered, this, [this] { m_grid->setGridStreamQuality(1); });
    connect(streamMenu->addAction(T("Подпоток для всех ячеек (снижает нагрузку на 1920×1080)")),
            &QAction::triggered, this, [this] { m_grid->setGridStreamQuality(2); });
    streamBtn->setMenu(streamMenu);
    tools->addWidget(streamBtn);

    // Именованные шаблоны убраны: текущая раскладка камер сохраняется автоматически
    // после каждого переноса и восстанавливается при следующем запуске.

    auto *refreshBtn = new QToolButton(center);
    refreshBtn->setText(T("⟳ Обновить"));
    refreshBtn->setToolTip(T("Обновить окна с камерами"));
    tools->addWidget(refreshBtn);

    auto *archiveBtn = new QToolButton(center);
    archiveBtn->setText(T("Архив"));
    archiveBtn->setIcon(makeArchiveToolbarIcon());
    archiveBtn->setIconSize(QSize(17, 17));
    archiveBtn->setToolTip(T("Открыть архив записей выбранной камеры"));
    archiveBtn->setCursor(Qt::PointingHandCursor);
    archiveBtn->setMinimumHeight(38);
    archiveBtn->setMinimumWidth(92);
    archiveBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    tools->addWidget(archiveBtn);
    tools->addSpacing(12);

    auto *startBtn = new QToolButton(center);
    startBtn->setObjectName(QStringLiteral("toolbarStartBtn"));
    startBtn->setText(T("Запустить"));
    startBtn->setIcon(makeCellIcon(QStringLiteral("play"), QColor("#F5B700")));
    startBtn->setIconSize(QSize(15, 15));
    startBtn->setToolTip(T("Запустить показ камер в ячейках и запись"));
    auto *stopBtn = new QToolButton(center);
    stopBtn->setObjectName(QStringLiteral("toolbarStopBtn"));
    stopBtn->setText(T("Остановить показ"));
    stopBtn->setIcon(makeCellIcon(QStringLiteral("stop"), QColor("#F5B700")));
    stopBtn->setIconSize(QSize(15, 15));
    stopBtn->setToolTip(T("Остановить показ в ячейках (запись продолжается)"));
    tools->addWidget(startBtn);
    tools->addWidget(stopBtn);

    tools->addStretch(1);
    auto *closeAll = new QPushButton(T("Закрыть все"), center);
    closeAll->setObjectName(QStringLiteral("closeAllCamerasBtn"));
    // Фиксируем компактную ширину и уменьшаем внутренние отступы:
    // надпись «Закрыть всё» целиком помещается даже в узкой панели.
    closeAll->setFixedSize(136, 38);
    closeAll->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    closeAll->setStyleSheet(QStringLiteral(
        "QPushButton#closeAllCamerasBtn{padding:2px 5px;font-size:10pt;font-weight:700;}"));
    closeAll->setText(T("Закрыть всё"));
    closeAll->setToolTip(T("Остановить и закрыть все открытые камеры"));
    closeAll->setCursor(Qt::PointingHandCursor);
    tools->addWidget(closeAll);

    cv->addWidget(m_grid, 1);
    cv->addWidget(toolbar);

    // Правая панель управления PTZ — как на фирменном макете PSVms
    auto *rightPanel = new QWidget;
    rightPanel->setObjectName(QStringLiteral("controlRail"));
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    auto *ptzTitle = new QPushButton(T("Управление PTZ  ▾"), rightPanel);
    ptzTitle->setObjectName(QStringLiteral("panelTitle"));
    ptzTitle->setCheckable(true);
    ptzTitle->setChecked(true);
    ptzTitle->setCursor(Qt::PointingHandCursor);
    ptzTitle->setFixedHeight(28);
    ptzTitle->setMaximumWidth(178);
    ptzTitle->setStyleSheet(QStringLiteral("QPushButton#panelTitle{font-size:10pt;padding:2px 5px;min-height:0px;}"));
    rightLayout->addWidget(ptzTitle, 0, Qt::AlignLeft);
    auto *ptz = new PtzPanel(mgr, rightPanel);
    ptz->setEnabled(isProVersion());
    ptz->setToolTip(isProVersion() ? QString() : T("Управление PTZ доступно в Pro-версии"));
    rightLayout->addWidget(ptz);
    connect(ptzTitle, &QPushButton::toggled, ptz, [ptz, ptzTitle](bool visible) {
        ptz->setVisible(visible);
        ptzTitle->setText(visible ? T("Управление PTZ  ▾") : T("Управление PTZ  ▸"));
    });
    rightLayout->addStretch(1);
    rightPanel->setMinimumWidth(188);
    rightPanel->setMaximumWidth(220);

    split->addWidget(left);
    split->addWidget(center);
    split->addWidget(rightPanel);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 0);
    split->setCollapsible(0, false);
    split->setCollapsible(1, false);
    split->setCollapsible(2, false);
    split->setSizes({220, 900, 230});

    // Единые размеры и защита подписей от выхода за границы панели.
    // Полные подписи остаются в подсказках, поэтому они доступны и при узком окне.
    const auto prepareCameraToolbarControl = [](QWidget *control) {
        if (!control) return;
        control->setMinimumHeight(38);
        control->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        if (auto *button = qobject_cast<QAbstractButton *>(control)) {
            const QString fullText = button->text();
            if (!fullText.isEmpty() && button->toolTip().isEmpty())
                button->setToolTip(fullText);
            QFont f = button->font();
            if (f.pointSizeF() > 0.0)
                f.setPointSizeF(qMin(10.0, f.pointSizeF()));
            button->setFont(f);
        }
    };
    for (QWidget *control : toolbar->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        prepareCameraToolbarControl(control);
    prepareCameraToolbarControl(closeAll);

    // ---- сигналы ----
    connect(closeAll, &QPushButton::clicked, m_grid, &VideoGrid::clearAll);
    connect(startBtn, &QToolButton::clicked, this, [this] {
        m_grid->startAllCells();
        m_mgr->startAll();
    });
    connect(stopBtn, &QToolButton::clicked, m_grid, &VideoGrid::stopAllCells);
    connect(archiveBtn, &QToolButton::clicked, this, [this] { openArchive(m_grid->selectedCameraId()); });
    connect(refreshBtn, &QToolButton::clicked, this, [this] {
        m_grid->refreshCells();
        m_mgr->probeNow();
        emit message(T("Обновление потоков и проверка доступности камер…"));
    });
    connect(panelBtn, &QToolButton::toggled, this, &LiveViewPage::setPanelVisible);
    connect(m_search, &QLineEdit::textChanged, this, &LiveViewPage::applyFilter);
    connect(statusFilter, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, statusFilter](int) {
        const int mode = statusFilter->currentData().toInt();
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *grp = m_tree->topLevelItem(i);
            int visibleChildren = 0;
            for (int j = 0; j < grp->childCount(); ++j) {
                QTreeWidgetItem *ch = grp->child(j);
                const QString cid = ch->data(0, Qt::UserRole).toString();
                bool ok = true;
                if (mode == 1) ok = m_mgr->isOnline(cid);
                else if (mode == 2) ok = !m_mgr->isOnline(cid);
                else if (mode == 3) ok = m_mgr->isStarted(cid);
                ch->setHidden(!ok);
                if (ok) ++visibleChildren;
            }
            grp->setHidden(visibleChildren == 0);
        }
    });
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = m_tree->itemAt(pos);
        if (!item) return;
        const QString cameraId = item->data(0, Qt::UserRole).toString();
        const bool isGroup = cameraId.isEmpty();
        const QString groupName = item->text(0);
        QList<CameraEntry> targets;
        if (isGroup) {
            for (const CameraEntry &e : m_mgr->cameras()) {
                const QString shownGroup = e.group.isEmpty() ? T("Без группы") : e.group;
                if (shownGroup == groupName) targets.append(e);
            }
        } else if (const CameraEntry *e = m_mgr->find(cameraId)) {
            targets.append(*e);
        }
        if (targets.isEmpty()) return;

        QMenu menu(this);
        if (isGroup) {
            QAction *renameGroup = menu.addAction(T("Переименовать группу…"));
            connect(renameGroup, &QAction::triggered, this, [this, targets, groupName] {
                bool ok = false;
                const QString newName = QInputDialog::getText(
                    this, T("Переименовать группу"),
                    T("Новое название группы «%1»:").arg(groupName),
                    QLineEdit::Normal, groupName, &ok).trimmed();
                if (!ok || newName.isEmpty() || newName == groupName)
                    return;
                for (const CameraEntry &entry : targets) {
                    if (const CameraEntry *current = m_mgr->find(entry.id)) {
                        CameraEntry updated = *current;
                        updated.group = newName;
                        m_mgr->update(updated);
                    }
                }
            });
        } else {
            QAction *diagCamera = menu.addAction(T("Проверить поток и адрес"));
            connect(diagCamera, &QAction::triggered, this, [this, cameraId] {
                if (const CameraEntry *cur = m_mgr->find(cameraId)) {
                    m_mgr->probeNow();
                    const bool on = m_mgr->isOnline(cur->id);
                    const bool rec = m_mgr->isStarted(cur->id);
                    const QString addr = cur->liveUrl.trimmed().isEmpty() ? cur->url : cur->liveUrl;
                    QMessageBox::information(this, T("Диагностика камеры"),
                        T("Камера: %1\nСостояние: %2\nЗапись: %3\nПоток: %4")
                            .arg(cur->name)
                            .arg(on ? T("В сети") : T("Нет связи / проверка"))
                            .arg(rec ? T("Активна") : T("Остановлена"))
                            .arg(addr));
                }
            });
            menu.addSeparator();
            QAction *renameCamera = menu.addAction(T("Переименовать камеру…"));
            connect(renameCamera, &QAction::triggered, this, [this, cameraId] {
                const CameraEntry *current = m_mgr->find(cameraId);
                if (!current)
                    return;
                bool ok = false;
                const QString newName = QInputDialog::getText(
                    this, T("Переименовать камеру"), T("Новое название камеры:"),
                    QLineEdit::Normal, current->name, &ok).trimmed();
                if (!ok || newName.isEmpty() || newName == current->name)
                    return;
                CameraEntry updated = *current;
                updated.name = newName;
                m_mgr->update(updated);
            });

            menu.addSeparator();
            QAction *openArchiveAct = menu.addAction(T("Открыть архив камеры"));
            connect(openArchiveAct, &QAction::triggered, this, [this, cameraId] {
                openArchive(cameraId);
            });
            QAction *toggleQualityAct = menu.addAction(T("Переключить поток (основной / подпоток)"));
            connect(toggleQualityAct, &QAction::triggered, this, [this, cameraId] {
                if (const CameraEntry *cur = m_mgr->find(cameraId)) {
                    CameraEntry upd = *cur;
                    upd.viewQuality = (upd.viewQuality == 2) ? 1 : 2;
                    m_mgr->update(upd);
                    m_grid->refreshCells();
                    emit message(upd.viewQuality == 2
                        ? T("Включён подпоток для «%1»").arg(upd.name)
                        : T("Включён основной поток для «%1»").arg(upd.name));
                }
            });
            QAction *editCamAct = menu.addAction(T("Настройки камеры…"));
            connect(editCamAct, &QAction::triggered, this, [this] {
                if (!Users::current().isAdmin()) {
                    emit message(T("Настройки камеры доступны администратору"));
                    return;
                }
                if (QWidget *win = window()) {
                    if (auto *mw = qobject_cast<QMainWindow *>(win)) {
                        if (auto *nav = mw->findChild<SideNav *>())
                            nav->setCurrent(1, true);
                    }
                }
                emit message(T("Открыт раздел «Камеры»"));
            });
            menu.addSeparator();
            QAction *deleteCamera = menu.addAction(T("Удалить эту камеру / канал регистратора…"));
            deleteCamera->setEnabled(Users::current().isAdmin());
            connect(deleteCamera, &QAction::triggered, this, [this, cameraId] {
                const CameraEntry *current = m_mgr->find(cameraId);
                if (!current)
                    return;
                const QString deviceName = current->name;
                QMessageBox box(QMessageBox::Warning, T("Удаление устройства"),
                    T("Удалить «%1» из PSVms?\nЗапись будет остановлена, устройство исчезнет из списка. Уже сохранённые файлы архива останутся на диске.").arg(deviceName),
                    QMessageBox::NoButton, this);
                QPushButton *removeButton = box.addButton(T("Удалить"), QMessageBox::DestructiveRole);
                box.addButton(T("Отмена"), QMessageBox::RejectRole);
                box.exec();
                if (box.clickedButton() != removeButton)
                    return;
                m_mgr->remove(cameraId);
            });
        }
        if (isGroup) {
            menu.addSeparator();
            QAction *startGrpRec = menu.addAction(T("Запустить запись для группы"));
            connect(startGrpRec, &QAction::triggered, this, [this, targets] {
                for (const CameraEntry &entry : targets)
                    m_mgr->startCamera(entry.id);
                emit message(T("Запуск записи для группы (%1)").arg(targets.size()));
            });
            QAction *stopGrpRec = menu.addAction(T("Остановить запись для группы"));
            connect(stopGrpRec, &QAction::triggered, this, [this, targets] {
                for (const CameraEntry &entry : targets)
                    m_mgr->stopCamera(entry.id);
                emit message(T("Остановка записи для группы (%1)").arg(targets.size()));
            });
            menu.addSeparator();
            QMenu *streamMenu = menu.addMenu(T("Поток для всей группы"));
            auto *streamActions = new QActionGroup(streamMenu);
            streamActions->setExclusive(true);
            const QList<QPair<int, QString>> streamModes = {
                {0, T("Автоматически (основной / дополнительный)")},
                {1, T("Основной поток")},
                {2, T("Дополнительный поток")}
            };
            int commonQuality = targets.first().viewQuality;
            for (const CameraEntry &entry : targets)
                if (entry.viewQuality != commonQuality) commonQuality = -1;
            for (const auto &mode : streamModes) {
                QAction *action = streamMenu->addAction(mode.second);
                action->setCheckable(true);
                action->setChecked(commonQuality == mode.first);
                streamActions->addAction(action);
                connect(action, &QAction::triggered, this, [this, targets, quality = mode.first] {
                    for (const CameraEntry &entry : targets) {
                        if (const CameraEntry *current = m_mgr->find(entry.id)) {
                            CameraEntry updated = *current;
                            updated.viewQuality = quality;
                            m_mgr->update(updated);
                        }
                    }
                    // Применяем смену адреса потока сразу ко всем уже открытым ячейкам.
                    m_grid->refreshCells();
                });
            }
        }

        menu.addSeparator();
        bool allFilled = true;
        for (const CameraEntry &e : targets) allFilled = allFilled && e.fillCell;
        const QString actionText = isGroup
            ? T("Заполнить ячейки всей группы (обрезать края)")
            : T("Заполнить всю ячейку (обрезать края)");
        QAction *toggle = menu.addAction(actionText);
        toggle->setCheckable(true);
        toggle->setChecked(allFilled);
        connect(toggle, &QAction::triggered, this, [this, targets, allFilled] {
            for (const CameraEntry &entry : targets) {
                if (const CameraEntry *current = m_mgr->find(entry.id)) {
                    CameraEntry updated = *current;
                    updated.fillCell = !allFilled;
                    m_mgr->update(updated);
                }
            }
        });
        if (isGroup) {
            QAction *disable = menu.addAction(T("Отключить заполнение для группы"));
            connect(disable, &QAction::triggered, this, [this, targets] {
                for (const CameraEntry &entry : targets) {
                    if (const CameraEntry *current = m_mgr->find(entry.id)) {
                        CameraEntry updated = *current; updated.fillCell = false; m_mgr->update(updated);
                    }
                }
            });
        }
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        const QString id = it->data(0, Qt::UserRole).toString();
        if (!id.isEmpty())
            m_grid->openCamera(id);
    });
    connect(m_grid, &VideoGrid::archiveRequested, this, &LiveViewPage::openArchive);
    connect(m_grid, &VideoGrid::addCameraRequested, this, &LiveViewPage::addCameraRequested);
    connect(m_grid, &VideoGrid::message, this, &LiveViewPage::message);
    connect(m_grid, &VideoGrid::selectionChanged, ptz, &PtzPanel::setCamera);
    connect(m_mgr, &CameraManager::camerasChanged, this, &LiveViewPage::rebuildTree);
    connect(m_mgr, &CameraManager::cameraStateChanged, this, &LiveViewPage::refreshStates);
    connect(m_mgr, &CameraManager::onlineChanged, this, &LiveViewPage::refreshStates);

    rebuildTree();
    m_grid->restoreLayout();     // камеры возвращаются на свои места (без запуска, если отключён автозапуск)
    applyLayoutMetricsNow();
    installCameraButtonAutoFit(this);
}

namespace {
QWidget *activateExistingPsvmsWindow(const QString &key)
{
    if (!qApp) return nullptr;
    const auto windows = QApplication::topLevelWidgets();
    for (QWidget *window : windows) {
        if (!window || window->property("psvms.singleton").toString() != key)
            continue;
        if (window->isMinimized())
            window->showNormal();
        else
            window->show();
        window->raise();
        window->activateWindow();
        return window;
    }
    return nullptr;
}
}

void LiveViewPage::openArchive(const QString &cameraId)
{
    // Архив — единое окно на всё приложение: повторный вызов активирует уже открытое.
    if (activateExistingPsvmsWindow(QStringLiteral("archive")))
        return;

    auto *w = new PlaybackWindow(m_mgr, cameraId, this);
    w->setProperty("psvms.singleton", QStringLiteral("archive"));
    w->show();
    w->raise();
    w->activateWindow();
}

void LiveViewPage::openArchiveForSelectedCamera()
{
    openArchive(m_grid ? m_grid->selectedCameraId() : QString());
}

void LiveViewPage::exportArchiveIntervalForCamera(const QString &cameraId, qint64 fromMs, qint64 toMs)
{
    if (!canCurrentUserPerformAction(QStringLiteral("archive.export")) && !Users::current().isAdmin()) {
        emit message(T("Недостаточно прав для экспорта фрагмента архива."));
        return;
    }
    if (cameraId.trimmed().isEmpty()) {
        emit message(T("Выберите камеру для экспорта фрагмента архива."));
        return;
    }
    if (toMs <= fromMs) {
        emit message(T("Некорректный временной интервал экспорта."));
        return;
    }
    const QString suggestedName = QStringLiteral("psvms_export_%1_%2.json")
        .arg(cameraId)
        .arg(QDateTime::fromMSecsSinceEpoch(fromMs).toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString outPath = QFileDialog::getSaveFileName(
        this, T("Экспорт интервала архива"), suggestedName,
        T("Плейлист фрагмента архива (*.json);;Все файлы (*)"));
    if (outPath.isEmpty())
        return;

    RecordingIndex idx;
    const QString dbPath = m_mgr->storageRoot() + QStringLiteral("/segments.sqlite");
    QList<SegmentInfo> matched;
    if (idx.open(dbPath)) {
        matched = idx.segmentsInRange(cameraId, fromMs, toMs);
        idx.close();
    }
    QString err;
    if (!writeArchiveRangeManifest(cameraId, outPath, fromMs, toMs, matched, &err)) {
        emit message(err);
        return;
    }
    emit message(T("Экспортирован фрагмент архива (%1 сегм.): %2").arg(matched.size()).arg(outPath));
}



void LiveViewPage::rebuildTemplatesMenu()
{
    QMenu *menu = m_tplBtn->menu();
    menu->clear();
    connect(menu->addAction(T("Сохранить текущее расположение как шаблон…")), &QAction::triggered, this, [this] {
        QInputDialog dlg(this);
        dlg.setWindowTitle(T("Новый шаблон"));
        dlg.setLabelText(T("Название шаблона:"));
        dlg.setOkButtonText(T("ОК"));
        dlg.setCancelButtonText(T("Отмена"));
        if (dlg.exec() == QDialog::Accepted && !dlg.textValue().trimmed().isEmpty())
            m_grid->saveTemplate(dlg.textValue().trimmed());
    });

    const QStringList names = m_grid->templateNames();
    if (!names.isEmpty()) {
        menu->addSeparator();
        for (const QString &n : names)
            connect(menu->addAction(n), &QAction::triggered, this, [this, n] { m_grid->applyTemplateNamed(n); });
        menu->addSeparator();
        QMenu *del = menu->addMenu(T("Удалить шаблон"));
        for (const QString &n : names)
            connect(del->addAction(n), &QAction::triggered, this, [this, n] { m_grid->removeTemplate(n); });
    }
}

void LiveViewPage::rebuildTree()
{
    m_tree->clear();
    m_items.clear();

    QMap<QString, QTreeWidgetItem *> groups;
    for (const CameraEntry &e : m_mgr->cameras()) {
        const QString g = e.group.isEmpty() ? T("Без группы") : e.group;
        QTreeWidgetItem *parent = groups.value(g, nullptr);
        if (!parent) {
            parent = new QTreeWidgetItem(m_tree, QStringList(g));
            parent->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            groups.insert(g, parent);
        }
        auto *it = new QTreeWidgetItem(parent, QStringList(e.name));
        it->setData(0, Qt::UserRole, e.id);
        it->setData(0, Qt::UserRole + 1, e.ip + QLatin1Char(' ') + maskCredentialsInUrl(e.url));
        it->setIcon(0, liveDotIcon(liveStateColor(m_mgr, e.id)));
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        m_items.insert(e.id, it);
    }
    m_tree->expandAll();
    applyFilter(m_search->text());
}

void LiveViewPage::refreshStates()
{
    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it)
        it.value()->setIcon(0, liveDotIcon(liveStateColor(m_mgr, it.key())));
}

void LiveViewPage::applyFilter(const QString &text)
{
    const QString t = text.trimmed();
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *grp = m_tree->topLevelItem(g);
        const bool groupMatch = !t.isEmpty()
            && grp->text(0).contains(t, Qt::CaseInsensitive);
        int visible = 0;
        for (int i = 0; i < grp->childCount(); ++i) {
            QTreeWidgetItem *c = grp->child(i);
            const QString meta = c->data(0, Qt::UserRole + 1).toString();
            const bool show = t.isEmpty()
                || groupMatch
                || c->text(0).contains(t, Qt::CaseInsensitive)
                || meta.contains(t, Qt::CaseInsensitive);
            c->setHidden(!show);
            if (show)
                ++visible;
        }
        grp->setHidden(visible == 0);
    }
}

void LiveViewPage::setPanelVisible(bool on)
{
    if (m_panelAnim)
        m_panelAnim->stop();

    m_leftPanel->setMinimumWidth(0);
    if (on)
        m_leftPanel->show();

    if (!Cfg::view().animations) {
        m_leftPanel->setMaximumWidth(on ? 380 : 0);
        if (on) {
            m_leftPanel->setMinimumWidth(230);
        } else {
            m_leftPanel->hide();
        }
        return;
    }
    auto *anim = new QPropertyAnimation(m_leftPanel, "maximumWidth", this);
    anim->setDuration(240);
    anim->setEasingCurve(QEasingCurve::InOutCubic);
    anim->setStartValue(m_leftPanel->width());
    const int targetW = m_leftPanel->maximumWidth() > 0 ? m_leftPanel->maximumWidth() : 260;
    anim->setEndValue(on ? targetW : 0);
    connect(anim, &QPropertyAnimation::finished, this, [this, on] {
        if (on) {
            applyLayoutMetricsNow();
        } else {
            m_leftPanel->hide();
        }
    });
    m_panelAnim = anim;
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void LiveViewPage::applyLayoutMetricsNow()
{
    QSettings s;
    const int leftPct = qBound(70, s.value(QStringLiteral("ui/leftPanelWidthPercent"), 100).toInt(), 140);
    int screenBonus = 0;
    if (QScreen *scr = screen() ? screen() : QGuiApplication::primaryScreen()) {
        if (scr->geometry().width() >= 3440)
            screenBonus = 20;
    }
    const int minLeftW = qMax(140, qRound((190 + screenBonus) * leftPct / 100.0));
    const int maxLeftW = qMax(minLeftW + 30, qRound((260 + screenBonus) * leftPct / 100.0));
    if (m_leftPanel) {
        m_leftPanel->setMinimumWidth(minLeftW);
        m_leftPanel->setMaximumWidth(maxLeftW);
    }

    const int gapPct = qBound(25, s.value(QStringLiteral("ui/gridGapPercent"), 100).toInt(), 200);
    const int gridMargin = qMax(1, qRound(4 * gapPct / 100.0));
    if (m_grid) {
        m_grid->setContentsMargins(gridMargin, gridMargin, gridMargin, gridMargin);
        if (auto *gl = qobject_cast<QGridLayout *>(m_grid->layout()))
            gl->setSpacing(qMax(1, qRound(3 * gapPct / 100.0)));
    }
}

// ======================================================================
