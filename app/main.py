
import sys, os, io, json, logging, shutil, subprocess, platform, gc, time, sqlite3, csv, re
from contextlib import contextmanager
from pathlib import Path
from datetime import datetime
from PIL import Image, ImageOps, ImageDraw, ImageFont, ImageEnhance
from PySide6.QtCore import Qt, QSize, QTimer, QDate, QEvent
from PySide6.QtGui import QPixmap, QImage, QIcon, QFont, QColor, QPainter, QLinearGradient, QPen, QDragEnterEvent, QDropEvent
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QLabel, QPushButton, QFileDialog, QListWidget, QSplashScreen,
    QListWidgetItem, QVBoxLayout, QHBoxLayout, QGridLayout, QTabWidget, QSpinBox,
    QDoubleSpinBox, QComboBox, QCheckBox, QLineEdit, QMessageBox, QGroupBox,
    QSplitter, QProgressBar, QPlainTextEdit, QFormLayout, QAbstractItemView, QSizePolicy,
    QTableWidget, QTableWidgetItem, QDialog, QDialogButtonBox, QDateEdit, QHeaderView, QMenu, QSlider, QInputDialog
)
from updater import UpdateDialog, APP_VERSION, DEFAULT_UPDATE_SOURCE_URL
from network_sync import NetworkProfile, load_profile as load_network_profile, save_profile as save_network_profile, probe_server
from employee_sync import ensure_employee_sync_schema, sync_once as sync_employee_records, queue_existing_employees
from version_utils import startup_window_metrics
from hub_integration import (inspect_template, load_card, find_photo_element, atomic_write_card, safe_component,
                              discover_hub_executable, discover_hub_templates, copy_hub_template, open_card_file, compare_template_preservation, extract_card_preview)
from quality_control import assess_photo, format_report
from photo_intelligence import assess_employee_photos, write_photo_quality_csv
from photo_duplicates import find_photo_duplicates
from employee_data_protection import inspect_employee_records, format_employee_integrity_report
from employee_history import ensure_employee_history_schema, record_employee_change, list_employee_changes, restore_employee_change
from expiry_control import ensure_expiry_control_schema, record_status_change, renew_many, list_status_history
from photo_studio import replace_background, BACKGROUNDS
from smart_cards import CARD_TYPES, CARD_STATUSES, normalize_card_type, normalize_card_status, can_issue, reissue_reason
from employee_lifecycle import expiry_status, hash_pin, verify_pin, role_allows, bulk_expiry_update, expiry_label, parse_date
from security_center import PinAttemptGuard, encrypt_backup, decrypt_backup, integrity_check, expire_active_cards
from security_audit import ensure_security_audit_schema, record_security_event, summarize_security_events, list_security_events
from operator_accounts import ensure_operator_schema, create_account, update_account_pin, set_account_enabled, delete_account, list_accounts, authenticate as authenticate_operator, account_allows, get_account_permissions, update_account_permissions, unlock_account, PERMISSIONS
from operator_sessions import ensure_operator_sessions_schema, start_session, end_session, recover_interrupted_sessions, list_sessions, session_summary, format_duration, duration_seconds, export_sessions_csv
from analytics import summarize_employees, expiry_rows
from access_dashboard import build_access_overview, LABELS as ACCESS_STATE_LABELS
from database_health import inspect_database_health, format_database_health_report
from batch_issuance import issuance_block_reason
from access_control import CARD_EVENTS, validate_card_event, insert_card_event
from batch_studio import index_photo_files, match_employee_photo, validate_batch_size
from organization_catalog import normalize_organization_name
from template_profiles import set_profile, get_profile_template, remove_profile
from card_designer import CardDesignerDialog
from card_personalization import personalize_design
from card_preview import CardPreviewDialog, render_card_preview, inspect_card_layout
from card_preflight import preflight_employee_card, format_preflight
from reporting import build_report, notification_digest, export_csv as export_full_report_csv, export_xlsx as export_full_report_xlsx, export_pdf as export_full_report_pdf
from backup_recovery import create_full_backup, inspect_full_backup, restore_full_backup, prune_full_backups, audit_backup_directory, rehearse_backup_restore
from professional_release import readiness_checks, summarize_checks
from support_bundle import create_support_bundle
from runtime_safety import acquire_single_instance_lock

APP_NAME = "PS PassMaster PRO 8.9.0 — Employee LAN Sync"
DEVELOPER_NAME = "P.Sveshnikov"

# Estimated portrait frame measured proportionally from the user's HUB screenshot.
VNUKOVO_PHOTO_WIDTH_MM = 21.8
VNUKOVO_PHOTO_HEIGHT_MM = 29.0
VNUKOVO_PHOTO_WIDTH_CM = VNUKOVO_PHOTO_WIDTH_MM / 10
VNUKOVO_PHOTO_HEIGHT_CM = VNUKOVO_PHOTO_HEIGHT_MM / 10
VNUKOVO_PHOTO_SUFFIX = "21_8x29mm_300dpi"

# Portable mode: all application-owned files stay beside the program.
if getattr(sys, "frozen", False):
    ROOT = Path(sys.executable).resolve().parent
else:
    ROOT = Path(__file__).resolve().parent.parent

# Installed builds store mutable data in the user profile; portable builds opt in
# with portable.flag next to the EXE. Source/development runs remain self-contained.
if getattr(sys, "frozen", False) and not (ROOT / "portable.flag").exists():
    _local = Path(os.environ.get("LOCALAPPDATA") or Path.home() / ".local" / "share")
    APP_DATA_DIR = _local / "PSPassMasterPRO"
else:
    APP_DATA_DIR = ROOT

CONFIG_DIR = APP_DATA_DIR / "Config"
LOG_DIR = APP_DATA_DIR / "Logs"
CACHE_DIR = APP_DATA_DIR / "Cache"
HISTORY_DIR = APP_DATA_DIR / "History"
OUTPUT_DIR = APP_DATA_DIR / "Exports"
TEMP_DIR = APP_DATA_DIR / "Temp"
BACKUP_DIR = APP_DATA_DIR / "Backups"
EMPLOYEE_DIR = APP_DATA_DIR / "Employees"
DB_FILE = EMPLOYEE_DIR / "employees.sqlite3"
RESOURCES_DIR = ROOT / "Resources"

for folder in (
    CONFIG_DIR, LOG_DIR, CACHE_DIR, HISTORY_DIR,
    OUTPUT_DIR, TEMP_DIR, BACKUP_DIR, EMPLOYEE_DIR, RESOURCES_DIR
):
    folder.mkdir(parents=True, exist_ok=True)

for stale_file in TEMP_DIR.iterdir():
    try:
        if stale_file.is_file():
            stale_file.unlink()
        elif stale_file.is_dir():
            shutil.rmtree(stale_file, ignore_errors=True)
    except Exception:
        pass

logging.basicConfig(
    filename=str(LOG_DIR / "pspassmaster.log"), level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s", encoding="utf-8"
)
HISTORY_FILE = HISTORY_DIR / "history.json"
SETTINGS_FILE = CONFIG_DIR / "settings.json"
PHOTO_EDITS_FILE = CONFIG_DIR / "photo_edits.json"
NETWORK_PROFILE_FILE = CONFIG_DIR / "network.json"
# Локальная база сотрудников. Фотографии и персональные данные остаются на этом ПК.
@contextmanager
def db_connect():
    con = sqlite3.connect(DB_FILE, timeout=30)
    con.execute("PRAGMA busy_timeout=30000")
    con.execute("PRAGMA foreign_keys=ON")
    con.row_factory = sqlite3.Row
    try:
        yield con
        con.commit()
    except Exception:
        con.rollback()
        raise
    finally:
        con.close()

def create_database_backup(reason="manual"):
    """Create a consistent timestamped SQLite backup before data-changing operations."""
    if not DB_FILE.exists():
        return None
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    safe_reason = re.sub(r"[^A-Za-z0-9_-]+", "_", str(reason))[:32]
    target = BACKUP_DIR / f"employees_{stamp}_{safe_reason}.sqlite3"
    source_con = sqlite3.connect(DB_FILE)
    target_con = sqlite3.connect(target)
    try:
        source_con.backup(target_con)
        check = target_con.execute("PRAGMA integrity_check").fetchone()
        if not check or check[0] != "ok":
            raise sqlite3.DatabaseError(f"Backup integrity check failed: {check[0] if check else 'no result'}")
    except Exception:
        try:
            target_con.close()
        finally:
            source_con.close()
        try:
            target.unlink(missing_ok=True)
        except Exception:
            pass
        raise
    else:
        target_con.close()
        source_con.close()
    # Keep the latest 30 automatic backups to avoid unbounded disk usage.
    backups = sorted(BACKUP_DIR.glob("employees_*.sqlite3"), key=lambda p: p.stat().st_mtime, reverse=True)
    for old in backups[30:]:
        try:
            old.unlink()
        except OSError:
            logging.warning("Could not remove old backup %s", old)
    return target


def create_daily_backup_if_needed():
    """Create at most one verified automatic database backup per local calendar day."""
    today = datetime.now().strftime("%Y%m%d")
    if any(p.name.startswith(f"employees_{today}_") for p in BACKUP_DIR.glob("employees_*.sqlite3")):
        return None
    return create_database_backup("daily")


def create_daily_full_backup_if_needed():
    """Create one portable full-data backup per day and apply the retention policy."""
    today = datetime.now().strftime("%Y%m%d")
    if any(p.name.startswith(f"PSPassMaster_full_{today}_") for p in BACKUP_DIR.glob("PSPassMaster_full_*.pspmbackup")):
        return None
    target = BACKUP_DIR / f"PSPassMaster_full_{datetime.now().strftime('%Y%m%d_%H%M%S')}.pspmbackup"
    result = create_full_backup(DB_FILE, APP_DATA_DIR, ROOT / "Templates", target)
    prune_full_backups(BACKUP_DIR, keep=int(load_json(SETTINGS_FILE, {}).get("backup_retention", 14)))
    return result


def init_employee_db():
    # Preserve a verified pre-migration snapshot when upgrading an existing database.
    if DB_FILE.exists():
        try:
            probe = sqlite3.connect(DB_FILE)
            exists = probe.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='employees'").fetchone()
            existing_columns = {row[1] for row in probe.execute("PRAGMA table_info(employees)").fetchall()} if exists else set()
            probe.close()
            if exists and not {"is_deleted", "deleted_at"}.issubset(existing_columns):
                create_database_backup("before_3_7_migration")
        except Exception:
            logging.exception("Could not create pre-migration backup")
            raise
    with db_connect() as con:
        con.execute("""CREATE TABLE IF NOT EXISTS employees (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            full_name TEXT NOT NULL, organization TEXT DEFAULT '', department TEXT DEFAULT '',
            position TEXT DEFAULT '', badge_number TEXT DEFAULT '', valid_until TEXT DEFAULT '',
            photo_path TEXT DEFAULT '', issue_date TEXT DEFAULT '', created_at TEXT NOT NULL, updated_at TEXT NOT NULL)""")
        columns = {row[1] for row in con.execute("PRAGMA table_info(employees)").fetchall()}
        if "issue_date" not in columns:
            con.execute("ALTER TABLE employees ADD COLUMN issue_date TEXT DEFAULT ''")
        if "is_deleted" not in columns:
            con.execute("ALTER TABLE employees ADD COLUMN is_deleted INTEGER NOT NULL DEFAULT 0")
        if "deleted_at" not in columns:
            con.execute("ALTER TABLE employees ADD COLUMN deleted_at TEXT DEFAULT ''")
        for column, declaration in (("card_type", "TEXT NOT NULL DEFAULT 'Постоянный'"), ("card_status", "TEXT NOT NULL DEFAULT 'Активен'"), ("blocked_reason", "TEXT DEFAULT ''")):
            if column not in columns:
                con.execute(f"ALTER TABLE employees ADD COLUMN {column} {declaration}")
        con.execute("CREATE INDEX IF NOT EXISTS ix_employees_card_status ON employees(card_status, card_type)")
        con.execute("""CREATE TABLE IF NOT EXISTS card_movements (
            id INTEGER PRIMARY KEY AUTOINCREMENT, employee_id INTEGER,
            full_name TEXT NOT NULL DEFAULT '', badge_number TEXT NOT NULL DEFAULT '',
            event_type TEXT NOT NULL, note TEXT DEFAULT '', operator TEXT DEFAULT '',
            created_at TEXT NOT NULL
        )""")
        con.execute("CREATE INDEX IF NOT EXISTS ix_card_movements_employee ON card_movements(employee_id, id DESC)")
        con.execute("CREATE TABLE IF NOT EXISTS organization_catalog (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL COLLATE NOCASE UNIQUE, created_at TEXT NOT NULL)")
        con.execute("INSERT OR IGNORE INTO organization_catalog(name, created_at) SELECT DISTINCT TRIM(organization), ? FROM employees WHERE TRIM(COALESCE(organization, '')) <> ''", (datetime.now().isoformat(timespec="seconds"),))
        con.execute("CREATE TABLE IF NOT EXISTS card_reissues (id INTEGER PRIMARY KEY AUTOINCREMENT, employee_id INTEGER, old_badge_number TEXT DEFAULT '', new_badge_number TEXT DEFAULT '', reason TEXT NOT NULL, created_at TEXT NOT NULL)")
        con.execute("CREATE INDEX IF NOT EXISTS ix_employees_deleted ON employees(is_deleted, full_name)")
        ensure_employee_history_schema(con)
        ensure_security_audit_schema(con)
        ensure_operator_schema(con)
        ensure_operator_sessions_schema(con)
        ensure_expiry_control_schema(con)
        ensure_employee_sync_schema(con)
        con.execute("CREATE TABLE IF NOT EXISTS app_roles (role TEXT PRIMARY KEY, pin_salt TEXT NOT NULL, pin_hash TEXT NOT NULL, iterations INTEGER NOT NULL, updated_at TEXT NOT NULL)")
        con.execute("""CREATE TABLE IF NOT EXISTS issuance_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            employee_id INTEGER,
            full_name TEXT NOT NULL,
            badge_number TEXT DEFAULT '',
            issue_date TEXT DEFAULT '',
            card_file TEXT DEFAULT '',
            created_at TEXT NOT NULL
        )""")
        con.execute("""CREATE TABLE IF NOT EXISTS audit_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            event_type TEXT NOT NULL,
            employee_id INTEGER,
            full_name TEXT DEFAULT '',
            badge_number TEXT DEFAULT '',
            details TEXT DEFAULT '',
            operator TEXT DEFAULT '',
            created_at TEXT NOT NULL
        )""")
        try:
            con.execute("""CREATE UNIQUE INDEX IF NOT EXISTS ux_employees_badge_number
                ON employees(badge_number) WHERE badge_number IS NOT NULL AND badge_number<>''""")
        except sqlite3.IntegrityError:
            logging.error("Legacy duplicate badge numbers found; application-level duplicate checks remain enabled.")


init_employee_db()


def write_audit(con, event_type, employee_id=None, full_name="", badge_number="", details="", operator=""):
    """Write a local audit event inside the caller's SQLite transaction."""
    con.execute(
        "INSERT INTO audit_log(event_type,employee_id,full_name,badge_number,details,operator,created_at) "
        "VALUES(?,?,?,?,?,?,?)",
        (event_type, employee_id, full_name or "", badge_number or "", details or "",
         operator or os.environ.get("USERNAME") or os.environ.get("USER") or "local",
         datetime.now().isoformat(timespec="seconds"))
    )


SUPPORTED = {".jpg", ".jpeg", ".png", ".webp", ".tif", ".tiff", ".bmp", ".gif"}

def load_json(path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8")) if path.exists() else default
    except Exception:
        logging.exception("Не удалось прочитать %s", path)
        return default

def save_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")

def make_photo(source, width_cm=3.0, height_cm=4.0, dpi=300, mode="Заполнить (обрезать)", focus_x=0.5, focus_y=0.5, zoom=1.0, edits=None):
    with Image.open(source) as raw:
        im = ImageOps.exif_transpose(raw).convert("RGB")
        edits = edits or {}
        bg_name = edits.get("background", "Оригинал")
        if bg_name != "Оригинал":
            im = replace_background(im, bg_name)
        rotation = int(edits.get("rotation", 0)) % 360
        if rotation:
            im = im.rotate(-rotation, expand=True, resample=Image.Resampling.BICUBIC)
        if edits.get("flip_h"):
            im = ImageOps.mirror(im)
        if edits.get("flip_v"):
            im = ImageOps.flip(im)
        for key, enhancer in (("brightness", ImageEnhance.Brightness), ("contrast", ImageEnhance.Contrast),
                              ("color", ImageEnhance.Color), ("sharpness", ImageEnhance.Sharpness)):
            value = float(edits.get(key, 1.0))
            if value != 1.0:
                im = enhancer(im).enhance(value)
        w, h = max(1, round(width_cm / 2.54 * dpi)), max(1, round(height_cm / 2.54 * dpi))
        if mode.startswith("Заполнить"):
            src_ratio, dst_ratio = im.width / im.height, w / h
            # Выбираем область нужного соотношения сторон. focus_x/focus_y задают
            # положение рамки, zoom > 1 приближает изображение внутри рамки.
            if src_ratio > dst_ratio:
                crop_h = im.height / zoom
                crop_w = crop_h * dst_ratio
                if crop_w > im.width:
                    crop_w = im.width / zoom
                    crop_h = crop_w / dst_ratio
            else:
                crop_w = im.width / zoom
                crop_h = crop_w / dst_ratio
                if crop_h > im.height:
                    crop_h = im.height / zoom
                    crop_w = crop_h * dst_ratio
            cx = min(max(focus_x, 0.0), 1.0) * im.width
            cy = min(max(focus_y, 0.0), 1.0) * im.height
            left = min(max(0, round(cx - crop_w / 2)), max(0, round(im.width - crop_w)))
            top = min(max(0, round(cy - crop_h / 2)), max(0, round(im.height - crop_h)))
            right = min(im.width, round(left + crop_w))
            bottom = min(im.height, round(top + crop_h))
            im = im.crop((left, top, right, bottom))
            im = im.resize((w, h), Image.Resampling.LANCZOS)
        else:
            im.thumbnail((w, h), Image.Resampling.LANCZOS)
            canvas = Image.new("RGB", (w, h), "white")
            canvas.paste(im, ((w-im.width)//2, (h-im.height)//2))
            im = canvas
        return im, w, h

def detect_face_focus(source, rotation=0, flip_h=False, flip_v=False):
    """Return normalized crop focus around the largest detected face.

    OpenCV is optional. When unavailable or no face is detected, return None
    so the normal centered/manual crop remains available.
    """
    try:
        import cv2
        import numpy as np
        with Image.open(source) as raw:
            oriented = ImageOps.exif_transpose(raw).convert("RGB")
            if rotation:
                oriented = oriented.rotate(-int(rotation) % 360, expand=True, resample=Image.Resampling.BICUBIC)
            if flip_h: oriented = ImageOps.mirror(oriented)
            if flip_v: oriented = ImageOps.flip(oriented)
            rgb = np.array(oriented)
        gray = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY)
        cascade_path = str(Path(cv2.data.haarcascades) / "haarcascade_frontalface_default.xml")
        cascade = cv2.CascadeClassifier(cascade_path)
        faces = cascade.detectMultiScale(gray, scaleFactor=1.1, minNeighbors=5, minSize=(36, 36))
        if len(faces) == 0:
            return None
        x, y, w, h = max(faces, key=lambda r: int(r[2]) * int(r[3]))
        # Place crop center slightly below face center to retain head and shoulders.
        cx = (x + w / 2) / rgb.shape[1]
        cy = min(0.92, (y + h * 0.72) / rgb.shape[0])
        # Aim for a natural head-and-shoulders portrait; never force excessive zoom.
        suggested_zoom = min(3.0, max(1.0, rgb.shape[0] / max(1.0, h * 3.2)))
        return (float(cx), float(cy), (int(x), int(y), int(w), int(h)), float(suggested_zoom))
    except ImportError:
        return None
    except Exception:
        logging.exception("Face detection failed: %s", source)
        return None


class DropList(QListWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAcceptDrops(True)
        self.setDragDropMode(QAbstractItemView.DropOnly)
        self.setSelectionMode(QAbstractItemView.ExtendedSelection)
    def dragEnterEvent(self, e: QDragEnterEvent):
        if e.mimeData().hasUrls(): e.acceptProposedAction()
        else: e.ignore()
    def dragMoveEvent(self, e): e.acceptProposedAction()
    def dropEvent(self, e: QDropEvent):
        paths = [u.toLocalFile() for u in e.mimeData().urls()]
        self.window().add_files(paths)
        e.acceptProposedAction()


class CropPreview(QLabel):
    """Preview with drag-to-reframe and mouse-wheel zoom."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAlignment(Qt.AlignCenter)
        self.setMinimumSize(320, 420)
        self.setMouseTracking(True)
        self.focus_x = 0.5
        self.focus_y = 0.5
        self.zoom = 1.0
        self.on_change = None
        self._drag_pos = None
        self.setToolTip("Перетаскивайте фото мышью, чтобы изменить кадр. Колёсико мыши — приблизить/отдалить.")

    def mousePressEvent(self, event):
        if event.button() == Qt.LeftButton:
            self._drag_pos = event.position()
            self.setCursor(Qt.ClosedHandCursor)
            event.accept()
        else:
            super().mousePressEvent(event)

    def mouseMoveEvent(self, event):
        if self._drag_pos is not None:
            delta = event.position() - self._drag_pos
            self._drag_pos = event.position()
            # Moving the visible crop frame to the right reveals more of the
            # source image on the left, hence the inverse movement of focus.
            pix = self.pixmap()
            rendered_w = pix.width() if pix and not pix.isNull() else self.width()
            rendered_h = pix.height() if pix and not pix.isNull() else self.height()
            self.focus_x = min(1.0, max(0.0, self.focus_x - delta.x() / max(1, rendered_w)))
            self.focus_y = min(1.0, max(0.0, self.focus_y - delta.y() / max(1, rendered_h)))
            if self.on_change:
                self.on_change()
            event.accept()
        else:
            super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.LeftButton:
            self._drag_pos = None
            self.setCursor(Qt.ArrowCursor)
            event.accept()
        else:
            super().mouseReleaseEvent(event)

    def wheelEvent(self, event):
        step = 1.08 if event.angleDelta().y() > 0 else 1 / 1.08
        self.zoom = min(3.0, max(1.0, self.zoom * step))
        if self.on_change:
            self.on_change()
        event.accept()

    def reset_crop(self):
        self.focus_x = 0.5
        self.focus_y = 0.5
        self.zoom = 1.0
        if self.on_change:
            self.on_change()



class PhotoEditorDialog(QDialog):
    """Non-destructive photo editor: tonal controls and reversible geometry."""
    def __init__(self, source, edits=None, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Редактор фотографии — Magicard HUB")
        self.source = str(source)
        self.edits = dict(edits or {})
        self.edits.setdefault("rotation", 0)
        for key in ("brightness", "contrast", "color", "sharpness"):
            self.edits.setdefault(key, 1.0)
        self.edits.setdefault("flip_h", False); self.edits.setdefault("flip_v", False)
        self.edits.setdefault("background", "Оригинал")
        screen = QApplication.primaryScreen()
        area = screen.availableGeometry() if screen else None
        self.resize(min(900, area.width()-80) if area else 820, min(760, area.height()-80) if area else 680)
        root = QVBoxLayout(self)
        self.preview = QLabel("Предпросмотр")
        self.preview.setAlignment(Qt.AlignCenter); self.preview.setMinimumSize(280, 260)
        self.preview.setStyleSheet("background:#20252B;border:1px solid #59616b;border-radius:8px;")
        root.addWidget(self.preview, 1)
        actions = QHBoxLayout()
        for label, key, delta in (("↶ Поворот −90°", "rotation", -90), ("Поворот +90° ↷", "rotation", 90)):
            b=QPushButton(label); b.clicked.connect(lambda _, k=key, d=delta: self.rotate(d)); actions.addWidget(b)
        self.flip_h_btn=QPushButton("Отразить ↔"); self.flip_h_btn.setCheckable(True); self.flip_h_btn.setChecked(bool(self.edits["flip_h"])); self.flip_h_btn.toggled.connect(lambda v:self.set_edit("flip_h",v)); actions.addWidget(self.flip_h_btn)
        self.flip_v_btn=QPushButton("Отразить ↕"); self.flip_v_btn.setCheckable(True); self.flip_v_btn.setChecked(bool(self.edits["flip_v"])); self.flip_v_btn.toggled.connect(lambda v:self.set_edit("flip_v",v)); actions.addWidget(self.flip_v_btn)
        root.addLayout(actions)
        bg_row = QHBoxLayout()
        bg_row.addWidget(QLabel("Фон портрета:"))
        self.background_combo = QComboBox()
        self.background_combo.addItems(list(BACKGROUNDS.keys()))
        self.background_combo.setCurrentText(self.edits.get("background", "Оригинал"))
        self.background_combo.setToolTip("Автоматическое отделение человека от фона. Результат можно отменить выбором «Оригинал».")
        self.background_combo.currentTextChanged.connect(lambda value: self.set_edit("background", value))
        bg_row.addWidget(self.background_combo, 1)
        root.addLayout(bg_row)
        self.sliders={}
        grid=QGridLayout()
        labels=(("brightness","Яркость"),("contrast","Контраст"),("color","Насыщенность"),("sharpness","Резкость"))
        for row,(key,label) in enumerate(labels):
            grid.addWidget(QLabel(label),row,0)
            slider=QSlider(Qt.Horizontal); slider.setRange(0,200); slider.setValue(round(float(self.edits[key])*100))
            value=QLabel(f"{slider.value()/100:.2f}×"); grid.addWidget(slider,row,1); grid.addWidget(value,row,2)
            slider.valueChanged.connect(lambda v,k=key,l=value: (self.set_edit(k,v/100),l.setText(f"{v/100:.2f}×")))
            self.sliders[key]=slider
        root.addLayout(grid)
        bottom=QHBoxLayout(); reset=QPushButton("Сбросить коррекцию"); reset.clicked.connect(self.reset_edits); bottom.addWidget(reset); bottom.addStretch()
        buttons=QDialogButtonBox(QDialogButtonBox.Save|QDialogButtonBox.Cancel); buttons.accepted.connect(self.accept); buttons.rejected.connect(self.reject); bottom.addWidget(buttons); root.addLayout(bottom)
        self.refresh()

    def set_edit(self,key,value): self.edits[key]=value; self.refresh()
    def rotate(self,delta): self.edits["rotation"]=(int(self.edits.get("rotation",0))+delta)%360; self.refresh()
    def reset_edits(self):
        self.edits={"rotation":0,"flip_h":False,"flip_v":False,"brightness":1.0,"contrast":1.0,"color":1.0,"sharpness":1.0,"background":"Оригинал"}
        self.flip_h_btn.setChecked(False); self.flip_v_btn.setChecked(False)
        self.background_combo.blockSignals(True); self.background_combo.setCurrentText("Оригинал"); self.background_combo.blockSignals(False)
        for slider in self.sliders.values(): slider.blockSignals(True); slider.setValue(100); slider.blockSignals(False)
        self.refresh()
    def refresh(self):
        try:
            im,_,_=make_photo(self.source, VNUKOVO_PHOTO_WIDTH_CM,VNUKOVO_PHOTO_HEIGHT_CM,110,"Заполнить (обрезать)",0.5,0.5,1.0,self.edits)
            buf=io.BytesIO(); im.save(buf,"PNG")
            self.preview.setPixmap(QPixmap.fromImage(QImage.fromData(buf.getvalue())).scaled(self.preview.size(),Qt.KeepAspectRatio,Qt.SmoothTransformation))
            im.close(); buf.close()
        except Exception: logging.exception("Photo editor preview failed: %s",self.source)

    def resizeEvent(self,event): super().resizeEvent(event); self.refresh()


class EmployeeCropDialog(QDialog):
    def __init__(self, source, parent=None, focus_x=0.5, focus_y=0.5, zoom=1.0, edits=None):
        super().__init__(parent)
        self.source = str(source)
        self.edits = dict(edits or {})
        self.setWindowTitle("Кадрирование фотографии для Magicard HUB")
        screen = QApplication.primaryScreen()
        if screen:
            area = screen.availableGeometry()
            dialog_w = min(620, max(320, int(area.width() * 0.78)))
            dialog_h = min(650, max(380, int(area.height() * 0.84)))
            self.resize(dialog_w, dialog_h)
            self.setMinimumSize(min(320, dialog_w), min(380, dialog_h))
        else:
            self.resize(620, 650)
            self.setMinimumSize(320, 380)
        layout = QVBoxLayout(self)
        self.preview = CropPreview()
        self.preview.setMinimumSize(180, 220)
        self.preview.focus_x, self.preview.focus_y, self.preview.zoom = focus_x, focus_y, zoom
        self.preview.on_change = self.refresh_preview
        layout.addWidget(self.preview, 1)
        layout.addWidget(QLabel("Перетаскивайте фото мышью. Стрелки — точная подстройка; колёсико — масштаб."))
        controls = QGridLayout()
        for label, dx, dy, r, c in [
            ("↑", 0, 0.012, 0, 1), ("←", 0.012, 0, 1, 0),
            ("↓", 0, -0.012, 1, 1), ("→", -0.012, 0, 1, 2)
        ]:
            btn = QPushButton(label); btn.setMaximumWidth(64)
            btn.clicked.connect(lambda checked=False, x=dx, y=dy: self.nudge(x, y))
            controls.addWidget(btn, r, c)
        layout.addLayout(controls)
        row = QHBoxLayout()
        reset = QPushButton("Сбросить"); reset.clicked.connect(self.preview.reset_crop)
        row.addWidget(reset); row.addStretch()
        buttons = QDialogButtonBox(QDialogButtonBox.Save | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept); buttons.rejected.connect(self.reject)
        row.addWidget(buttons); layout.addLayout(row)
        self.refresh_preview()

    def refresh_preview(self):
        try:
            im, _, _ = make_photo(self.source, VNUKOVO_PHOTO_WIDTH_CM, VNUKOVO_PHOTO_HEIGHT_CM,
                110, "Заполнить (обрезать)", self.preview.focus_x, self.preview.focus_y, self.preview.zoom, self.edits)
            buf = io.BytesIO(); im.save(buf, "PNG")
            pix = QPixmap.fromImage(QImage.fromData(buf.getvalue())).scaled(
                self.preview.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation)
            self.preview.setPixmap(pix); im.close(); buf.close()
        except Exception:
            logging.exception("Employee crop preview failed")

    def nudge(self, dx, dy):
        self.preview.focus_x = min(1.0, max(0.0, self.preview.focus_x + dx))
        self.preview.focus_y = min(1.0, max(0.0, self.preview.focus_y + dy))
        self.refresh_preview()

    def values(self):
        return self.preview.focus_x, self.preview.focus_y, self.preview.zoom


class EmployeeDialog(QDialog):
    def __init__(self, parent=None, record=None, organizations=None):
        super().__init__(parent)
        self.setWindowTitle("Карточка сотрудника")
        self.setMinimumWidth(460)
        form = QFormLayout(self)
        record = dict(record) if record else {}
        self.full_name = QLineEdit(record.get("full_name", ""))
        self.organization = QComboBox()
        self.organization.setEditable(True)
        self.organization.addItems(list(organizations or []))
        self.organization.setCurrentText(record.get("organization", ""))
        self.organization.setInsertPolicy(QComboBox.NoInsert)
        self.department = QLineEdit(record.get("department", ""))
        self.position = QLineEdit(record.get("position", ""))
        self.badge_number = QLineEdit(record.get("badge_number", ""))
        self.valid_until = QLineEdit(record.get("valid_until", ""))
        self.issue_date = QDateEdit()
        self.issue_date.setCalendarPopup(True)
        self.issue_date.setDisplayFormat("dd.MM.yyyy")
        raw_issue = record.get("issue_date") or datetime.now().strftime("%d.%m.%Y")
        parsed_issue = QDate.fromString(raw_issue, "dd.MM.yyyy")
        self.issue_date.setDate(parsed_issue if parsed_issue.isValid() else QDate.currentDate())
        self.photo_path = QLineEdit(record.get("photo_path", ""))
        photo_row = QHBoxLayout(); photo_row.addWidget(self.photo_path)
        browse = QPushButton("Выбрать…")
        browse.clicked.connect(self.pick_photo)
        photo_row.addWidget(browse)
        photo_wrap = QWidget(); photo_wrap.setLayout(photo_row)
        form.addRow("ФИО *", self.full_name)
        form.addRow("Организация", self.organization)
        form.addRow("Подразделение", self.department)
        form.addRow("Должность", self.position)
        form.addRow("Номер пропуска", self.badge_number)
        form.addRow("Дата выдачи пропуска", self.issue_date)
        form.addRow("Действует до (ДД.ММ.ГГГГ)", self.valid_until)
        form.addRow("Исходная фотография", photo_wrap)
        buttons = QDialogButtonBox(QDialogButtonBox.Save | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept); buttons.rejected.connect(self.reject)
        form.addRow(buttons)

    def pick_photo(self):
        path, _ = QFileDialog.getOpenFileName(self, "Фотография сотрудника", "", "Изображения (*.jpg *.jpeg *.png *.bmp *.webp *.tif)")
        if path: self.photo_path.setText(path)

    def values(self):
        return {"full_name": self.full_name.text().strip(), "organization": self.organization.currentText().strip(),
                "department": self.department.text().strip(), "position": self.position.text().strip(),
                "badge_number": self.badge_number.text().strip(), "valid_until": self.valid_until.text().strip(),
                "issue_date": self.issue_date.date().toString("dd.MM.yyyy"),
                "photo_path": self.photo_path.text().strip()}

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(APP_NAME)
        icon_path = RESOURCES_DIR / "PS_PassMaster.ico"
        if icon_path.exists():
            self.setWindowIcon(QIcon(str(icon_path)))
        screen = QApplication.primaryScreen()
        if screen:
            area = screen.availableGeometry()
            min_w, min_h, target_w, target_h = startup_window_metrics(area.width(), area.height())
            self.setMinimumSize(min_w, min_h)
            self.resize(target_w, target_h)
            self.move(area.x() + max(0, (area.width() - target_w) // 2),
                      area.y() + max(0, (area.height() - target_h) // 2))
        else:
            self.setMinimumSize(760, 520)
            self.resize(1120, 740)
        self.files = []
        self.prepared = {}
        self.photo_edits = load_json(PHOTO_EDITS_FILE, {})
        self.settings = load_json(SETTINGS_FILE, {})
        self.pin_guard = PinAttemptGuard(self.settings.get("pin_guard", {}))
        self.current_operator = None
        self.active_session_id = None
        with db_connect() as con:
            recover_interrupted_sessions(con)
            has_named = con.execute("SELECT 1 FROM operator_accounts WHERE is_enabled=1 LIMIT 1").fetchone()
            has_legacy = con.execute("SELECT 1 FROM app_roles LIMIT 1").fetchone()
            if not has_named and not has_legacy:
                self.active_session_id = start_session(con, {"username": "Локальный оператор", "display_name": "Локальный оператор", "role": "legacy"})
                self.active_session_username = "Локальный оператор"
        self.idle_lock_timer = QTimer(self); self.idle_lock_timer.setSingleShot(True)
        self.idle_lock_timer.timeout.connect(self.lock_after_inactivity)
        QApplication.instance().installEventFilter(self)
        self.history = load_json(HISTORY_FILE, [])
        self.build_ui()
        self.apply_settings()
        self.auto_import_hub_template(silent=True)
        self.refresh_history()
        try:
            expired = expire_active_cards(DB_FILE, datetime.now().date())
            if expired:
                logging.info("Automatically expired %d cards", len(expired))
            create_daily_backup_if_needed()
            if self.settings.get("full_backup_auto", True):
                create_daily_full_backup_if_needed()
        except Exception:
            logging.exception("Daily backup failed")
        self.statusBar().showMessage("Готово к работе")
        developer_label = QLabel(f"Разработчик: {DEVELOPER_NAME}")
        developer_label.setObjectName("muted")
        self.statusBar().addPermanentWidget(developer_label)
        self.reset_idle_lock_timer()
        QTimer.singleShot(1200, self.show_expiry_notification_if_due)
        if not self.settings.get("first_run_wizard_seen", False):
            QTimer.singleShot(2500, self.open_first_run_wizard)

    def build_ui(self):
        """Compact, responsive workflow dedicated to Magicard HUB badge production."""
        self.setStyleSheet("""
        QMainWindow, QWidget { background:#20252B; color:#F1F3F5; font-family:"Segoe UI","Arial",sans-serif; font-size:9pt; }
        QLabel { color:#F1F3F5; }
        QLabel#muted { color:#AEB6BE; }
        QPushButton { background:#3A424B; color:#F7F7F7; border:1px solid #515B66; border-radius:7px; padding:6px 11px; min-height:29px; font-weight:600; }
        QPushButton:hover { background:#4A535D; border-color:#F2C230; }
        QPushButton:pressed { background:#252A30; border-color:#D9A91E; }
        QPushButton:disabled { background:#30363D; color:#818A93; border-color:#3B434B; }
        QPushButton#primary, QPushButton:default { background:#F2C230; color:#252A30; border:1px solid #F2C230; }
        QPushButton#primary:hover, QPushButton:default:hover { background:#FFD54A; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QListWidget, QPlainTextEdit, QDateEdit { background:#2B3138; border:1px solid #4A535D; border-radius:6px; padding:5px 7px; color:#F1F3F5; selection-background-color:#B88D13; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QListWidget:focus, QPlainTextEdit:focus, QDateEdit:focus { border:1px solid #F2C230; }
        QComboBox QAbstractItemView { background:#2B3138; color:#F1F3F5; selection-background-color:#5A4B1D; }
        QTabWidget::pane { border:1px solid #454E58; border-radius:8px; background:#252B31; }
        QTabBar::tab { background:#30363D; color:#C9D0D6; padding:8px 13px; margin-right:3px; min-height:27px; border:1px solid #454E58; border-bottom:0; border-top-left-radius:7px; border-top-right-radius:7px; }
        QTabBar::tab:selected { background:#F2C230; color:#252A30; font-weight:700; }
        QGroupBox { border:1px solid #454E58; border-radius:8px; margin-top:12px; padding:10px; font-weight:600; background:#272D33; }
        QGroupBox::title { subcontrol-origin:margin; left:12px; padding:0 6px; color:#F2C230; }
        QProgressBar { border:1px solid #4A535D; border-radius:6px; text-align:center; background:#2B3138; min-height:14px; }
        QProgressBar::chunk { background:#F2C230; border-radius:5px; }
        QTableWidget { background:#2B3138; alternate-background-color:#30373F; gridline-color:#454E58; border:1px solid #454E58; border-radius:6px; }
        QHeaderView::section { background:#3A424B; color:#F2C230; border:0; padding:6px; font-weight:700; }
        QTableWidget::item:selected { background:#5A4B1D; color:white; }
        QMenuBar, QStatusBar { background:#252A30; color:#BFC6CC; }
        QMenu, QMessageBox { background:#2B3138; color:#F1F3F5; border:1px solid #4A535D; }
        QMenu::item { padding:7px 24px; } QMenu::item:selected { background:#5A4B1D; }
        QScrollBar:vertical { background:#252A30; width:11px; } QScrollBar::handle:vertical { background:#59636D; min-height:25px; border-radius:5px; }
        QCheckBox::indicator { width:16px; height:16px; }
        QCheckBox::indicator:checked { background:#F2C230; border:1px solid #D9A91E; border-radius:3px; }
        QCheckBox::indicator:unchecked { background:#2B3138; border:1px solid #69737D; border-radius:3px; }
        """)

        tabs = QTabWidget()
        tabs.setDocumentMode(True)
        tabs.setMovable(False)
        self.main_tabs = tabs
        self.setCentralWidget(tabs)

        # Keep the legacy photo pipeline internally, but lock it to the Magicard HUB profile.
        self.width_cm = QDoubleSpinBox(); self.width_cm.setRange(0.5, 30); self.width_cm.setValue(VNUKOVO_PHOTO_WIDTH_CM)
        self.height_cm = QDoubleSpinBox(); self.height_cm.setRange(0.5, 30); self.height_cm.setValue(VNUKOVO_PHOTO_HEIGHT_CM)
        self.dpi = QSpinBox(); self.dpi.setRange(300, 300); self.dpi.setValue(300)
        self.mode = QComboBox(); self.mode.addItems(["Заполнить (обрезать)"])
        self.format = QComboBox(); self.format.addItems(["JPG"])
        self.quality = QSpinBox(); self.quality.setRange(95, 95); self.quality.setValue(95)
        self.name_prefix = QLineEdit("Magicard")
        self.auto_face = QCheckBox(); self.auto_face.setChecked(True)

        # 1. Employee register — the primary screen and the normal operator workflow.
        emp = QWidget(); el = QVBoxLayout(emp); el.setContentsMargins(10, 10, 10, 10); el.setSpacing(8)
        header = QHBoxLayout()
        title = QLabel("Реестр сотрудников и пропусков")
        title.setStyleSheet("font-size:15pt;font-weight:700;color:#F2C230;")
        header.addWidget(title); header.addStretch()
        self.number_status = QLabel("Предыдущая нумерация: —    Следующий номер: —")
        self.number_status.setObjectName("muted")
        header.addWidget(self.number_status)
        el.addLayout(header)
        self.employee_search = QLineEdit(); self.employee_search.setPlaceholderText("Поиск по ФИО, организации, должности или номеру пропуска…")
        self.employee_search.textChanged.connect(self.filter_employees)
        el.addWidget(self.employee_search)
        self.employee_table = QTableWidget(0, 7)
        self.employee_table.setHorizontalHeaderLabels(["ID", "ФИО", "Организация", "Должность", "№ пропуска", "Дата выдачи", "Действует до"])
        self.employee_table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.employee_table.setSelectionMode(QAbstractItemView.ExtendedSelection)
        self.employee_table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.employee_table.setAlternatingRowColors(True)
        self.employee_table.verticalHeader().setVisible(False)
        self.employee_table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.employee_table.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        self.employee_table.horizontalHeader().setSectionResizeMode(2, QHeaderView.Stretch)
        self.employee_table.horizontalHeader().setSectionResizeMode(3, QHeaderView.Stretch)
        for col in (4, 5, 6): self.employee_table.horizontalHeader().setSectionResizeMode(col, QHeaderView.ResizeToContents)
        el.addWidget(self.employee_table, 1)
        erow = QGridLayout(); erow.setHorizontalSpacing(7); erow.setVerticalSpacing(6)
        primary_actions = [
            ("＋ Новый сотрудник", self.add_employee, True, 0, 0),
            ("Изменить", self.edit_employee, False, 0, 1),
            ("В корзину", self.delete_employee, False, 0, 2),
            ("Подготовить фото", self.prepare_employee_photo, False, 1, 0),
            ("Выпустить карточку HUB (.card)", self.export_employee_to_hub_template, True, 1, 1),
        ]
        for label, slot, primary, row_idx, col_idx in primary_actions:
            b = QPushButton(label); b.clicked.connect(slot); b.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
            if primary: b.setObjectName("primary")
            erow.addWidget(b, row_idx, col_idx)
        more_employee_btn = QPushButton("Ещё ▾")
        more_employee_menu = QMenu(more_employee_btn)
        more_employee_menu.addAction("Автопоиск фотографий для выбранных…", self.match_selected_employee_photos)
        more_employee_menu.addAction("Пакетное создание .card (до 500)", self.export_selected_employees_to_hub)
        more_employee_menu.addAction("Проверить фото выбранных сотрудников", self.check_selected_employee_photos)
        more_employee_menu.addAction("Отчёт качества всех фото (CSV)…", self.export_photo_quality_report)
        more_employee_menu.addAction("Полная проверка карточки перед выпуском…", self.open_card_preflight)
        more_employee_menu.addAction("Корзина сотрудников…", self.show_employee_trash)
        more_employee_menu.addAction("Отчёт по срокам пропусков (CSV)…", self.export_expiry_report)
        more_employee_menu.addAction("Отчёт по срокам пропусков (Excel)…", self.export_expiry_report_excel)
        more_employee_menu.addAction("Продлить срок выбранным сотрудникам…", self.bulk_update_expiry)
        more_employee_menu.addAction("Журнал блокировок и статусов…", self.show_card_status_history)
        more_employee_menu.addAction("Тип и статус пропуска…", self.manage_card_status)
        more_employee_menu.addAction("Перевыпустить пропуск…", self.reissue_employee_card)
        more_employee_menu.addAction("Учёт выдачи / возврата пропуска…", self.record_card_movement)
        more_employee_menu.addAction("Журнал выдачи и возврата…", self.show_card_movement_history)
        more_employee_menu.addAction("Экспорт журнала пропусков (CSV)…", self.export_card_movement_history)
        more_employee_menu.addAction("История перевыпусков…", self.show_reissue_history)
        more_employee_menu.addAction("Настроить роли и PIN…", self.configure_roles)
        more_employee_menu.addAction("Учётные записи операторов…", self.manage_operator_accounts)
        more_employee_menu.addAction("Сессии операторов и отчёт по сменам…", self.open_operator_sessions)
        more_employee_menu.addAction("Центр безопасности…", self.open_security_center)
        more_employee_menu.addAction("Единый центр управления доступом…", self.open_access_control_center)
        more_employee_menu.addAction("Центр отчётов и уведомлений…", self.open_reports_center)
        more_employee_menu.addAction("Краткая панель статистики…", self.open_analytics_dashboard)
        more_employee_menu.addSeparator()
        more_employee_menu.addAction("Импорт сотрудников из Excel…", self.import_employees_excel)
        more_employee_menu.addAction("Импорт сотрудников из CSV…", self.import_employees_csv)
        more_employee_menu.addAction("Экспорт сотрудников в Excel…", self.export_employees_excel)
        more_employee_menu.addAction("Экспорт сотрудников в CSV…", self.export_employees_csv)
        more_employee_btn.setMenu(more_employee_menu); erow.addWidget(more_employee_btn, 1, 2)
        for col in range(3): erow.setColumnStretch(col, 1)
        el.addLayout(erow)
        tabs.addTab(emp, QIcon(str(RESOURCES_DIR / "employees.png")), "Сотрудники")
        self.refresh_employees()

        # 2. Dedicated portrait preparation; generic passport/photo sizes are removed from UI.
        prep = QWidget(); root = QVBoxLayout(prep); root.setContentsMargins(10, 10, 10, 10); root.setSpacing(8)
        prep_header = QLabel("Подготовка портрета для карточки Magicard HUB")
        prep_header.setStyleSheet("font-size:14pt;font-weight:700;color:#F2C230;")
        root.addWidget(prep_header)
        profile_note = QLabel(f"Профиль: JPG · 300 DPI · {VNUKOVO_PHOTO_WIDTH_MM:.1f} × {VNUKOVO_PHOTO_HEIGHT_MM:.1f} мм. Размер окна портрета предварительный — он основан на предоставленном снимке экрана HUB.")
        profile_note.setWordWrap(True); profile_note.setObjectName("muted"); root.addWidget(profile_note)
        top = QHBoxLayout()
        self.add_btn = QPushButton("＋ Добавить фотографии"); self.add_btn.clicked.connect(self.choose_files)
        self.remove_btn = QPushButton("Убрать выбранные"); self.remove_btn.clicked.connect(self.remove_selected)
        self.clear_btn = QPushButton("Очистить список"); self.clear_btn.clicked.connect(self.clear_files)
        top.addWidget(self.add_btn); top.addWidget(self.remove_btn); top.addWidget(self.clear_btn); top.addStretch()
        root.addLayout(top)
        splitter = QSplitter(Qt.Horizontal)
        left = QWidget(); ll = QVBoxLayout(left); ll.setContentsMargins(0, 0, 0, 0)
        ll.addWidget(QLabel("Список фотографий — файлы можно перетащить сюда"))
        self.file_list = DropList(); self.file_list.currentItemChanged.connect(self.preview_selected); ll.addWidget(self.file_list, 1)
        self.count_label = QLabel("Файлов: 0"); self.count_label.setObjectName("muted"); ll.addWidget(self.count_label)
        right = QWidget(); rl = QVBoxLayout(right); rl.setContentsMargins(0, 0, 0, 0)
        self.preview = CropPreview(); self.preview.setText("Выберите фотографию")
        self.preview.on_change = self.preview_selected
        self.preview.setMinimumSize(180, 180)
        self.preview.setStyleSheet("background:#10151d;border:1px dashed #526078;border-radius:10px;")
        rl.addWidget(self.preview, 1)
        self.info = QLabel("Перемещайте фото мышью, масштабируйте колёсиком. Результат будет сохранён в формате JPG для HUB.")
        self.info.setObjectName("muted"); self.info.setWordWrap(True); rl.addWidget(self.info)
        crop_actions = QHBoxLayout()
        reset_crop_btn = QPushButton("Сбросить кадрирование"); reset_crop_btn.clicked.connect(self.preview.reset_crop)
        self.face_btn = QPushButton("Автоподгонка лица"); self.face_btn.clicked.connect(self.detect_selected_face)
        self.editor_btn = QPushButton("Редактор фото"); self.editor_btn.clicked.connect(self.open_photo_editor)
        self.auto_face.setText("Автоподгонка лица при подготовке")
        self.auto_face.setToolTip("Для каждой фотографии отдельно ищет лицо и рассчитывает начальное кадрирование. Можно отключить и настроить вручную.")
        crop_actions.addWidget(reset_crop_btn); crop_actions.addWidget(self.face_btn); crop_actions.addWidget(self.editor_btn); rl.addLayout(crop_actions)
        rl.addWidget(self.auto_face)
        splitter.addWidget(left); splitter.addWidget(right); splitter.setStretchFactor(0, 1); splitter.setStretchFactor(1, 2)
        root.addWidget(splitter, 1)
        actions = QHBoxLayout()
        self.prepare_btn = QPushButton("Подготовить JPG для HUB"); self.prepare_btn.setObjectName("primary")
        self.prepare_btn.clicked.connect(self.prepare_all)
        self.export_btn = QPushButton("Открыть папку готовых фото"); self.export_btn.clicked.connect(self.open_output)
        actions.addWidget(self.prepare_btn); actions.addWidget(self.export_btn); actions.addStretch(); root.addLayout(actions)
        self.progress = QProgressBar(); self.progress.setValue(0); root.addWidget(self.progress)
        tabs.addTab(prep, QIcon(str(RESOURCES_DIR / "photo.png")), "Фото для HUB")

        # 3. HUB integration settings kept deliberately small and explicit.
        magic = QWidget(); ml = QVBoxLayout(magic); ml.setContentsMargins(12, 12, 12, 12); ml.setSpacing(10)
        hub_title = QLabel("Magicard HUB · профиль оборудования")
        hub_title.setStyleSheet("font-size:15pt;font-weight:700;color:#F2C230;"); ml.addWidget(hub_title)
        intro = QLabel("Рабочий профиль: Magicard 300 + Magicard HUB 2.0.6 · формат карты CR80 (85,60 × 53,98 мм). Программа подготавливает фотографии и .card-копии шаблона; физическая печать запускается штатными средствами Magicard HUB.")
        intro.setWordWrap(True); ml.addWidget(intro)
        mbox = QGroupBox("Подключение Magicard HUB")
        mf = QFormLayout(mbox)
        self.hub_path = QLineEdit(self.settings.get("hub_path", "")); self.hub_path.setPlaceholderText("Путь к Magicard HUB.exe")
        hubrow = QHBoxLayout(); hubrow.addWidget(self.hub_path, 1)
        browse = QPushButton("Обзор…"); browse.clicked.connect(self.choose_hub); hubrow.addWidget(browse)
        hubwrap = QWidget(); hubwrap.setLayout(hubrow)
        mf.addRow("Программа HUB", hubwrap)
        self.magicard_file = QLineEdit(); self.magicard_file.setReadOnly(True)
        self.magicard_file.setPlaceholderText("Здесь появится последнее подготовленное фото")
        mf.addRow("Последнее фото", self.magicard_file)
        self.template_path_label = QLabel(self.settings.get("hub_card_template", str(ROOT / "Templates" / "New Card Design.card")))
        self.template_path_label.setWordWrap(True); self.template_path_label.setObjectName("muted")
        mf.addRow("Шаблон карточки", self.template_path_label)
        hub_actions = QHBoxLayout()
        open_hub_btn = QPushButton("Открыть Magicard HUB"); open_hub_btn.setObjectName("primary"); open_hub_btn.clicked.connect(self.open_hub)
        template_btn = QPushButton("Выбрать шаблон .card"); template_btn.clicked.connect(self.select_hub_template_ui)
        refresh_template_btn = QPushButton("Обновить шаблон HUB"); refresh_template_btn.clicked.connect(self.refresh_hub_template)
        check_template_btn = QPushButton("Проверить шаблон"); check_template_btn.clicked.connect(self.check_hub_template)
        output_btn = QPushButton("Папка результатов"); output_btn.clicked.connect(self.open_output)
        open_last_btn = QPushButton("Открыть последнюю .card"); open_last_btn.clicked.connect(self.open_last_card)
        preview_last_btn = QPushButton("Предпросмотр .card"); preview_last_btn.clicked.connect(self.preview_last_card)
        hub_actions.addWidget(open_hub_btn); hub_actions.addWidget(open_last_btn); hub_actions.addWidget(preview_last_btn)
        hub_actions.addWidget(template_btn); hub_actions.addWidget(refresh_template_btn); hub_actions.addWidget(check_template_btn); hub_actions.addWidget(output_btn)
        mf.addRow(hub_actions); ml.addWidget(mbox)
        workflow = QGroupBox("Порядок работы")
        wf = QVBoxLayout(workflow)
        wf.addWidget(QLabel("1. Добавьте сотрудника и выберите исходную фотографию.\n2. Подготовьте портрет в профиле HUB.\n3. Создайте копию .card из выбранного шаблона.\n4. Откройте файл в Magicard HUB, проверьте макет и выполните печать штатной командой."))
        ml.addWidget(workflow); ml.addStretch(1)
        tabs.addTab(magic, QIcon(str(RESOURCES_DIR / "magicard.png")), "Magicard HUB")

        # 4. Short operational history; destructive/maintenance actions moved to one menu.
        hist = QWidget(); hl = QVBoxLayout(hist); hl.setContentsMargins(10, 10, 10, 10); hl.setSpacing(7)
        hl.addWidget(QLabel("История подготовки фотографий и создания карточек HUB"))
        self.history_list = QListWidget(); hl.addWidget(self.history_list, 1)
        self.issuance_list = QListWidget(); self.issuance_list.setMaximumHeight(120)
        hl.addWidget(QLabel("Последние созданные .card-файлы")); hl.addWidget(self.issuance_list)
        self.audit_list = QListWidget(); self.audit_list.setMaximumHeight(130)
        hl.addWidget(QLabel("Журнал действий")); hl.addWidget(self.audit_list)
        hrow = QHBoxLayout()
        refresh = QPushButton("Обновить"); refresh.clicked.connect(self.refresh_history)
        open_hist = QPushButton("Папка результатов"); open_hist.clicked.connect(self.open_output)
        history_more = QPushButton("Обслуживание ▾")
        history_menu = QMenu(history_more)
        history_menu.addAction("Создать резервную копию базы", self.manual_backup)
        history_menu.addAction("Восстановить базу из копии…", self.restore_database_backup)
        history_menu.addAction("Очистить историю файлов…", self.clear_history)
        history_menu.addSeparator()
        history_menu.addAction("Создать диагностический пакет поддержки…", self.create_support_bundle_ui)
        history_more.setMenu(history_menu)
        hrow.addWidget(refresh); hrow.addWidget(open_hist); hrow.addWidget(history_more); hrow.addStretch(); hl.addLayout(hrow)
        tabs.addTab(hist, QIcon(str(RESOURCES_DIR / "history.png")), "История")

        # Consistent icons and compact control metrics; no oversized toolbar button rows.
        for button in self.findChildren(QPushButton):
            button.setMinimumHeight(32)
            button.setSizePolicy(QSizePolicy.Minimum, QSizePolicy.Fixed)
            if not button.toolTip(): button.setToolTip(button.text())
        tabs.tabBar().setIconSize(QSize(18, 18))
        tabs.tabBar().setUsesScrollButtons(True)
        for label in self.findChildren(QLabel): label.setWordWrap(True)

        # 0. Professional home dashboard: concise entry points and live status.
        dashboard = QWidget(); dl = QVBoxLayout(dashboard); dl.setContentsMargins(18, 18, 18, 18); dl.setSpacing(12)
        hero = QLabel("PS PassMaster PRO 8.9.0")
        hero.setStyleSheet("font-size:22pt;font-weight:800;color:#F2C230;")
        dl.addWidget(hero)
        subtitle = QLabel("Профессиональная рабочая среда для выпуска пропусков · Magicard HUB · разработчик P.Sveshnikov")
        subtitle.setObjectName("muted"); subtitle.setWordWrap(True); dl.addWidget(subtitle)
        self.dashboard_summary = QLabel("Загрузка сводки…")
        self.dashboard_summary.setStyleSheet("font-size:12pt;font-weight:650;padding:14px;background:#2B3138;border:1px solid #46505A;border-radius:9px;")
        dl.addWidget(self.dashboard_summary)
        quick = QGroupBox("Быстрые действия"); ql = QGridLayout(quick)
        quick_actions = [("＋ Новый сотрудник", self.add_employee), ("Подготовить фото", self.prepare_employee_photo),
                         ("Выпустить .card для HUB", self.export_employee_to_hub_template),
                         ("Центр отчётов", self.open_reports_center), ("Резервное копирование", self.open_backup_center),
                         ("Проверить готовность", self.run_system_check)]
        for i, (caption, callback) in enumerate(quick_actions):
            button = QPushButton(caption); button.clicked.connect(callback)
            if i in (0, 2): button.setObjectName("primary")
            ql.addWidget(button, i // 3, i % 3)
        for col in range(3): ql.setColumnStretch(col, 1)
        dl.addWidget(quick)
        self.dashboard_health = QLabel("Проверка готовности: не запускалась")
        self.dashboard_health.setWordWrap(True); self.dashboard_health.setObjectName("muted"); dl.addWidget(self.dashboard_health)
        dl.addStretch(1)
        self.main_tabs.insertTab(0, dashboard, QIcon(str(RESOURCES_DIR / "about.png")), "Главная")
        self.refresh_dashboard()

        settings_menu = self.menuBar().addMenu("Настройки")
        settings_menu.addAction("Мастер первого запуска…", self.open_first_run_wizard)
        settings_menu.addAction("Сетевой режим и подключение…", self.open_network_settings)
        settings_menu.addAction("Проверка готовности программы…", self.run_system_check)
        settings_menu.addAction("Проверка целостности данных сотрудников…", self.run_employee_data_integrity)
        settings_menu.addAction("Расширенная диагностика базы данных…", self.run_database_health_check)
        settings_menu.addAction("История изменений выбранного сотрудника…", self.show_employee_change_history)
        settings_menu.addAction("Организации и профили…", self.manage_organizations)
        settings_menu.addSeparator()
        settings_menu.addAction("Шаблоны по организациям и типам…", self.manage_template_profiles)
        settings_menu.addAction("Визуальный редактор макета карточки…", self.open_card_designer)
        settings_menu.addAction("Предпросмотр карточки сотрудника…", self.open_card_preview)
        settings_menu.addAction("Проверка карточки перед выпуском…", self.open_card_preflight)
        settings_menu.addAction("Выбрать / импортировать общий шаблон HUB…", self.select_hub_template_ui)
        settings_menu.addAction("Повторно найти шаблон HUB", self.refresh_hub_template)
        settings_menu.addSeparator()
        settings_menu.addAction("Открыть папку данных программы", lambda: self.open_data_folder())
        settings_menu.addAction("Центр резервного копирования и восстановления…", self.open_backup_center)
        settings_menu.addAction("Открыть резервные копии", lambda: self.open_folder(BACKUP_DIR))
        settings_menu.addAction("Открыть журнал программы", lambda: self.open_folder(LOG_DIR))
        settings_menu.addSeparator()
        settings_menu.addAction("Журнал действий: поиск и фильтры…", self.open_audit_search)
        settings_menu.addAction("Настроить автоблокировку…", self.configure_idle_lock)

        program_menu = self.menuBar().addMenu("Программа")
        update_action = program_menu.addAction(QIcon(str(RESOURCES_DIR / "update.png")), "Проверить обновления…")
        update_action.triggered.connect(self.open_update_dialog)
        program_menu.addSeparator()
        about_action = program_menu.addAction(QIcon(str(RESOURCES_DIR / "about.png")), "О программе")
        about_action.triggered.connect(self.show_about)

    def open_network_settings(self):
        """Configure LAN deployment profile; live record synchronization is a later stage."""
        dlg = QDialog(self)
        dlg.setWindowTitle("Сетевой режим PS PassMaster PRO")
        dlg.resize(620, 430)
        layout = QVBoxLayout(dlg)
        intro = QLabel(
            "Настройка защищённого обмена карточками в локальной сети. "
            "Изменения сотрудников автоматически попадают в локальную очередь; "
            "обмен запускается вручную кнопкой ниже. Фотографии и шаблоны пока передаются отдельно. "
            "Не размещайте employees.sqlite3 в общей сетевой папке."
        )
        intro.setWordWrap(True)
        layout.addWidget(intro)
        form = QFormLayout()
        role = QComboBox(); role.addItem("Автономный компьютер", "standalone")
        role.addItem("Главный компьютер (планируемый сервер)", "server")
        role.addItem("Рабочая станция (клиент)", "client")
        host = QLineEdit(); host.setPlaceholderText("Например, 192.168.1.10")
        port = QLineEdit("8765"); port.setMaxLength(5)
        tls = QCheckBox("Требовать защищённое TLS-соединение"); tls.setChecked(True); tls.setEnabled(False)
        workspace = QLineEdit("default")
        form.addRow("Роль компьютера:", role); form.addRow("Адрес главного ПК:", host)
        form.addRow("Порт:", port); form.addRow("Защита:", tls); form.addRow("Рабочая группа:", workspace)
        layout.addLayout(form)
        status = QLabel("Профиль не проверен."); status.setWordWrap(True)
        layout.addWidget(status)
        try:
            profile = load_network_profile(NETWORK_PROFILE_FILE)
            idx = role.findData(profile.role)
            if idx >= 0: role.setCurrentIndex(idx)
            host.setText(profile.server_host); port.setText(str(profile.server_port))
            tls.setChecked(profile.use_tls); workspace.setText(profile.workspace_id)
        except Exception as exc:
            status.setText(f"Не удалось прочитать сетевой профиль: {exc}")

        def check_connection():
            try:
                p = NetworkProfile(role.currentData(), host.text().strip(), int(port.text()), tls.isChecked(), workspace.text().strip()).validate()
                if p.role != "client":
                    status.setText("Параметры корректны. Для проверки подключения выберите роль «Рабочая станция». "
                                   "Сетевой сервер в этой версии не запускается автоматически.")
                    return
                ok, message = probe_server(p.server_host, p.server_port)
                status.setText(message)
            except Exception as exc:
                status.setText(f"Ошибка параметров: {exc}")

        def save_settings():
            try:
                p = NetworkProfile(role.currentData(), host.text().strip(), int(port.text()), tls.isChecked(), workspace.text().strip()).validate()
                save_network_profile(p, NETWORK_PROFILE_FILE)
                QMessageBox.information(dlg, "Сетевой профиль", "Профиль сохранён. На главном ПК запустите run_lan_server.py. На рабочей станции используйте кнопку «Синхронизировать карточки». Токен вводится только на время операции и не сохраняется.")
                dlg.accept()
            except Exception as exc:
                QMessageBox.warning(dlg, "Не удалось сохранить", str(exc))

        bootstrap = QCheckBox("Первичная отправка всех существующих карточек (только при первом подключении)")
        layout.addWidget(bootstrap)

        def sync_now():
            try:
                p = NetworkProfile(role.currentData(), host.text().strip(), int(port.text()), tls.isChecked(), workspace.text().strip()).validate()
                if p.role != "client":
                    QMessageBox.information(dlg, "Синхронизация", "Для ручного обмена выберите роль «Рабочая станция». Серверную базу можно первично отправить через run_employee_sync.py.")
                    return
                token, ok = QInputDialog.getText(dlg, "Токен синхронизации", "Введите токен сервера (не сохраняется в настройках):", QLineEdit.Password)
                if not ok: return
                if len(token) < 24: raise ValueError("Токен должен содержать не менее 24 символов.")
                ca_path, _ = QFileDialog.getOpenFileName(dlg, "Выберите доверенный TLS-сертификат", str(ROOT), "Сертификаты (*.pem *.crt *.cer);;Все файлы (*.*)")
                if not ca_path: return
                if bootstrap.isChecked():
                    with db_connect() as con: queued = queue_existing_employees(con)
                else: queued = 0
                result = sync_employee_records(DB_FILE, f"https://{p.server_host}:{p.server_port}", token, ca_path)
                message = (f"Отправлено изменений: {result['pushed']}\nПолучено изменений: {result['pulled']}\n"
                           f"Ожидает отправки: {result['pending']}\nКонфликтов: {len(result['conflicts'])}\n"
                           f"Первично поставлено в очередь: {queued}")
                if result['conflicts']:
                    message += "\n\nКонфликтующие карточки не перезаписаны. Обратитесь к администратору для сверки."
                    QMessageBox.warning(dlg, "Синхронизация завершена с конфликтами", message)
                else:
                    QMessageBox.information(dlg, "Синхронизация завершена", message)
            except Exception as exc:
                QMessageBox.warning(dlg, "Ошибка синхронизации", str(exc))

        actions = QHBoxLayout()
        check_btn = QPushButton("Проверить подключение"); check_btn.clicked.connect(check_connection)
        sync_btn = QPushButton("Синхронизировать карточки"); sync_btn.clicked.connect(sync_now)
        save_btn = QPushButton("Сохранить профиль"); save_btn.setObjectName("primary"); save_btn.clicked.connect(save_settings)
        close_btn = QPushButton("Закрыть"); close_btn.clicked.connect(dlg.reject)
        actions.addWidget(check_btn); actions.addWidget(sync_btn); actions.addStretch(1); actions.addWidget(save_btn); actions.addWidget(close_btn)
        layout.addLayout(actions)
        dlg.exec()

    def run_database_health_check(self):
        """Run read-only SQLite and employee-data diagnostics."""
        if not self.require_role("view"):
            return
        try:
            with db_connect() as con:
                records = [dict(row) for row in con.execute(
                    "SELECT id,full_name,badge_number,valid_until,card_status,photo_path,is_deleted FROM employees"
                ).fetchall()]
            report = inspect_database_health(DB_FILE, records)
            message = format_database_health_report(report)
            if report.get("ok"):
                QMessageBox.information(self, "Диагностика базы данных", message)
            else:
                QMessageBox.warning(self, "Диагностика базы данных", message)
            with db_connect() as con:
                write_audit(con, "database_health_check", details=(
                    f"Проверено: {report.get('employees_checked', 0)}; замечаний: {len(report.get('findings', []))}; "
                    f"SQLite: {report.get('sqlite_integrity', 'не проверено')}"
                ))
        except Exception as exc:
            logging.exception("Database health check failed")
            QMessageBox.critical(self, "Диагностика базы данных", f"Не удалось выполнить проверку:\n{exc}")

    def open_card_designer(self):
        """Open the conservative visual editor for a HUB template copy."""
        start = self.settings.get("hub_card_template", str(ROOT / "Templates" / "New Card Design.card"))
        path, _ = QFileDialog.getOpenFileName(self, "Выберите шаблон Magicard для редактирования", str(Path(start).parent), "Шаблоны Magicard (*.card)")
        if not path:
            return
        try:
            dialog = CardDesignerDialog(path, self)
            if dialog.exec() == QDialog.Accepted:
                QMessageBox.information(self, APP_NAME, "Новый шаблон сохранён. При необходимости назначьте его организации и типу пропуска в разделе «Шаблоны по организациям и типам». ")
        except Exception as exc:
            logging.exception("Card designer failed")
            QMessageBox.critical(self, APP_NAME, f"Не удалось открыть редактор макета:\n{exc}")

    def open_card_preflight(self):
        """Run a read-only preflight on the selected employee and assigned HUB template."""
        rec = self.get_employee()
        if not rec:
            return
        template = get_profile_template(self.settings.get("hub_template_profiles", {}), rec.get("organization", ""), rec.get("card_type", ""))
        if not template:
            template = self.settings.get("hub_card_template", str(ROOT / "Templates" / "New Card Design.card"))
        if not Path(template).is_file():
            template = self.choose_hub_card_template()
        if not template:
            return
        report = preflight_employee_card(rec, template)
        message = format_preflight(report)
        if report["errors"]:
            QMessageBox.warning(self, "Проверка карточки — выпуск запрещён", message)
        elif report["warnings"]:
            QMessageBox.information(self, "Проверка карточки — нужны проверки", message)
        else:
            QMessageBox.information(self, "Проверка карточки пройдена", message)

    def open_card_preview(self):
        """Show a personalized proof for the currently selected employee."""
        rec = self.get_employee()
        if not rec:
            return
        template = get_profile_template(self.settings.get("hub_template_profiles", {}), rec.get("organization", ""), rec.get("card_type", ""))
        if not template:
            template = self.settings.get("hub_card_template", str(ROOT / "Templates" / "New Card Design.card"))
        if not Path(template).is_file():
            template = self.choose_hub_card_template()
        if not template:
            return
        try:
            design = load_card(template)
            personalize_design(design, rec)
            photo = rec.get("photo_path") or None
            dialog = CardPreviewDialog(design, rec, photo, self)
            dialog.exec()
        except Exception as exc:
            logging.exception("Card preview failed")
            QMessageBox.critical(self, APP_NAME, f"Не удалось подготовить предпросмотр:\n{exc}")

    def organization_names(self):
        with db_connect() as con:
            return [r[0] for r in con.execute("SELECT name FROM organization_catalog ORDER BY name COLLATE NOCASE").fetchall()]

    def manage_template_profiles(self):
        dialog = QDialog(self)
        dialog.setWindowTitle("Профили шаблонов Magicard")
        dialog.setMinimumWidth(680)
        layout = QVBoxLayout(dialog)
        layout.addWidget(QLabel("Назначьте отдельный проверенный .card-шаблон для каждой организации и типа пропуска. Исходные файлы не изменяются."))
        table = QTableWidget(0, 3)
        table.setHorizontalHeaderLabels(["Организация", "Тип пропуска", "Файл шаблона"])
        table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeToContents)
        table.horizontalHeader().setSectionResizeMode(2, QHeaderView.Stretch)
        table.setSelectionBehavior(QAbstractItemView.SelectRows)
        layout.addWidget(table, 1)
        row = QHBoxLayout()
        org = QComboBox(); org.setEditable(True); org.addItems(self.organization_names())
        org.setMinimumWidth(180)
        kind = QComboBox(); kind.addItems(CARD_TYPES)
        choose = QPushButton("Выбрать .card и назначить")
        row.addWidget(QLabel("Организация:")); row.addWidget(org, 1)
        row.addWidget(QLabel("Тип:")); row.addWidget(kind)
        layout.addLayout(row); layout.addWidget(choose)
        actions = QHBoxLayout()
        remove = QPushButton("Удалить назначение")
        preview = QPushButton("Предпросмотр шаблона")
        close = QPushButton("Готово")
        actions.addWidget(remove); actions.addWidget(preview); actions.addStretch(); actions.addWidget(close)
        layout.addLayout(actions)
        def refresh():
            table.setRowCount(0)
            profiles = self.settings.get("hub_template_profiles", {})
            for key, item in sorted(profiles.items(), key=lambda pair: (pair[1].get("organization", "").casefold(), pair[1].get("card_type", ""))):
                r = table.rowCount(); table.insertRow(r)
                for c, value in enumerate((item.get("organization", ""), item.get("card_type", ""), item.get("template_path", ""))):
                    cell = QTableWidgetItem(value); cell.setData(Qt.UserRole, key); table.setItem(r, c, cell)
        def assign():
            name = org.currentText().strip()
            if not name:
                QMessageBox.warning(dialog, APP_NAME, "Укажите организацию."); return
            path, _ = QFileDialog.getOpenFileName(dialog, "Выберите шаблон Magicard HUB", str(self.managed_templates_dir()), "Шаблоны Magicard (*.card)")
            if not path: return
            try:
                managed = copy_hub_template(path, self.managed_templates_dir())
                self.settings["hub_template_profiles"] = set_profile(self.settings.get("hub_template_profiles", {}), name, kind.currentText(), str(managed))
                save_json(SETTINGS_FILE, self.settings)
                with db_connect() as con:
                    write_audit(con, "hub_template_profile_assigned", None, name, "", f"Тип: {kind.currentText()}; шаблон: {managed.name}")
                refresh()
            except Exception as exc:
                QMessageBox.warning(dialog, APP_NAME, f"Шаблон не назначен: {exc}")
        def delete_profile():
            r = table.currentRow()
            if r < 0: return
            name = table.item(r, 0).text(); card_kind = table.item(r, 1).text()
            self.settings["hub_template_profiles"] = remove_profile(self.settings.get("hub_template_profiles", {}), name, card_kind)
            save_json(SETTINGS_FILE, self.settings); refresh()
        def show_preview():
            r = table.currentRow()
            if r < 0: return
            path = table.item(r, 2).text()
            try:
                design = load_card(path)
                value = design.get("about", {}).get("preview", "")
                if not value.startswith("data:image/"):
                    QMessageBox.information(dialog, APP_NAME, "В шаблоне нет встроенного предпросмотра. Откройте .card в Magicard HUB для просмотра."); return
                out = HISTORY_DIR / (safe_component(Path(path).stem) + "_profile_preview.png")
                extract_card_preview(path, out)
                if os.name == "nt": os.startfile(str(out))
                elif platform.system() == "Darwin": subprocess.Popen(["open", str(out)])
                else: subprocess.Popen(["xdg-open", str(out)])
            except Exception as exc:
                QMessageBox.warning(dialog, APP_NAME, f"Не удалось открыть предпросмотр: {exc}")
        choose.clicked.connect(assign); remove.clicked.connect(delete_profile); preview.clicked.connect(show_preview); close.clicked.connect(dialog.accept)
        refresh(); dialog.exec()

    def manage_organizations(self):
        dialog = QDialog(self)
        dialog.setWindowTitle("Организации и профили")
        dialog.setMinimumWidth(440)
        layout = QVBoxLayout(dialog)
        layout.addWidget(QLabel("Справочник организаций. Названия уже записанные у сотрудников сохраняются, даже если убрать организацию из справочника."))
        listing = QListWidget()
        listing.addItems(self.organization_names())
        layout.addWidget(listing, 1)
        entry = QLineEdit(); entry.setPlaceholderText("Название новой организации")
        layout.addWidget(entry)
        buttons = QHBoxLayout()
        add = QPushButton("Добавить")
        remove = QPushButton("Убрать из справочника")
        close = QPushButton("Готово")
        buttons.addWidget(add); buttons.addWidget(remove); buttons.addStretch(); buttons.addWidget(close)
        layout.addLayout(buttons)
        def refresh():
            listing.clear(); listing.addItems(self.organization_names())
        def add_org():
            try:
                name = normalize_organization_name(entry.text())
            except ValueError as exc:
                QMessageBox.warning(dialog, APP_NAME, str(exc)); return
            try:
                with db_connect() as con:
                    con.execute("INSERT INTO organization_catalog(name, created_at) VALUES(?,?)", (name, datetime.now().isoformat(timespec="seconds")))
                    write_audit(con, "organization_added", details=name)
                entry.clear(); refresh()
            except sqlite3.IntegrityError:
                QMessageBox.information(dialog, APP_NAME, "Такая организация уже есть в справочнике.")
        def remove_org():
            item = listing.currentItem()
            if not item: return
            name = item.text()
            if QMessageBox.question(dialog, APP_NAME, f"Убрать «{name}» из справочника? Записи сотрудников не изменятся.") != QMessageBox.Yes: return
            with db_connect() as con:
                con.execute("DELETE FROM organization_catalog WHERE name=?", (name,))
                write_audit(con, "organization_removed_from_catalog", details=name)
            refresh()
        add.clicked.connect(add_org); remove.clicked.connect(remove_org); close.clicked.connect(dialog.accept)
        dialog.exec()

    def eventFilter(self, watched, event):
        if event.type() in (QEvent.MouseButtonPress, QEvent.MouseMove, QEvent.KeyPress, QEvent.Wheel, QEvent.TouchBegin):
            if hasattr(self, "idle_lock_timer") and not getattr(self, "_locking_now", False):
                self.reset_idle_lock_timer()
        return super().eventFilter(watched, event)

    def reset_idle_lock_timer(self):
        minutes = int(self.settings.get("idle_lock_minutes", 10) or 0)
        if hasattr(self, "idle_lock_timer"):
            self.idle_lock_timer.stop()
            if minutes > 0:
                self.idle_lock_timer.start(minutes * 60 * 1000)

    def configure_idle_lock(self):
        if not self.require_role("roles"):
            return
        current = int(self.settings.get("idle_lock_minutes", 10) or 0)
        value, ok = QInputDialog.getInt(self, "Автоблокировка", "Блокировать программу после бездействия (минуты; 0 — выключено):", current, 0, 240, 1)
        if not ok:
            return
        self.settings["idle_lock_minutes"] = value
        save_json(SETTINGS_FILE, self.settings)
        self.reset_idle_lock_timer()
        with db_connect() as con:
            write_audit(con, "idle_lock_configured", details=f"Таймаут: {value} мин.")
        QMessageBox.information(self, APP_NAME, "Настройка автоблокировки сохранена. Для защиты требуется настроенный PIN администратора/оператора.")

    def lock_after_inactivity(self):
        with db_connect() as con:
            configured = con.execute("SELECT 1 FROM app_roles LIMIT 1").fetchone()
            if not configured:
                configured = con.execute("SELECT 1 FROM operator_accounts WHERE is_enabled=1 LIMIT 1").fetchone()
            if configured and getattr(self, "active_session_id", None):
                end_session(con, self.active_session_id, "idle_lock")
                record_security_event(con, "session_ended", actor=getattr(self, "active_session_username", ""),
                                      details="Сессия завершена из-за автоблокировки по бездействию")
                self.active_session_id = None
        if not configured:
            self.statusBar().showMessage("Автоблокировка пропущена: учётные записи ещё не настроены", 8000)
            return
        self._locking_now = True
        self.setEnabled(False)
        try:
            QMessageBox.information(self, APP_NAME, "Программа заблокирована из-за бездействия. Для продолжения потребуется PIN.")
            while not self.require_role("view"):
                answer = QMessageBox.question(self, APP_NAME, "Доступ не подтверждён. Повторить ввод PIN?", QMessageBox.Yes | QMessageBox.No, QMessageBox.Yes)
                if answer != QMessageBox.Yes:
                    QApplication.instance().quit()
                    return
            self.statusBar().showMessage("Доступ восстановлен", 5000)
        finally:
            self.setEnabled(True)
            self._locking_now = False
            self.reset_idle_lock_timer()

    def open_audit_search(self):
        if not self.require_role("export"):
            return
        dlg = QDialog(self); dlg.setWindowTitle("Журнал действий — поиск и фильтры"); dlg.resize(980, 560)
        layout = QVBoxLayout(dlg)
        filters = QHBoxLayout()
        query = QLineEdit(); query.setPlaceholderText("Поиск по событию, ФИО, номеру или деталям…")
        actor_filter = QComboBox(); actor_filter.addItem("Все операторы", "")
        with db_connect() as con:
            actors = [r[0] for r in con.execute("SELECT DISTINCT actor FROM security_events WHERE COALESCE(actor, '')<>'' ORDER BY actor COLLATE NOCASE").fetchall()]
        for actor in actors: actor_filter.addItem(actor, actor)
        filters.addWidget(query, 1); filters.addWidget(actor_filter); layout.addLayout(filters)
        table = QTableWidget(0, 6); table.setHorizontalHeaderLabels(["Дата и время", "Событие", "ФИО", "№ пропуска", "Оператор", "Подробности"])
        table.setSelectionBehavior(QAbstractItemView.SelectRows); table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        table.horizontalHeader().setSectionResizeMode(5, QHeaderView.Stretch); layout.addWidget(table, 1)
        def reload_rows():
            term = query.text().strip().casefold()
            with db_connect() as con:
                rows = con.execute("SELECT created_at,event_type,full_name,badge_number,operator,details FROM audit_log ORDER BY id DESC LIMIT 3000").fetchall()
            selected_actor = actor_filter.currentData()
            rows = [r for r in rows if (not selected_actor or str(r[2] or "") == selected_actor) and (not term or term in " ".join(str(x or "") for x in r).casefold())]
            table.setRowCount(len(rows))
            for i, row in enumerate(rows):
                for j, value in enumerate(row): table.setItem(i, j, QTableWidgetItem(str(value or "")))
        query.textChanged.connect(reload_rows); actor_filter.currentIndexChanged.connect(reload_rows); reload_rows()
        buttons = QDialogButtonBox(QDialogButtonBox.Close); buttons.rejected.connect(dlg.reject); buttons.accepted.connect(dlg.accept); layout.addWidget(buttons)
        dlg.exec()

    def open_folder(self, path):
        Path(path).mkdir(parents=True, exist_ok=True)
        try:
            if os.name == "nt": os.startfile(str(path))
            elif sys.platform == "darwin": subprocess.Popen(["open", str(path)])
            else: subprocess.Popen(["xdg-open", str(path)])
        except Exception as exc:
            QMessageBox.warning(self, APP_NAME, f"Не удалось открыть папку:\n{exc}")

    def open_data_folder(self):
        self.open_folder(APP_DATA_DIR)

    def create_support_bundle_ui(self):
        """Export a privacy-minimal diagnostic ZIP without employee content."""
        default_name = f"PSPassMaster_support_{datetime.now().strftime('%Y%m%d_%H%M%S')}.zip"
        target, _ = QFileDialog.getSaveFileName(
            self, "Сохранить диагностический пакет", str(Path.home() / "Desktop" / default_name),
            "ZIP-архив (*.zip)"
        )
        if not target:
            return
        if not target.lower().endswith(".zip"):
            target += ".zip"
        template = self.settings.get("hub_card_template", "")
        try:
            result = create_support_bundle(
                Path(target), app_version=APP_VERSION, app_data_dir=APP_DATA_DIR,
                db_path=DB_FILE, template_path=Path(template) if template else None,
                hub_path=self.settings.get("hub_path", "")
            )
            QMessageBox.information(
                self, APP_NAME,
                "Диагностический пакет создан и проверен.\n\n"
                f"Файл: {result}\n\n"
                "В архив не включены база сотрудников, фотографии, шаблоны, настройки, PIN и исходные журналы. "
                "Перед отправкой проверьте архив и отправляйте его только доверенному специалисту."
            )
        except Exception as exc:
            logging.exception("Support bundle creation failed")
            QMessageBox.critical(self, APP_NAME, f"Не удалось создать диагностический пакет:\n{exc}")

    def check_hub_template(self):
        """Preflight the selected HUB design before any employee/card export."""
        template = self.settings.get("hub_card_template", str(ROOT / "Templates" / "New Card Design.card"))
        try:
            report = inspect_template(template)
            QMessageBox.information(self, "Проверка шаблона Magicard HUB",
                "Шаблон прошёл проверку.\n\n"
                f"Файл: {Path(template).name}\n"
                f"Фотообъект: {report['photo_object']}\n"
                f"Объектов на лицевой стороне: {report['elements']}\n"
                f"Встроенный предпросмотр: {'есть' if report['has_preview'] else 'нет'}\n"
                f"Изображения: {report['photo_images']['base64image']['width']}×{report['photo_images']['base64image']['height']} px / "
                f"{report['photo_images']['base64image']['format']} и "
                f"{report['photo_images']['originalBase64Image']['width']}×{report['photo_images']['originalBase64Image']['height']} px / "
                f"{report['photo_images']['originalBase64Image']['format']}\n\n"
                "Можно создавать персональные копии .card. Исходный шаблон останется неизменным.")
        except Exception as exc:
            logging.exception("HUB template preflight failed")
            QMessageBox.warning(self, "Шаблон Magicard HUB не прошёл проверку", str(exc))

    def select_hub_template_ui(self):
        path = self.choose_hub_card_template()
        if path:
            self.template_path_label.setText(path)

    def open_update_dialog(self):
        dialog = UpdateDialog(self, ROOT, self.settings)
        dialog.exec()
        self.settings["update_source_url"] = dialog.url_edit.text().strip()
        save_json(SETTINGS_FILE, self.settings)

    def refresh_dashboard(self):
        """Refresh the dashboard without changing employee records."""
        try:
            with db_connect() as con:
                active = con.execute("SELECT COUNT(*) FROM employees WHERE COALESCE(is_deleted,0)=0").fetchone()[0]
                expired = con.execute("SELECT COUNT(*) FROM employees WHERE COALESCE(is_deleted,0)=0 AND COALESCE(card_status,'Активен')='Активен'").fetchone()[0]
                issued = con.execute("SELECT COUNT(*) FROM issuance_history").fetchone()[0]
            self.dashboard_summary.setText(f"Сотрудников в реестре: {active}    ·    Активных по учётному статусу: {expired}    ·    История выпусков: {issued}")
        except Exception:
            logging.exception("Dashboard refresh failed")
            self.dashboard_summary.setText("Не удалось загрузить сводку. Проверьте журнал программы.")

    def run_system_check(self):
        """Run read-only checks of user data, SQLite, HUB template and HUB executable."""
        template = self.settings.get("hub_card_template", str(ROOT / "Templates" / "New Card Design.card"))
        checks = readiness_checks(app_data_dir=APP_DATA_DIR, template_path=Path(template),
                                  db_path=DB_FILE, hub_path=self.settings.get("hub_path", ""),
                                  backup_dir=BACKUP_DIR, photo_dir=EMPLOYEE_DIR / "Photos",
                                  template_dir=ROOT / "Templates")
        counts = summarize_checks(checks)
        labels = {"ok": "ГОТОВО", "warning": "ВНИМАНИЕ", "error": "ОШИБКА"}
        lines = [f"{'✓' if c['status']=='ok' else '!' if c['status']=='warning' else '✕'} {labels[c['status']]} — {c['name']}\n   {c['details']}" for c in checks]
        report = "\n\n".join(lines)
        self.dashboard_health.setText(f"Проверка готовности: {counts['ok']} успешно · {counts['warning']} предупреждений · {counts['error']} ошибок")
        title = "Проверка готовности пройдена" if counts['error'] == 0 else "Требуется внимание к настройкам"
        QMessageBox.information(self, title, report)
        self.statusBar().showMessage(self.dashboard_health.text(), 8000)
        return checks

    def open_first_run_wizard(self):
        """Guided, non-destructive first-run checklist; can be reopened anytime."""
        dlg = QDialog(self); dlg.setWindowTitle("Мастер первого запуска PS PassMaster PRO"); dlg.resize(620, 430)
        layout = QVBoxLayout(dlg)
        heading = QLabel("Подготовим программу к работе")
        heading.setStyleSheet("font-size:17pt;font-weight:700;color:#F2C230;"); layout.addWidget(heading)
        note = QLabel("Мастер ничего не удаляет и не меняет в карточках сотрудников. Выполните шаги по порядку; необязательные настройки можно пропустить.")
        note.setWordWrap(True); layout.addWidget(note)
        steps = ["Проверьте шаблон Magicard HUB (.card) в разделе «Magicard HUB».",
                 "Укажите путь к Magicard HUB.exe, если хотите открывать карточки из программы.",
                 "Настройте роли и PIN в меню «Ещё», если нескольким людям нужен разный доступ.",
                 "Откройте центр резервного копирования и создайте первую полную копию.",
                 "Добавьте сотрудника, фотографию и проверьте карточку перед выпуском."]
        for i, step in enumerate(steps, 1):
            label = QLabel(f"{i}. {step}"); label.setWordWrap(True); layout.addWidget(label)
        layout.addStretch(1)
        buttons = QDialogButtonBox(QDialogButtonBox.Close); buttons.rejected.connect(dlg.reject); buttons.accepted.connect(dlg.accept); layout.addWidget(buttons)
        dlg.exec()
        self.settings["first_run_wizard_seen"] = True; save_json(SETTINGS_FILE, self.settings)

    def show_about(self):
        QMessageBox.about(self, APP_NAME, f"<h3>PS PassMaster PRO — Access Card Studio</h3><p>Версия {APP_VERSION}</p><p>Реестр сотрудников, подготовка портретов и создание карточек .card для Magicard HUB.</p><p>Печать выполняется штатными средствами Magicard HUB.</p><p><b>Разработчик:</b> {DEVELOPER_NAME}</p>")

    def apply_settings(self):
        self.settings.setdefault("update_source_url", DEFAULT_UPDATE_SOURCE_URL)
        detected_hub = discover_hub_executable()
        configured_hub = self.settings.get("hub_path", "")
        if configured_hub and not Path(configured_hub).is_file():
            configured_hub = ""
        if not configured_hub and detected_hub:
            configured_hub = str(detected_hub)
            self.settings["hub_path"] = configured_hub
            save_json(SETTINGS_FILE, self.settings)
        self.hub_path.setText(configured_hub)
        self.settings.setdefault("printer_model", "Magicard 300")
        self.settings.setdefault("hub_version", "2.0.6")
        self.settings.setdefault("photo_profile", "Vnukovo portrait approx. 21.8x29.0mm 300dpi (estimated from HUB screenshot)")

    def choose_files(self):
        paths, _ = QFileDialog.getOpenFileNames(self, "Добавить фотографии", "", "Изображения (*.jpg *.jpeg *.png *.webp *.tif *.tiff *.bmp *.gif)")
        self.add_files(paths)

    def add_files(self, paths):
        added = 0
        for path in paths:
            p = Path(path)
            if p.is_dir():
                candidates = [x for x in p.iterdir() if x.suffix.lower() in SUPPORTED]
            else:
                candidates = [p] if p.suffix.lower() in SUPPORTED else []
            for f in candidates:
                s = str(f.resolve())
                if s not in self.files and f.is_file():
                    self.files.append(s); self.file_list.addItem(QListWidgetItem(f.name)); added += 1
        self.count_label.setText(f"Файлов: {len(self.files)}")
        if added: self.statusBar().showMessage(f"Добавлено файлов: {added}", 4000)
        if self.file_list.count(): self.file_list.setCurrentRow(0)

    def remove_selected(self):
        rows = sorted({self.file_list.row(i) for i in self.file_list.selectedItems()}, reverse=True)
        for r in rows:
            self.file_list.takeItem(r); self.files.pop(r)
        self.count_label.setText(f"Файлов: {len(self.files)}")
        self.preview_selected()

    def clear_files(self):
        self.files.clear(); self.prepared.clear(); self.file_list.clear()
        self.preview.clear(); self.preview.setText("Предварительный просмотр"); self.preview.reset_crop()
        self.count_label.setText("Файлов: 0"); self.magicard_file.clear()

    def preview_selected(self, *args):
        row = self.file_list.currentRow()
        if row < 0 or row >= len(self.files): return
        path = self.files[row]
        try:
            im, w, h = make_photo(path, self.width_cm.value(), self.height_cm.value(), self.dpi.value(), self.mode.currentText(), self.preview.focus_x, self.preview.focus_y, self.preview.zoom, self.photo_edits.get(path, {}))
            buf = io.BytesIO(); im.save(buf, format="PNG")
            qimg = QImage.fromData(buf.getvalue())
            pix = QPixmap.fromImage(qimg).scaled(self.preview.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation)
            self.preview.setPixmap(pix)
            self.info.setText(f"{Path(path).name} · результат {w} × {h} px · {self.dpi.value()} DPI")
            im.close(); buf.close()
        except Exception as e:
            self.preview.setText("Не удалось открыть изображение")
            self.info.setText(str(e)); logging.exception("Preview error: %s", path)

    def open_photo_editor(self):
        row = self.file_list.currentRow()
        if row < 0 or row >= len(self.files):
            QMessageBox.information(self, APP_NAME, "Сначала выберите фотографию."); return
        source = self.files[row]
        dlg = PhotoEditorDialog(source, self.photo_edits.get(source, {}), self)
        if dlg.exec() == QDialog.Accepted:
            self.photo_edits[source] = dlg.edits
            save_json(PHOTO_EDITS_FILE, self.photo_edits)
            # Rotation changes image geometry; center the crop before optional face detection.
            self.preview.focus_x = self.preview.focus_y = 0.5
            self.preview.zoom = 1.0
            self.preview_selected()
            self.statusBar().showMessage("Коррекция фотографии применена (исходник не изменён)", 5000)

    def detect_selected_face(self):
        row = self.file_list.currentRow()
        if row < 0 or row >= len(self.files):
            QMessageBox.information(self, APP_NAME, "Сначала выберите фотографию.")
            return
        edits = self.photo_edits.get(self.files[row], {})
        result = detect_face_focus(self.files[row], edits.get("rotation",0), edits.get("flip_h",False), edits.get("flip_v",False))
        if result is None:
            QMessageBox.information(self, APP_NAME,
                "Лицо не найдено или модуль распознавания не установлен.\n"
                "Можно установить OpenCV командой: pip install opencv-python-headless\n"
                "Либо выполнить кадрирование вручную мышью.")
            return
        self.preview.focus_x, self.preview.focus_y, _, suggested_zoom = result
        self.preview.zoom = suggested_zoom
        self.preview_selected()
        self.statusBar().showMessage("Лицо найдено — кадрирование обновлено", 5000)

    def prepare_all(self):
        if not self.files:
            QMessageBox.information(self, APP_NAME, "Сначала добавьте фотографии."); return
        selected = sorted({self.file_list.row(i) for i in self.file_list.selectedItems()})
        indexes = selected if selected else list(range(len(self.files)))
        self.progress.setMaximum(len(indexes)); self.progress.setValue(0)
        success = 0
        for n, idx in enumerate(indexes, 1):
            source = self.files[idx]
            try:
                fx, fy = self.preview.focus_x, self.preview.focus_y
                zoom = self.preview.zoom
                if self.auto_face.isChecked():
                    face = detect_face_focus(source, self.photo_edits.get(source, {}).get("rotation",0), self.photo_edits.get(source, {}).get("flip_h",False), self.photo_edits.get(source, {}).get("flip_v",False))
                    if face is not None:
                        fx, fy, _, suggested_zoom = face
                        zoom = max(zoom, suggested_zoom)
                im, w, h = make_photo(source, self.width_cm.value(), self.height_cm.value(), self.dpi.value(), self.mode.currentText(), fx, fy, zoom, self.photo_edits.get(source, {}))
                self.prepared[source] = (im.copy(), w, h)
                success += 1
            except Exception:
                logging.exception("Prepare error: %s", source)
            self.progress.setValue(n)
        self.statusBar().showMessage(f"Подготовлено: {success} из {len(indexes)}", 6000)
        self.preview_selected()

    def add_history(self, path, w, h, fmt):
        self.history.insert(0, {"path":path, "date":datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                                "pixels":f"{w}x{h}", "format":fmt})
        self.history = self.history[:500]
        save_json(HISTORY_FILE, self.history)

    def refresh_history(self):
        if not hasattr(self, "history_list"): return
        self.history_list.clear()
        for item in self.history:
            self.history_list.addItem(f"{item.get('date','')} | {Path(item.get('path','')).name} | {item.get('pixels','')} | {item.get('format','')}")
        if hasattr(self, "issuance_list"):
            self.issuance_list.clear()
            with db_connect() as con:
                issued = con.execute("SELECT created_at,full_name,badge_number,card_file FROM issuance_history ORDER BY id DESC LIMIT 100").fetchall()
            for item in issued:
                self.issuance_list.addItem(f"{item['created_at']} | {item['full_name']} | № {item['badge_number']} | {Path(item['card_file']).name}")
        if hasattr(self, "audit_list"):
            self.audit_list.clear()
            with db_connect() as con:
                events = con.execute(
                    "SELECT created_at,event_type,full_name,badge_number,details,operator "
                    "FROM audit_log ORDER BY id DESC LIMIT 200"
                ).fetchall()
            for event in events:
                self.audit_list.addItem(
                    f"{event['created_at']} | {event['operator']} | {event['event_type']} | "
                    f"{event['full_name']} | № {event['badge_number']} | {event['details']}"
                )

    def manual_backup(self):
        try:
            target = create_database_backup("manual")
            if target:
                QMessageBox.information(self, APP_NAME, f"Резервная копия базы создана:\\n{target}")
            else:
                QMessageBox.information(self, APP_NAME, "База сотрудников пока не создана.")
        except Exception as e:
            logging.exception("Manual backup failed")
            QMessageBox.critical(self, APP_NAME, f"Не удалось создать резервную копию:\\n{e}")

    def clear_history(self):
        if QMessageBox.question(self, APP_NAME, "Очистить историю?") == QMessageBox.Yes:
            self.history = []; save_json(HISTORY_FILE, self.history); self.refresh_history()

    def current_prepared_path(self):
        row = self.file_list.currentRow()
        if row >= 0 and row < len(self.files):
            src = self.files[row]
            if src in self.prepared:
                target = OUTPUT_DIR / f"{Path(src).stem}_prepared.jpg"
                self.prepared[src][0].save(target, "JPEG", quality=95, dpi=(self.dpi.value(), self.dpi.value()))
                self.add_history(str(target), self.prepared[src][1], self.prepared[src][2], "JPG")
                self.refresh_history()
                return str(target)
            QMessageBox.information(self, APP_NAME, "Сначала нажмите «Подготовить выбранные / все».")
        elif self.prepared:
            src, (im,w,h) = next(iter(self.prepared.items()))
            target = OUTPUT_DIR / f"{Path(src).stem}_prepared.jpg"
            im.save(target, "JPEG", quality=95, dpi=(self.dpi.value(), self.dpi.value()))
            return str(target)
        return None

    def copy_for_hub(self):
        # Privacy-first: never place an ID photo into the Windows clipboard.
        path = self.current_prepared_path()
        if not path:
            return
        self.magicard_file.setText(path)
        self.statusBar().showMessage("Фото сохранено локально для Magicard HUB.", 8000)
        QMessageBox.information(
            self, APP_NAME,
            "Фото подготовлено и сохранено локально:\n\n" + path +
            "\n\nОткройте Magicard HUB и выберите этот файл через кнопку добавления фотографии. "
            "Буфер обмена Windows не используется для защиты конфиденциальности."
        )

    def choose_hub(self):
        path, _ = QFileDialog.getOpenFileName(self, "Выбрать Magicard HUB", "", "Программы (*.exe);;Все файлы (*)")
        if path:
            self.hub_path.setText(path); self.settings["hub_path"] = path; save_json(SETTINGS_FILE, self.settings)

    def open_hub(self):
        path = self.hub_path.text().strip()
        if path and Path(path).exists():
            try: os.startfile(path) if os.name == "nt" else subprocess.Popen([path])
            except Exception as e: QMessageBox.warning(self, APP_NAME, f"Не удалось открыть HUB:\n{e}")
        else:
            QMessageBox.information(self, APP_NAME, "Укажите путь к исполняемому файлу Magicard HUB в поле выше. Если HUB уже открыт, переключитесь в его окно.")

    def latest_card_path(self):
        with db_connect() as con:
            row = con.execute("SELECT card_file FROM issuance_history WHERE card_file<>'' ORDER BY id DESC LIMIT 1").fetchone()
        if not row:
            return None
        path = Path(row["card_file"])
        return path if path.is_file() else None

    def open_last_card(self):
        path = self.latest_card_path()
        if not path:
            QMessageBox.information(self, APP_NAME, "В истории нет доступного файла .card. Сначала создайте карточку.")
            return
        try:
            open_card_file(path, self.hub_path.text().strip() or None)
            self.statusBar().showMessage(f"Передан на открытие в Magicard HUB: {path.name}", 8000)
        except Exception as exc:
            QMessageBox.warning(self, APP_NAME, f"Не удалось открыть карточку в Magicard HUB:\n{exc}\n\nМожно открыть файл вручную из папки результатов:\n{path}")

    def preview_last_card(self):
        path = self.latest_card_path()
        if not path:
            QMessageBox.information(self, APP_NAME, "В истории нет доступного файла .card.")
            return
        try:
            preview_path = HISTORY_DIR / f"{safe_component(path.stem)}_preview.png"
            extract_card_preview(path, preview_path)
            if os.name == "nt": os.startfile(str(preview_path))
            elif platform.system() == "Darwin": subprocess.Popen(["open", str(preview_path)])
            else: subprocess.Popen(["xdg-open", str(preview_path)])
            QMessageBox.information(self, APP_NAME, "Открыт встроенный предпросмотр из .card. Это визуальная проверка макета, а не подтверждение физического размера или качества печати в HUB.")
        except Exception as exc:
            QMessageBox.warning(self, APP_NAME, f"Не удалось показать предпросмотр:\n{exc}")

    def restore_database_backup(self):
        path, _ = QFileDialog.getOpenFileName(self, "Выберите резервную копию базы", str(BACKUP_DIR), "База SQLite (*.sqlite3)")
        if not path: return
        source = Path(path)
        try:
            check = sqlite3.connect(f"file:{source.as_posix()}?mode=ro", uri=True)
            integrity = check.execute("PRAGMA integrity_check").fetchone()
            tables = {r[0] for r in check.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            check.close()
            if not integrity or integrity[0] != "ok" or "employees" not in tables:
                raise ValueError("Выбранный файл не прошёл проверку целостности или не содержит реестр сотрудников.")
            answer = QMessageBox.question(self, APP_NAME,
                "Текущая база будет сохранена в резервную копию, а затем заменена выбранной.\n\nПродолжить?",
                QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
            if answer != QMessageBox.Yes: return
            create_database_backup("before_restore")
            temp = DB_FILE.with_suffix(".restore.tmp")
            shutil.copy2(source, temp)
            verify = sqlite3.connect(temp)
            ok = verify.execute("PRAGMA integrity_check").fetchone()
            verify.close()
            if not ok or ok[0] != "ok":
                temp.unlink(missing_ok=True)
                raise ValueError("Копия повреждена при проверке перед восстановлением.")
            temp.replace(DB_FILE)
            self.refresh_employees(); self.refresh_history()
            QMessageBox.information(self, APP_NAME, "База успешно восстановлена. Предыдущая версия сохранена в Backups.")
        except Exception as exc:
            logging.exception("Database restore failed")
            QMessageBox.critical(self, APP_NAME, f"Не удалось восстановить базу:\n{exc}")

    def open_output(self):
        OUTPUT_DIR.mkdir(exist_ok=True)
        try: os.startfile(str(OUTPUT_DIR)) if os.name == "nt" else subprocess.Popen(["xdg-open", str(OUTPUT_DIR)])
        except Exception: logging.exception("Open folder error")

    def guided_insert(self):
        # Save a local file and optionally launch HUB; no clipboard or network transfer.
        path = self.current_prepared_path()
        if not path:
            return
        self.magicard_file.setText(path)
        hub = self.hub_path.text().strip()
        if hub and Path(hub).exists():
            try:
                os.startfile(hub) if os.name == "nt" else subprocess.Popen([hub])
            except Exception as e:
                QMessageBox.warning(self, APP_NAME, f"Не удалось открыть HUB:\n{e}")
        try:
            if os.name == "nt":
                subprocess.Popen(["explorer", "/select,", str(Path(path).resolve())])
            else:
                self.open_output()
        except Exception:
            logging.exception("Could not reveal prepared photo")
        QMessageBox.information(
            self, "Фото подготовлено для Magicard HUB",
            "Готовый JPG сохранён:\n" + path +
            "\n\nПапка с файлом открыта. В HUB откройте карточку сотрудника и импортируйте выделенный JPG штатной командой. "
            "Прямое заполнение карточки пока не заявляется: оно зависит от версии HUB и доступных способов интеграции."
        )

    def refresh_employees(self):
        if not hasattr(self, "employee_table"): return
        with db_connect() as con:
            rows = con.execute("SELECT * FROM employees WHERE COALESCE(is_deleted,0)=0 ORDER BY full_name COLLATE NOCASE").fetchall()
        self.employee_table.setRowCount(len(rows))
        for r, rec in enumerate(rows):
            values = [rec["id"], rec["full_name"], rec["organization"], rec["position"], rec["badge_number"], rec["issue_date"], rec["valid_until"]]
            for c, value in enumerate(values):
                cell = QTableWidgetItem(str(value or ""))
                if c == 6:
                    status = expiry_status(str(value or ""))
                    if status == "expired": cell.setForeground(QColor("#ff5b5b")); cell.setToolTip("Срок действия пропуска истёк")
                    elif status == "soon": cell.setForeground(QColor("#f2c230")); cell.setToolTip("Срок действия истекает в течение 30 дней")
                    elif status == "unknown": cell.setToolTip("Не удалось распознать дату срока действия")
                self.employee_table.setItem(r, c, cell)
        self.employee_table.resizeRowsToContents()
        expired = sum(1 for rec in rows if expiry_status(rec["valid_until"]) == "expired")
        soon = sum(1 for rec in rows if expiry_status(rec["valid_until"]) == "soon")
        self.statusBar().showMessage(f"Сотрудников: {len(rows)} · Истёкших пропусков: {expired} · Истекают за 30 дней: {soon}")
        self.refresh_number_status()
        if hasattr(self, "dashboard_summary"):
            self.refresh_dashboard()

    def filter_employees(self, text):
        query = (text or "").strip().casefold()
        for row in range(self.employee_table.rowCount()):
            values = [self.employee_table.item(row, col).text() if self.employee_table.item(row, col) else ""
                      for col in range(self.employee_table.columnCount())]
            self.employee_table.setRowHidden(row, bool(query) and query not in " ".join(values).casefold())

    def match_selected_employee_photos(self):
        """Assign photos to selected records only when filename match is unique."""
        rows = sorted({idx.row() for idx in self.employee_table.selectionModel().selectedRows()})
        if not rows:
            QMessageBox.information(self, APP_NAME, "Выберите сотрудников в реестре.")
            return
        folder = QFileDialog.getExistingDirectory(self, "Папка с фотографиями сотрудников", str(OUTPUT_DIR))
        if not folder:
            return
        photo_index = index_photo_files(folder)
        if not photo_index:
            QMessageBox.information(self, APP_NAME, "В выбранной папке не найдены JPG, PNG, BMP или WEBP.")
            return
        create_database_backup("before_batch_photo_match")
        matched, skipped = [], []
        with db_connect() as con:
            for row in rows:
                item = self.employee_table.item(row, 0)
                rec_id = int(item.text()) if item else None
                rec_row = con.execute("SELECT * FROM employees WHERE id=?", (rec_id,)).fetchone()
                if not rec_row:
                    skipped.append(f"ID {rec_id}: запись не найдена")
                    continue
                rec = dict(rec_row)
                photo, reason = match_employee_photo(rec, photo_index)
                if photo is None:
                    skipped.append(f"{rec.get('full_name')}: {reason}")
                    continue
                if not self.confirm_photo_not_duplicate(str(photo), exclude_employee_id=rec_id):
                    skipped.append(f"{rec.get('full_name')}: фотография отклонена проверкой повторов")
                    continue
                con.execute("UPDATE employees SET photo_path=?, updated_at=? WHERE id=?",
                            (str(photo.resolve()), datetime.now().isoformat(timespec="seconds"), rec_id))
                write_audit(con, "employee_photo_auto_matched", rec_id, rec.get("full_name", ""),
                            rec.get("badge_number", ""), f"Фото сопоставлено: {photo.name}; {reason}")
                matched.append(f"{rec.get('full_name')}: {photo.name}")
        self.refresh_employees()
        message = f"Сопоставлено фотографий: {len(matched)}\nПропущено: {len(skipped)}"
        if skipped:
            message += "\n\nТребуют ручной проверки:\n" + "\n".join(skipped[:30])
        QMessageBox.information(self, "Автопоиск фотографий", message)

    def check_selected_employee_photos(self):
        rows = sorted({idx.row() for idx in self.employee_table.selectionModel().selectedRows()})
        if not rows:
            QMessageBox.information(self, APP_NAME, "Выберите сотрудников в реестре.")
            return
        results = []
        for row in rows:
            item = self.employee_table.item(row, 0)
            rec_id = int(item.text()) if item else None
            with db_connect() as con:
                rec = con.execute("SELECT full_name, photo_path FROM employees WHERE id=?", (rec_id,)).fetchone()
            if not rec:
                continue
            report = assess_photo(rec["photo_path"] or "")
            status = "OK" if report["ok"] and not report["warnings"] else ("ПРОВЕРИТЬ" if report["ok"] else "ОШИБКА")
            results.append(f"{status} · {rec['full_name']}\n{format_report(report)}")
        QMessageBox.information(self, "Контроль качества фотографий", "\n\n".join(results)[:12000])

    def export_photo_quality_report(self):
        """Analyze active employee portraits and export a non-destructive CSV report."""
        with db_connect() as con:
            records = [dict(row) for row in con.execute(
                "SELECT id, full_name, badge_number, photo_path FROM employees WHERE COALESCE(is_deleted, 0)=0 ORDER BY full_name COLLATE NOCASE"
            ).fetchall()]
        if not records:
            QMessageBox.information(self, APP_NAME, "В реестре нет активных сотрудников для проверки.")
            return
        destination, _ = QFileDialog.getSaveFileName(
            self, "Сохранить отчёт качества фотографий",
            str(OUTPUT_DIR / f"photo_quality_{datetime.now():%Y%m%d_%H%M%S}.csv"),
            "CSV (*.csv)"
        )
        if not destination:
            return
        if not destination.lower().endswith(".csv"):
            destination += ".csv"
        try:
            rows = assess_employee_photos(records)
            write_photo_quality_csv(rows, destination)
        except Exception as exc:
            logging.exception("Photo quality report failed")
            QMessageBox.critical(self, APP_NAME, f"Не удалось сформировать отчёт:\n{exc}")
            return
        counts = {key: sum(1 for row in rows if row["status"] == key) for key in ("ГОТОВО", "ПРОВЕРИТЬ", "ЗАМЕНИТЬ")}
        QMessageBox.information(
            self, "Отчёт качества фотографий",
            f"Проверено сотрудников: {len(rows)}\nГотово: {counts['ГОТОВО']}\nТребуют проверки: {counts['ПРОВЕРИТЬ']}\nТребуют замены: {counts['ЗАМЕНИТЬ']}\n\nОтчёт сохранён: {destination}\nИсходные фотографии не изменялись."
        )

    def selected_employee_id(self):
        row = self.employee_table.currentRow() if hasattr(self, "employee_table") else -1
        if row < 0: return None
        item = self.employee_table.item(row, 0)
        return int(item.text()) if item else None

    def get_employee(self):
        emp_id = self.selected_employee_id()
        if emp_id is None:
            QMessageBox.information(self, APP_NAME, "Сначала выберите сотрудника."); return None
        with db_connect() as con:
            rec = con.execute("SELECT * FROM employees WHERE id=?", (emp_id,)).fetchone()
        return dict(rec) if rec else None

    def next_badge_number(self):
        with db_connect() as con:
            values = [str(r[0]).strip() for r in con.execute(
                "SELECT badge_number FROM employees WHERE badge_number IS NOT NULL AND badge_number<>''"
            ).fetchall()]
        numeric = [(int(v), len(v)) for v in values if v.isdigit()]
        stored_last = int(self.settings.get("last_badge_number", 0) or 0)
        stored_width = int(self.settings.get("badge_number_width", 0) or 0)
        if numeric:
            highest, width = max(numeric, key=lambda x: x[0])
            last = max(highest, stored_last)
            return str(last + 1).zfill(max(width, stored_width))
        return str(max(stored_last + 1, int(self.settings.get("badge_start_number", 1)))).zfill(stored_width)

    def sync_last_badge_number(self):
        with db_connect() as con:
            vals = [str(r[0]).strip() for r in con.execute(
                "SELECT badge_number FROM employees WHERE badge_number IS NOT NULL AND badge_number<>''"
            ).fetchall()]
        numeric = [(int(v), len(v)) for v in vals if v.isdigit()]
        stored = int(self.settings.get("last_badge_number", 0) or 0)
        stored_width = int(self.settings.get("badge_number_width", 0) or 0)
        last = max([stored] + [n for n, _ in numeric])
        width = max([stored_width] + [w for n, w in numeric if n == last])
        if last != stored or width != stored_width:
            self.settings["last_badge_number"] = last
            self.settings["badge_number_width"] = width
            save_json(SETTINGS_FILE, self.settings)

    def refresh_number_status(self):
        if not hasattr(self, "number_status"):
            return
        self.sync_last_badge_number()
        previous = self.settings.get("last_badge_number", "—")
        self.number_status.setText(
            f"Предыдущая нумерация: {previous}    "
            f"Следующий номер: {self.next_badge_number()}"
        )

    def confirm_photo_not_duplicate(self, photo_path, exclude_employee_id=None):
        """Warn before assigning an identical or visually similar portrait to another employee."""
        if not photo_path or not Path(photo_path).is_file():
            return True
        with db_connect() as con:
            records = con.execute("SELECT id,full_name,badge_number,photo_path FROM employees WHERE photo_path<>''").fetchall()
        matches = find_photo_duplicates(photo_path, records, exclude_employee_id=exclude_employee_id)
        if not matches:
            return True
        lines = []
        for item in matches[:8]:
            kind = "точное совпадение" if item["kind"] == "exact" else f"похожее фото (разница {item['distance']}/64)"
            lines.append(f"• {item['name']} · пропуск {item['badge'] or '—'} — {kind}")
        if len(matches) > 8:
            lines.append(f"… и ещё {len(matches) - 8}")
        answer = QMessageBox.question(
            self, APP_NAME,
            "Похоже, эта фотография уже назначена другому сотруднику:\n\n" + "\n".join(lines) +
            "\n\nПродолжить? Проверка выполняется локально, фотографии никуда не отправляются.",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
        return answer == QMessageBox.Yes

    def add_employee(self):
        if not self.require_role("add"): return
        proposed = self.next_badge_number()
        dlg = EmployeeDialog(self, {"badge_number": proposed}, self.organization_names())
        if dlg.exec() != QDialog.Accepted:
            return
        data = dlg.values()
        if not data["full_name"]:
            QMessageBox.warning(self, APP_NAME, "Заполните ФИО сотрудника.")
            return
        if not self.confirm_photo_not_duplicate(data.get("photo_path")):
            return
        with db_connect() as con:
            same_name = con.execute("SELECT badge_number FROM employees WHERE lower(trim(full_name))=lower(trim(?))", (data["full_name"],)).fetchall()
        if same_name and QMessageBox.question(self, APP_NAME, "Сотрудник с таким ФИО уже есть в базе. Всё равно добавить новую запись?") != QMessageBox.Yes:
            return
        create_database_backup("before_add")
        now = datetime.now().isoformat(timespec="seconds")
        try:
            with db_connect() as con:
                con.execute("BEGIN IMMEDIATE")
                entered = (data.get("badge_number") or "").strip()
                if not entered or entered == proposed:
                    vals = [str(r[0]).strip() for r in con.execute(
                        "SELECT badge_number FROM employees WHERE badge_number IS NOT NULL AND badge_number<>''"
                    ).fetchall()]
                    numeric = [(int(v), len(v)) for v in vals if v.isdigit()]
                    stored_last = int(self.settings.get("last_badge_number", 0) or 0)
                    stored_width = int(self.settings.get("badge_number_width", 0) or 0)
                    if numeric:
                        highest, width = max(numeric, key=lambda x: x[0])
                        entered = str(max(highest, stored_last) + 1).zfill(max(width, stored_width))
                    else:
                        entered = str(max(stored_last + 1, int(self.settings.get("badge_start_number", 1)))).zfill(stored_width)
                duplicate = con.execute(
                    "SELECT 1 FROM employees WHERE badge_number=? AND badge_number<>''", (entered,)
                ).fetchone()
                if duplicate:
                    raise ValueError(f"Номер пропуска {entered} уже используется. Обновите форму и повторите.")
                con.execute(
                    "INSERT INTO employees(full_name,organization,department,position,badge_number,valid_until,photo_path,issue_date,created_at,updated_at) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?)",
                    (*[data[k] for k in ("full_name","organization","department","position")],
                     entered, data["valid_until"], data["photo_path"], data["issue_date"], now, now)
                )
                new_id = con.execute("SELECT last_insert_rowid()").fetchone()[0]
                write_audit(con, "employee_created", new_id, data["full_name"], entered, "Создана карточка сотрудника")
                if entered.isdigit():
                    self.settings["last_badge_number"] = max(int(self.settings.get("last_badge_number", 0) or 0), int(entered))
                    self.settings["badge_number_width"] = max(int(self.settings.get("badge_number_width", 0) or 0), len(entered))
                    save_json(SETTINGS_FILE, self.settings)
        except (sqlite3.IntegrityError, ValueError) as e:
            QMessageBox.warning(self, APP_NAME, str(e))
            return
        self.refresh_employees()

    def run_employee_data_integrity(self):
        """Run a read-only integrity scan over active employee records and portraits."""
        try:
            with db_connect() as con:
                rows = con.execute("SELECT id,full_name,organization,badge_number,photo_path,is_deleted FROM employees").fetchall()
            report = inspect_employee_records(rows)
            message = format_employee_integrity_report(report)
            if report.get("findings"):
                QMessageBox.warning(self, "Проверка данных сотрудников", message)
            else:
                QMessageBox.information(self, "Проверка данных сотрудников", message)
        except Exception as exc:
            logging.exception("Employee data integrity scan failed")
            QMessageBox.critical(self, "Проверка данных сотрудников", f"Не удалось завершить проверку. Подробности записаны в журнал программы.\n{type(exc).__name__}")

    def edit_employee(self):
        if not self.require_role("edit"): return
        rec = self.get_employee()
        if not rec: return
        dlg = EmployeeDialog(self, rec, self.organization_names())
        if dlg.exec() != QDialog.Accepted: return
        data = dlg.values()
        if not data["full_name"]: QMessageBox.warning(self, APP_NAME, "Заполните ФИО."); return
        if data.get("photo_path") != rec.get("photo_path") and not self.confirm_photo_not_duplicate(data.get("photo_path"), exclude_employee_id=rec["id"]):
            return
        if not data["badge_number"]:
            data["badge_number"] = self.next_badge_number()
        with db_connect() as con:
            same_name = con.execute("SELECT 1 FROM employees WHERE lower(trim(full_name))=lower(trim(?)) AND id<>? LIMIT 1", (data["full_name"], rec["id"])).fetchone()
            duplicate = con.execute("SELECT 1 FROM employees WHERE badge_number=? AND id<>? AND badge_number<>''",
                                    (data["badge_number"], rec["id"])).fetchone()
        if duplicate:
            QMessageBox.warning(self, APP_NAME, f"Номер пропуска {data['badge_number']} уже используется.")
            return
        if same_name and QMessageBox.question(self, APP_NAME, "В базе уже есть сотрудник с таким ФИО. Сохранить изменения?") != QMessageBox.Yes:
            return
        create_database_backup("before_edit")
        now = datetime.now().isoformat(timespec="seconds")
        try:
            with db_connect() as con:
                con.execute("BEGIN IMMEDIATE")
                duplicate = con.execute("SELECT 1 FROM employees WHERE badge_number=? AND id<>? AND badge_number<>''",
                                        (data["badge_number"], rec["id"])).fetchone()
                if duplicate:
                    raise ValueError(f"Номер пропуска {data['badge_number']} уже используется.")
                before = con.execute("SELECT * FROM employees WHERE id=?", (rec["id"],)).fetchone()
                if before is None:
                    raise ValueError("Карточка сотрудника больше не существует.")
                con.execute("UPDATE employees SET full_name=?,organization=?,department=?,position=?,badge_number=?,valid_until=?,photo_path=?,issue_date=?,updated_at=? WHERE id=?",
                    (*[data[k] for k in ("full_name","organization","department","position","badge_number","valid_until","photo_path","issue_date")], now, rec["id"]))
                after = con.execute("SELECT * FROM employees WHERE id=?", (rec["id"],)).fetchone()
                record_employee_change(con, rec["id"], before, after,
                    changed_by=os.environ.get("USERNAME") or os.environ.get("USER") or "local")
                write_audit(con, "employee_updated", rec["id"], data["full_name"], data["badge_number"], "Изменена карточка сотрудника; версия сохранена")
        except (sqlite3.IntegrityError, ValueError) as e:
            QMessageBox.warning(self, APP_NAME, str(e))
            return
        self.refresh_employees()

    def show_employee_change_history(self):
        """Show version history for the selected employee and allow safe rollback."""
        rec = self.get_employee()
        if not rec:
            QMessageBox.information(self, APP_NAME, "Сначала выберите сотрудника в реестре.")
            return
        try:
            with db_connect() as con:
                rows = list_employee_changes(con, rec["id"], limit=100)
            if not rows:
                QMessageBox.information(self, APP_NAME, "Для этой карточки пока нет сохранённых версий. История начнёт заполняться после изменения карточки в этой версии.")
                return
            labels = []
            for item in rows:
                changed = [key for key in item["before"] if item["before"].get(key) != item["after"].get(key)]
                label = f"#{item['id']} · {item['changed_at']} · {item['change_kind']} · {', '.join(changed) or 'без изменений'}"
                labels.append(label)
            selected, ok = QInputDialog.getItem(self, "История изменений сотрудника",
                f"{rec.get('full_name','')} · пропуск {rec.get('badge_number','')}\nВыберите версию для просмотра и восстановления:", labels, 0, False)
            if not ok or not selected:
                return
            index = labels.index(selected)
            item = rows[index]
            changed = [key for key in item["before"] if item["before"].get(key) != item["after"].get(key)]
            field_names = {"full_name":"ФИО", "organization":"Организация", "department":"Отдел", "position":"Должность", "badge_number":"Номер пропуска", "valid_until":"Действителен до", "photo_path":"Фотография", "issue_date":"Дата выдачи"}
            details = []
            for key in changed:
                before_value = item["before"].get(key, "")
                after_value = item["after"].get(key, "")
                if key == "photo_path":
                    before_value = Path(before_value).name if before_value else "—"
                    after_value = Path(after_value).name if after_value else "—"
                details.append(f"{field_names.get(key,key)}:\n  Было: {before_value or '—'}\n  Стало: {after_value or '—'}")
            message = f"Версия #{item['id']} от {item['changed_at']}\n\n" + "\n\n".join(details)
            message += "\n\nВосстановление создаст новую запись истории. Текущая версия не будет удалена."
            if QMessageBox.question(self, "Просмотр версии карточки", message + "\n\nВосстановить прежние значения?", QMessageBox.Yes | QMessageBox.No, QMessageBox.No) != QMessageBox.Yes:
                return
            if not self.require_role("edit"):
                return
            create_database_backup("before_employee_version_restore")
            with db_connect() as con:
                con.execute("BEGIN IMMEDIATE")
                result = restore_employee_change(con, item["id"], changed_by=os.environ.get("USERNAME") or os.environ.get("USER") or "local")
                current = con.execute("SELECT full_name,badge_number FROM employees WHERE id=?", (rec["id"],)).fetchone()
                write_audit(con, "employee_version_restored", rec["id"], current["full_name"], current["badge_number"], f"Восстановлена версия #{item['id']}; новая запись истории #{result['history_id']}")
            self.refresh_employees()
            QMessageBox.information(self, APP_NAME, "Предыдущая версия карточки восстановлена. Текущая версия сохранена в истории.")
        except (ValueError, LookupError, sqlite3.Error) as exc:
            QMessageBox.warning(self, APP_NAME, f"Не удалось восстановить версию: {exc}")
        except Exception as exc:
            logging.exception("Employee version history operation failed")
            QMessageBox.critical(self, APP_NAME, f"Ошибка работы с историей. Подробности записаны в журнал.\n{type(exc).__name__}")

    def delete_employee(self):
        if not self.require_role("delete"): return
        rec = self.get_employee()
        if not rec: return
        if QMessageBox.question(self, APP_NAME, f"Переместить сотрудника «{rec['full_name']}» в корзину? Его можно будет восстановить.") != QMessageBox.Yes: return
        create_database_backup("before_soft_delete")
        now = datetime.now().isoformat(timespec="seconds")
        with db_connect() as con:
            con.execute("UPDATE employees SET is_deleted=1, deleted_at=?, updated_at=? WHERE id=?", (now, now, rec["id"]))
            write_audit(con, "employee_trashed", rec["id"], rec.get("full_name",""), rec.get("badge_number",""), "Сотрудник перемещён в корзину")
        self.refresh_employees()

    def manage_card_status(self):
        if not self.require_role("edit"): return
        rec = self.get_employee()
        if not rec: return
        card_type, ok = QInputDialog.getItem(self, "Тип пропуска", "Выберите тип:", list(CARD_TYPES), CARD_TYPES.index(rec.get("card_type", "Постоянный")) if rec.get("card_type", "Постоянный") in CARD_TYPES else 0, False)
        if not ok: return
        status, ok = QInputDialog.getItem(self, "Статус пропуска", "Выберите статус:", list(CARD_STATUSES), CARD_STATUSES.index(rec.get("card_status", "Активен")) if rec.get("card_status", "Активен") in CARD_STATUSES else 0, False)
        if not ok: return
        reason = ""
        if status == "Заблокирован":
            reason, ok = QInputDialog.getText(self, "Блокировка пропуска", "Причина блокировки:")
            if not ok: return
            if len(reason.strip()) < 3:
                QMessageBox.warning(self, APP_NAME, "Укажите причину блокировки (не менее 3 символов)."); return
        create_database_backup("before_card_status")
        operator = os.environ.get("USERNAME") or os.environ.get("USER") or "local"
        with db_connect() as con:
            con.execute("BEGIN IMMEDIATE")
            old_status = rec.get("card_status", "Активен")
            new_status = normalize_card_status(status)
            con.execute("UPDATE employees SET card_type=?,card_status=?,blocked_reason=?,updated_at=? WHERE id=?", (normalize_card_type(card_type), new_status, reason.strip(), datetime.now().isoformat(timespec="seconds"), rec["id"]))
            record_status_change(con, rec["id"], rec.get("full_name", ""), rec.get("badge_number", ""), old_status, new_status, reason.strip(), operator)
            write_audit(con, "card_status_changed", rec["id"], rec.get("full_name", ""), rec.get("badge_number", ""), f"Тип: {card_type}; статус: {status}; причина: {reason.strip()}", operator)
        self.refresh_employees()
        QMessageBox.information(self, APP_NAME, f"Сохранено: {card_type} / {status}.")

    def show_card_status_history(self):
        if not self.require_role("view"):
            return
        with db_connect() as con:
            rows = list_status_history(con, limit=2000)
        dlg = QDialog(self); dlg.setWindowTitle("Журнал блокировок и статусов пропусков"); dlg.resize(1050, 580)
        layout = QVBoxLayout(dlg)
        info = QLabel(f"Записей: {len(rows)}. Здесь фиксируются ручные изменения статуса, блокировки по утере/аннулированию и автоматические события.")
        info.setWordWrap(True); layout.addWidget(info)
        table = QTableWidget(len(rows), 8)
        table.setHorizontalHeaderLabels(["Дата и время", "Сотрудник", "№ пропуска", "Было", "Стало", "Причина", "Оператор", "Источник"])
        for r, item in enumerate(rows):
            values = [item.get("changed_at", ""), item.get("full_name", ""), item.get("badge_number", ""), item.get("old_status", ""), item.get("new_status", ""), item.get("reason", ""), item.get("operator", ""), item.get("source", "")]
            for c, value in enumerate(values): table.setItem(r, c, QTableWidgetItem(str(value or "")))
            if item.get("new_status") == "Заблокирован":
                for c in range(8): table.item(r, c).setForeground(QColor("#d94b4b"))
        table.setEditTriggers(QAbstractItemView.NoEditTriggers); table.setSelectionBehavior(QAbstractItemView.SelectRows)
        table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents); table.horizontalHeader().setStretchLastSection(True)
        layout.addWidget(table)
        close = QPushButton("Закрыть"); close.clicked.connect(dlg.accept); layout.addWidget(close, alignment=Qt.AlignRight)
        dlg.exec()

    def reissue_employee_card(self):
        if not self.require_role("issue"): return
        rec = self.get_employee()
        if not rec: return
        reason, ok = QInputDialog.getText(self, "Перевыпуск пропуска", "Причина (утеря, повреждение, изменение данных и т. п.):")
        if not ok: return
        try: reason = reissue_reason(reason)
        except ValueError as exc:
            QMessageBox.warning(self, APP_NAME, str(exc)); return
        new_number, ok = QInputDialog.getText(self, "Новый номер", "Новый номер пропуска:", text=self.next_badge_number())
        if not ok: return
        new_number = new_number.strip()
        if not new_number:
            QMessageBox.warning(self, APP_NAME, "Номер пропуска не может быть пустым."); return
        with db_connect() as con:
            duplicate = con.execute("SELECT 1 FROM employees WHERE badge_number=? AND id<>? AND is_deleted=0", (new_number, rec["id"])).fetchone()
        if duplicate:
            QMessageBox.warning(self, APP_NAME, "Этот номер уже используется."); return
        if QMessageBox.question(self, APP_NAME, f"Заменить номер {rec.get('badge_number','')} на {new_number}? Старый пропуск будет помечен как заменённый.") != QMessageBox.Yes: return
        create_database_backup("before_card_reissue")
        now = datetime.now().isoformat(timespec="seconds")
        with db_connect() as con:
            con.execute("BEGIN IMMEDIATE")
            con.execute("INSERT INTO card_reissues(employee_id,old_badge_number,new_badge_number,reason,created_at) VALUES(?,?,?,?,?)", (rec["id"], rec.get("badge_number", ""), new_number, reason, now))
            con.execute("UPDATE employees SET badge_number=?,card_status='Активен',blocked_reason='',issue_date=?,updated_at=? WHERE id=?", (new_number, datetime.now().strftime("%d.%m.%Y"), now, rec["id"]))
            write_audit(con, "card_reissued", rec["id"], rec.get("full_name", ""), new_number, f"Старый номер: {rec.get('badge_number','')}; причина: {reason}")
        self.refresh_employees()
        QMessageBox.information(self, APP_NAME, "Новый номер сохранён. Для выпуска нового файла .card используйте кнопку «Выпустить карточку HUB (.card)».")

    def record_card_movement(self):
        """Record physical handover/return events locally; does not operate a door controller."""
        if not self.require_role("issue"):
            return
        rec = self.get_employee()
        if not rec:
            return
        event, ok = QInputDialog.getItem(self, "Учёт пропуска", "Операция:", list(CARD_EVENTS), 0, False)
        if not ok:
            return
        note = ""
        if event in ("Утерян", "Аннулирован"):
            note, ok = QInputDialog.getText(self, "Причина операции", "Укажите причину:")
            if not ok:
                return
        try:
            validate_card_event(event, rec.get("badge_number", ""), note)
        except ValueError as exc:
            QMessageBox.warning(self, APP_NAME, str(exc))
            return
        if QMessageBox.question(self, APP_NAME,
                f"Записать операцию «{event}» для {rec.get('full_name', '')}, пропуск № {rec.get('badge_number', '')}?",
                QMessageBox.Yes | QMessageBox.No, QMessageBox.No) != QMessageBox.Yes:
            return
        if event in ("Утерян", "Аннулирован"):
            create_database_backup("before_card_custody_change")
        now = datetime.now().isoformat(timespec="seconds")
        with db_connect() as con:
            con.execute("BEGIN IMMEDIATE")
            insert_card_event(con, rec, event, note, os.environ.get("USERNAME") or os.environ.get("USER") or "local")
            if event in ("Утерян", "Аннулирован"):
                reason = ("Утеря: " if event == "Утерян" else "Аннулирован: ") + note.strip()
                con.execute("UPDATE employees SET card_status='Заблокирован',blocked_reason=?,updated_at=? WHERE id=?",
                            (reason, now, rec["id"]))
                record_status_change(con, rec["id"], rec.get("full_name", ""), rec.get("badge_number", ""), rec.get("card_status", "Активен"), "Заблокирован", reason, os.environ.get("USERNAME") or os.environ.get("USER") or "local", source="card_custody")
            write_audit(con, "card_custody_" + event.lower(), rec["id"], rec.get("full_name", ""),
                        rec.get("badge_number", ""), note or event)
        self.refresh_employees()
        extra = " Статус пропуска переведён в «Заблокирован»." if event in ("Утерян", "Аннулирован") else ""
        QMessageBox.information(self, APP_NAME, f"Операция «{event}» записана в журнал.{extra}")

    def show_card_movement_history(self):
        if not self.require_role("view"):
            return
        with db_connect() as con:
            rows = con.execute("SELECT created_at,full_name,badge_number,event_type,note,operator FROM card_movements ORDER BY id DESC LIMIT 1000").fetchall()
        dlg = QDialog(self); dlg.setWindowTitle("Журнал выдачи и возврата пропусков"); dlg.resize(1000, 560)
        layout = QVBoxLayout(dlg)
        table = QTableWidget(len(rows), 6)
        table.setHorizontalHeaderLabels(["Дата и время", "Сотрудник", "№ пропуска", "Операция", "Комментарий", "Оператор"])
        table.setEditTriggers(QAbstractItemView.NoEditTriggers); table.setSelectionBehavior(QAbstractItemView.SelectRows)
        table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
        table.horizontalHeader().setStretchLastSection(True)
        for r, item in enumerate(rows):
            for c, value in enumerate(item): table.setItem(r, c, QTableWidgetItem(str(value or "")))
        layout.addWidget(QLabel(f"Записей: {len(rows)}. Журнал хранится локально в базе программы."))
        layout.addWidget(table)
        buttons = QDialogButtonBox(QDialogButtonBox.Close); buttons.rejected.connect(dlg.reject); buttons.accepted.connect(dlg.accept); layout.addWidget(buttons)
        dlg.exec()

    def export_card_movement_history(self):
        if not self.require_role("export"):
            return
        path, _ = QFileDialog.getSaveFileName(self, "Экспорт журнала пропусков", "Журнал_пропусков.csv", "CSV (*.csv)")
        if not path:
            return
        with db_connect() as con:
            rows = con.execute("SELECT created_at,full_name,badge_number,event_type,note,operator FROM card_movements ORDER BY id").fetchall()
        try:
            with open(path, "w", newline="", encoding="utf-8-sig") as f:
                writer = csv.writer(f, delimiter=";")
                writer.writerow(["Дата и время", "Сотрудник", "Номер пропуска", "Операция", "Комментарий", "Оператор"])
                writer.writerows([list(row) for row in rows])
            QMessageBox.information(self, APP_NAME, f"Экспортировано записей: {len(rows)}\nФайл: {path}")
        except OSError as exc:
            QMessageBox.critical(self, APP_NAME, f"Не удалось сохранить CSV: {exc}")

    def show_reissue_history(self):
        with db_connect() as con:
            rows = con.execute("SELECT created_at,employee_id,old_badge_number,new_badge_number,reason FROM card_reissues ORDER BY id DESC LIMIT 200").fetchall()
        if not rows:
            QMessageBox.information(self, APP_NAME, "История перевыпусков пока пуста."); return
        lines = [f"{r['created_at']} | ID {r['employee_id']} | {r['old_badge_number']} → {r['new_badge_number']} | {r['reason']}" for r in rows]
        dlg = QDialog(self); dlg.setWindowTitle("История перевыпусков"); dlg.resize(850, 480)
        layout = QVBoxLayout(dlg); view = QPlainTextEdit(); view.setReadOnly(True); view.setPlainText("\n".join(lines)); layout.addWidget(view)
        buttons = QDialogButtonBox(QDialogButtonBox.Close); buttons.rejected.connect(dlg.reject); buttons.accepted.connect(dlg.accept); layout.addWidget(buttons); dlg.exec()

    def require_role(self, permission):
        self.current_operator = None
        with db_connect() as con:
            named_accounts = list_accounts(con, include_disabled=True)
            roles = {r["role"]: {"salt": r["pin_salt"], "hash": r["pin_hash"], "iterations": r["iterations"]}
                 for r in con.execute("SELECT * FROM app_roles").fetchall()}
        if not named_accounts and not roles:
            return True  # Backward-compatible legacy mode until access protection is configured.
        remaining = self.pin_guard.remaining()
        if remaining:
            with db_connect() as con:
                record_security_event(con, "lockout_active", permission=permission,
                                      details=f"Осталось секунд блокировки: {remaining}")
            QMessageBox.warning(self, APP_NAME, f"Вход временно заблокирован. Повторите через {remaining} сек.")
            return False
        verified_role = None
        verified_operator = None
        if named_accounts:
            username, ok = QInputDialog.getText(self, "Вход оператора", "Логин:")
            if not ok: return False
            pin, ok = QInputDialog.getText(self, "Вход оператора", "PIN:", QLineEdit.Password)
            if not ok: return False
            with db_connect() as con:
                verified_operator = authenticate_operator(con, username, pin)
            if verified_operator:
                verified_role = verified_operator["role"]
        else:
            pin, ok = QInputDialog.getText(self, "Проверка доступа", "Введите PIN:", QLineEdit.Password)
            if not ok: return False
            for role in ("admin", "operator"):
                record = roles.get(role)
                if record and verify_pin(pin, record):
                    verified_role = role
                    break
        if verified_role and (account_allows(verified_operator, permission) if verified_operator else role_allows(verified_role, permission)):
            self.pin_guard.success(); self.settings["pin_guard"] = self.pin_guard.state(); save_json(SETTINGS_FILE, self.settings)
            self.current_role = verified_role
            self.current_operator = verified_operator
            session_user = (verified_operator or {}).get("username", "Локальный оператор")
            active_user = getattr(self, "active_session_username", None)
            with db_connect() as con:
                if self.active_session_id and active_user != session_user:
                    end_session(con, self.active_session_id, "operator_changed")
                    self.active_session_id = None
                if not self.active_session_id:
                    self.active_session_id = start_session(con, verified_operator or {"username": session_user, "display_name": session_user, "role": verified_role})
                    self.active_session_username = session_user
                record_security_event(con, "login_success", actor=session_user, role=verified_role, permission=permission,
                                      details="Вход подтверждён; доступ разрешён")
            return True
        self.pin_guard.fail(); self.settings["pin_guard"] = self.pin_guard.state(); save_json(SETTINGS_FILE, self.settings)
        event = "access_denied" if verified_role else "login_failure"
        with db_connect() as con:
            record_security_event(con, event, actor=(verified_operator or {}).get("username", ""), role=verified_role or "", permission=permission,
                                  details="Недостаточно прав" if verified_role else "Неверный логин/PIN или отключённая учётная запись")
        remaining = self.pin_guard.remaining()
        QMessageBox.warning(self, APP_NAME, "Неверные учётные данные или недостаточно прав." + (f" Повторная попытка через {remaining} сек." if remaining else ""))
        return False

    def open_operator_sessions(self):
        if not self.require_role("roles"):
            return
        dlg = QDialog(self); dlg.setWindowTitle("Сессии операторов и отчёт по сменам"); dlg.resize(1040, 600)
        layout = QVBoxLayout(dlg)
        filters = QHBoxLayout()
        days = QSpinBox(); days.setRange(1, 3650); days.setValue(90); days.setSuffix(" дней")
        operator = QComboBox(); operator.addItem("Все операторы", "")
        with db_connect() as con:
            names = [r[0] for r in con.execute("SELECT DISTINCT username FROM operator_sessions ORDER BY username COLLATE NOCASE").fetchall()]
        for name in names: operator.addItem(name, name)
        refresh = QPushButton("Обновить"); export = QPushButton("Экспорт CSV…")
        filters.addWidget(QLabel("Период:")); filters.addWidget(days); filters.addWidget(operator); filters.addWidget(refresh); filters.addWidget(export); filters.addStretch()
        layout.addLayout(filters)
        summary_label = QLabel(); summary_label.setWordWrap(True); layout.addWidget(summary_label)
        table = QTableWidget(0, 8); table.setHorizontalHeaderLabels(["ID", "Логин", "Оператор", "Роль", "Начало", "Окончание", "Длительность", "Завершение"])
        table.setEditTriggers(QAbstractItemView.NoEditTriggers); table.setSelectionBehavior(QAbstractItemView.SelectRows)
        table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents); table.horizontalHeader().setStretchLastSection(True)
        layout.addWidget(table)
        state = {"rows": []}
        def load_rows():
            with db_connect() as con:
                rows = list_sessions(con, since_days=days.value(), username=operator.currentData() or None)
                failures = con.execute("SELECT COUNT(*) FROM security_events WHERE event_type='login_failure' AND created_at>=?", ((datetime.now()-__import__('datetime').timedelta(days=days.value())).isoformat(timespec='seconds'),)).fetchone()[0]
                denied = con.execute("SELECT COUNT(*) FROM security_events WHERE event_type='access_denied' AND created_at>=?", ((datetime.now()-__import__('datetime').timedelta(days=days.value())).isoformat(timespec='seconds'),)).fetchone()[0]
            state["rows"] = rows
            summary = session_summary(rows)
            warning = "  |  ВНИМАНИЕ: много неудачных попыток/отказов" if failures >= 5 or denied >= 5 else ""
            summary_label.setText(f"Сессий: {summary['sessions']}  |  Активных: {summary['active']}  |  Учтено времени: {format_duration(summary['total_seconds'])}  |  Неудачных входов: {failures}  |  Отказов в доступе: {denied}{warning}")
            summary_label.setStyleSheet("color:#FF6B6B;font-weight:700;" if warning else "color:#D4D9DE;")
            table.setRowCount(len(rows))
            for ri, row in enumerate(rows):
                values = [row['id'], row['username'], row['display_name'], row['role'], row['started_at'], row['ended_at'] or 'Активна', format_duration(duration_seconds(row)), row['end_reason'] or ('В работе' if not row['ended_at'] else '')]
                for ci, value in enumerate(values): table.setItem(ri, ci, QTableWidgetItem(str(value)))
        def save_csv():
            if not state["rows"]:
                QMessageBox.information(dlg, APP_NAME, "Нет данных для экспорта."); return
            path, _ = QFileDialog.getSaveFileName(dlg, "Экспорт смен", "Сессии_операторов.csv", "CSV (*.csv)")
            if not path: return
            try:
                export_sessions_csv(state["rows"], path)
                QMessageBox.information(dlg, APP_NAME, f"Отчёт сохранён:\n{path}")
            except Exception as exc:
                QMessageBox.critical(dlg, APP_NAME, f"Не удалось сохранить отчёт: {exc}")
        refresh.clicked.connect(load_rows); export.clicked.connect(save_csv)
        days.valueChanged.connect(load_rows); operator.currentIndexChanged.connect(load_rows)
        load_rows(); dlg.exec()

    def manage_operator_accounts(self):
        if not self.require_role("roles"):
            return
        dlg = QDialog(self); dlg.setWindowTitle("Учётные записи операторов"); dlg.resize(760, 430)
        layout = QVBoxLayout(dlg)
        note = QLabel("Локальные персональные учётные записи. После создания первой записи вход выполняется по логину и PIN; старые общие PIN-роли больше не используются для входа.")
        note.setWordWrap(True); layout.addWidget(note)
        table = QTableWidget(0, 5); table.setHorizontalHeaderLabels(["ID", "Логин", "Имя", "Роль", "Состояние"])
        table.setSelectionBehavior(QAbstractItemView.SelectRows); table.setSelectionMode(QAbstractItemView.SingleSelection)
        table.setEditTriggers(QAbstractItemView.NoEditTriggers); table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents); table.horizontalHeader().setStretchLastSection(True)
        layout.addWidget(table)
        def refresh():
            with db_connect() as con: rows = list_accounts(con)
            table.setRowCount(len(rows))
            for i,row in enumerate(rows):
                vals=(row["id"],row["username"],row["display_name"],"Администратор" if row["role"]=="admin" else "Оператор","Включена" if row["is_enabled"] else "Отключена")
                for j,value in enumerate(vals): table.setItem(i,j,QTableWidgetItem(str(value)))
        def selected_id():
            r=table.currentRow()
            return int(table.item(r,0).text()) if r>=0 and table.item(r,0) else None
        def add_account():
            username,ok=QInputDialog.getText(dlg,"Новая учётная запись","Логин:")
            if not ok:return
            display,ok=QInputDialog.getText(dlg,"Новая учётная запись","Отображаемое имя:")
            if not ok:return
            role,ok=QInputDialog.getItem(dlg,"Роль","Выберите роль:",["operator","admin"],0,False)
            if not ok:return
            pin,ok=QInputDialog.getText(dlg,"Новая учётная запись","PIN (4–64 цифры):",QLineEdit.Password)
            if not ok:return
            try:
                with db_connect() as con:
                    create_account(con,username,display,role,pin)
                    record_security_event(con,"roles_changed",role="admin",permission="roles",details=f"Создана учётная запись {username}")
                refresh()
            except Exception as exc: QMessageBox.warning(dlg,APP_NAME,str(exc))
        def change_pin():
            aid=selected_id()
            if aid is None: QMessageBox.information(dlg,APP_NAME,"Выберите учётную запись."); return
            pin,ok=QInputDialog.getText(dlg,"Изменение PIN","Новый PIN (4–64 цифры):",QLineEdit.Password)
            if not ok:return
            try:
                with db_connect() as con:
                    update_account_pin(con,aid,pin)
                    record_security_event(con,"roles_changed",role="admin",permission="roles",details=f"Изменён PIN учётной записи ID {aid}")
            except Exception as exc: QMessageBox.warning(dlg,APP_NAME,str(exc))
        def toggle_account():
            aid=selected_id()
            if aid is None: QMessageBox.information(dlg,APP_NAME,"Выберите учётную запись."); return
            with db_connect() as con:
                row=con.execute("SELECT username,is_enabled FROM operator_accounts WHERE id=?",(aid,)).fetchone()
                if not row:return
                if row["username"].casefold()==(os.environ.get("USERNAME") or os.environ.get("USER") or "local").casefold():
                    QMessageBox.warning(dlg,APP_NAME,"Нельзя переключать состояние учётной записи, совпадающей с текущим пользователем Windows."); return
                set_account_enabled(con,aid,not bool(row["is_enabled"]))
                record_security_event(con,"roles_changed",role="admin",permission="roles",details=f"Изменено состояние учётной записи {row['username']}")
            refresh()
        def remove_account():
            aid=selected_id()
            if aid is None: QMessageBox.information(dlg,APP_NAME,"Выберите учётную запись."); return
            with db_connect() as con:
                count=con.execute("SELECT COUNT(*) FROM operator_accounts WHERE is_enabled=1 AND role='admin'").fetchone()[0]
                row=con.execute("SELECT username,role,is_enabled FROM operator_accounts WHERE id=?",(aid,)).fetchone()
                if row and row["role"]=="admin" and row["is_enabled"] and count<=1:
                    QMessageBox.warning(dlg,APP_NAME,"Нельзя удалить последнюю включённую учётную запись администратора."); return
            if QMessageBox.question(dlg,APP_NAME,"Удалить выбранную учётную запись?",QMessageBox.Yes|QMessageBox.No,QMessageBox.No)!=QMessageBox.Yes:return
            with db_connect() as con:
                delete_account(con,aid)
                record_security_event(con,"roles_changed",role="admin",permission="roles",details=f"Удалена учётная запись ID {aid}")
            refresh()
        def edit_permissions():
            aid=selected_id()
            if aid is None: QMessageBox.information(dlg,APP_NAME,"Выберите учётную запись."); return
            with db_connect() as con: current=get_account_permissions(con,aid)
            perm_dlg=QDialog(dlg); perm_dlg.setWindowTitle("Индивидуальные права оператора"); perm_layout=QVBoxLayout(perm_dlg)
            perm_layout.addWidget(QLabel("Отметьте разрешённые операции. Настройка заменяет стандартный набор прав роли только для выбранной учётной записи."))
            labels={"view":"Просмотр сотрудников и пропусков","add":"Добавление сотрудников","edit":"Изменение карточек","delete":"Перемещение в корзину","prepare":"Подготовка карты для Magicard HUB","issue":"Выдача и перевыпуск пропуска","export":"Экспорт и отчёты","restore":"Восстановление из корзины","purge":"Окончательное удаление","roles":"Управление ролями и операторами","backup":"Резервное копирование и восстановление"}
            checks={}
            for key in PERMISSIONS:
                cb=QCheckBox(labels.get(key,key)); cb.setChecked(key in current); checks[key]=cb; perm_layout.addWidget(cb)
            perm_buttons=QDialogButtonBox(QDialogButtonBox.Save|QDialogButtonBox.Cancel); perm_layout.addWidget(perm_buttons)
            perm_buttons.accepted.connect(perm_dlg.accept); perm_buttons.rejected.connect(perm_dlg.reject)
            if perm_dlg.exec()!=QDialog.Accepted:return
            selected={key for key,cb in checks.items() if cb.isChecked()}
            try:
                with db_connect() as con:
                    update_account_permissions(con,aid,selected)
                    record_security_event(con,"roles_changed",permission="roles",details=f"Изменены индивидуальные права учётной записи ID {aid}")
                QMessageBox.information(dlg,APP_NAME,"Индивидуальные права сохранены.")
            except Exception as exc: QMessageBox.warning(dlg,APP_NAME,str(exc))
        buttons=QHBoxLayout()
        for label,fn in (("＋ Добавить",add_account),("Права доступа…",edit_permissions),("Изменить PIN",change_pin),("Включить / отключить",toggle_account),("Удалить",remove_account)):
            b=QPushButton(label); b.clicked.connect(fn); buttons.addWidget(b)
        layout.addLayout(buttons)
        close=QDialogButtonBox(QDialogButtonBox.Close); close.rejected.connect(dlg.reject); close.accepted.connect(dlg.accept); layout.addWidget(close)
        refresh(); dlg.exec()

    def _load_report_snapshot(self, start_date, end_date, warning_days):
        with db_connect() as con:
            records = [dict(r) for r in con.execute(
                "SELECT id,full_name,organization,department,position,badge_number,valid_until "
                "FROM employees WHERE COALESCE(is_deleted,0)=0 ORDER BY full_name COLLATE NOCASE").fetchall()]
            movements = [dict(r) for r in con.execute(
                "SELECT created_at,full_name,badge_number,event_type,note,operator "
                "FROM card_movements ORDER BY id").fetchall()]
        return build_report(records, movements, start_date, end_date, warning_days=warning_days)

    def show_expiry_notification_if_due(self):
        """Show one local expiry digest per calendar day when explicitly enabled."""
        if not self.settings.get("expiry_notifications_enabled", False):
            return
        today = datetime.now().date()
        today_key = today.isoformat()
        if self.settings.get("last_expiry_notification_date") == today_key:
            return
        try:
            warning_days = max(1, min(365, int(self.settings.get("expiry_warning_days", 30) or 30)))
        except (TypeError, ValueError):
            warning_days = 30
        try:
            with db_connect() as con:
                records = [dict(r) for r in con.execute(
                    "SELECT full_name,badge_number,valid_until FROM employees "
                    "WHERE COALESCE(is_deleted,0)=0").fetchall()]
            digest = notification_digest(records, today=today, warning_days=warning_days)
            self.settings["last_expiry_notification_date"] = today_key
            save_json(SETTINGS_FILE, self.settings)
            if digest["total"] == 0:
                return
            message = (f"Просрочено: {digest['expired']}\n"
                       f"Истекают в ближайшие {warning_days} дн.: {digest['soon']}\n"
                       f"Даты требуют проверки: {digest['unknown']}\n\n"
                       "Подробный список доступен в «Ещё → Центр отчётов и уведомлений». ")
            if digest["expired"]:
                QMessageBox.warning(self, APP_NAME, "Требуется внимание к срокам пропусков.\n\n" + message)
            else:
                QMessageBox.information(self, APP_NAME, "Напоминание о сроках действия пропусков.\n\n" + message)
        except Exception:
            logging.exception("Daily expiry notification failed")

    def open_reports_center(self):
        if not self.require_role("export"):
            return
        today = datetime.now().date()
        dlg = QDialog(self)
        dlg.setWindowTitle("Центр отчётов и уведомлений")
        dlg.resize(980, 700)
        root = QVBoxLayout(dlg)
        heading = QLabel("Отчёты, контроль сроков и журнал операций")
        heading.setStyleSheet("font-size:15pt;font-weight:700;color:#F2C230;")
        root.addWidget(heading)

        filters = QGridLayout()
        filters.addWidget(QLabel("Начало периода движения:"), 0, 0)
        date_from = QDateEdit(QDate(today.year, today.month, 1)); date_from.setCalendarPopup(True)
        filters.addWidget(date_from, 0, 1)
        filters.addWidget(QLabel("Конец периода:"), 0, 2)
        date_to = QDateEdit(QDate(today.year, today.month, today.day)); date_to.setCalendarPopup(True)
        filters.addWidget(date_to, 0, 3)
        filters.addWidget(QLabel("Предупреждать за, дней:"), 1, 0)
        warning_days = QSpinBox(); warning_days.setRange(1, 365)
        try:
            saved_warning_days = max(1, min(365, int(self.settings.get("expiry_warning_days", 30) or 30)))
        except (TypeError, ValueError):
            saved_warning_days = 30
        warning_days.setValue(saved_warning_days)
        filters.addWidget(warning_days, 1, 1)
        daily_check = QCheckBox("Показывать ежедневное напоминание при запуске")
        daily_check.setChecked(bool(self.settings.get("expiry_notifications_enabled", False)))
        filters.addWidget(daily_check, 1, 2, 1, 2)
        root.addLayout(filters)

        summary = QLabel(); summary.setWordWrap(True); summary.setObjectName("muted")
        root.addWidget(summary)
        table = QTableWidget(0, 7)
        table.setHorizontalHeaderLabels(["ФИО", "Организация", "Подразделение", "Должность", "№ пропуска", "Действует до", "Состояние"])
        table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        table.setSelectionBehavior(QAbstractItemView.SelectRows)
        table.setAlternatingRowColors(True)
        table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        table.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        for col in range(2, 7):
            table.horizontalHeader().setSectionResizeMode(col, QHeaderView.ResizeToContents)
        root.addWidget(table, 1)
        event_summary = QLabel(); event_summary.setWordWrap(True)
        root.addWidget(event_summary)
        report_state = {"report": None}

        def refresh_report():
            start = date(date_from.date().year(), date_from.date().month(), date_from.date().day())
            end = date(date_to.date().year(), date_to.date().month(), date_to.date().day())
            if start > end:
                QMessageBox.warning(dlg, APP_NAME, "Начало периода не может быть позже окончания.")
                return False
            try:
                report = self._load_report_snapshot(start, end, warning_days.value())
            except Exception as exc:
                QMessageBox.critical(dlg, APP_NAME, f"Не удалось построить отчёт:\n{exc}")
                return False
            report_state["report"] = report
            counts = report["expiry_counts"]
            summary.setText(
                f"Сотрудников: {report['employee_count']}   |   Действуют: {counts.get('valid',0)}   |   "
                f"Просрочены: {counts.get('expired',0)}   |   Истекают: {counts.get('soon',0)}   |   "
                f"Без срока: {counts.get('none',0)}   |   Неизвестная дата: {counts.get('unknown',0)}\n"
                f"В очереди внимания: {len(report['expiry_rows'])}"
            )
            rows = report["expiry_rows"]
            table.setRowCount(len(rows))
            for row_idx, rec in enumerate(rows):
                values = [rec["full_name"], rec["organization"], rec["department"], rec["position"], rec["badge_number"], rec["valid_until"], rec["status_label"]]
                for col, value in enumerate(values):
                    item = QTableWidgetItem(str(value or ""))
                    if rec["status"] == "expired": item.setForeground(QColor("#ff5b5b"))
                    elif rec["status"] == "soon": item.setForeground(QColor("#F2C230"))
                    table.setItem(row_idx, col, item)
            events = report["event_counts"]
            event_summary.setText("Операции за выбранный период: " + (" · ".join(f"{k}: {v}" for k, v in events.items()) if events else "нет записей"))
            return True

        def save_notification_settings():
            self.settings["expiry_notifications_enabled"] = daily_check.isChecked()
            self.settings["expiry_warning_days"] = warning_days.value()
            if not daily_check.isChecked():
                self.settings.pop("last_expiry_notification_date", None)
            save_json(SETTINGS_FILE, self.settings)
            QMessageBox.information(dlg, APP_NAME,
                "Настройки напоминаний сохранены. Напоминание появляется не чаще одного раза в день при запуске программы."
                if daily_check.isChecked() else "Ежедневные напоминания отключены.")

        def export_report(kind):
            report = report_state.get("report")
            if report is None:
                return
            defaults = {"csv": "PS_Сводный_отчёт.csv", "xlsx": "PS_Сводный_отчёт.xlsx", "pdf": "PS_Сводный_отчёт.pdf"}
            filters_text = {"csv": "CSV (*.csv)", "xlsx": "Excel (*.xlsx)", "pdf": "PDF (*.pdf)"}
            target, _ = QFileDialog.getSaveFileName(dlg, "Сохранить отчёт", str(OUTPUT_DIR / defaults[kind]), filters_text[kind])
            if not target:
                return
            try:
                if kind == "csv": export_full_report_csv(target, report)
                elif kind == "xlsx": export_full_report_xlsx(target, report)
                else: export_full_report_pdf(target, report, RESOURCES_DIR / "DejaVuSans.ttf")
                with db_connect() as con:
                    write_audit(con, f"report_export_{kind}", details=f"Период {report['period_start']} — {report['period_end']}; сроков внимания {len(report['expiry_rows'])}; движений {len(report['movements'])}")
                QMessageBox.information(dlg, APP_NAME, f"Отчёт сохранён:\n{target}")
            except Exception as exc:
                logging.exception("Report export failed")
                QMessageBox.critical(dlg, APP_NAME, f"Не удалось сохранить отчёт:\n{exc}")

        buttons = QHBoxLayout()
        refresh = QPushButton("Обновить данные"); refresh.clicked.connect(refresh_report); buttons.addWidget(refresh)
        save_settings = QPushButton("Сохранить напоминания"); save_settings.clicked.connect(save_notification_settings); buttons.addWidget(save_settings)
        buttons.addStretch()
        for label, kind in (("CSV", "csv"), ("Excel", "xlsx"), ("PDF", "pdf")):
            button = QPushButton(f"Экспорт {label}…"); button.clicked.connect(lambda checked=False, k=kind: export_report(k)); buttons.addWidget(button)
        close = QPushButton("Закрыть"); close.clicked.connect(dlg.accept); buttons.addWidget(close)
        root.addLayout(buttons)
        refresh_report()
        dlg.exec()

    def open_access_control_center(self):
        """Unified local view of card validity, blocks and status-change history."""
        if not self.require_role("view"):
            return
        dlg = QDialog(self)
        dlg.setWindowTitle("Единый центр управления доступом")
        dlg.resize(1120, 700)
        root = QVBoxLayout(dlg)
        intro = QLabel("Контроль состояния пропусков и локальная история изменений. Это учётная панель PS PassMaster PRO, а не подключение к контроллеру дверей.")
        intro.setWordWrap(True)
        root.addWidget(intro)
        stats = QLabel()
        stats.setStyleSheet("font-size:11pt;font-weight:700;color:#F2C230;padding:6px;")
        root.addWidget(stats)
        filters = QHBoxLayout()
        search = QLineEdit(); search.setPlaceholderText("Поиск: ФИО, организация, подразделение или номер…")
        state_filter = QComboBox()
        state_filter.addItem(ACCESS_STATE_LABELS["all"], "all")
        for key in ("expired", "blocked", "soon", "check", "active", "other"):
            state_filter.addItem(ACCESS_STATE_LABELS[key], key)
        filters.addWidget(search, 1); filters.addWidget(state_filter)
        root.addLayout(filters)
        tabs = QTabWidget(); root.addWidget(tabs, 1)
        access_table = QTableWidget(0, 7)
        access_table.setHorizontalHeaderLabels(["ФИО", "Организация", "Подразделение", "№ пропуска", "Действует до", "Статус", "Причина блокировки"])
        access_table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        access_table.setSelectionBehavior(QAbstractItemView.SelectRows)
        access_table.setAlternatingRowColors(True)
        for col in (0, 1, 2, 6): access_table.horizontalHeader().setSectionResizeMode(col, QHeaderView.Stretch)
        for col in (3, 4, 5): access_table.horizontalHeader().setSectionResizeMode(col, QHeaderView.ResizeToContents)
        tabs.addTab(access_table, "Состояние пропусков")
        history_table = QTableWidget(0, 8)
        history_table.setHorizontalHeaderLabels(["Дата и время", "Сотрудник", "№ пропуска", "Было", "Стало", "Причина", "Оператор", "Источник"])
        history_table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        history_table.setSelectionBehavior(QAbstractItemView.SelectRows)
        history_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
        history_table.horizontalHeader().setStretchLastSection(True)
        tabs.addTab(history_table, "История блокировок и статусов")
        current_rows = []

        def refresh():
            nonlocal current_rows
            with db_connect() as con:
                records = [dict(r) for r in con.execute(
                    "SELECT id,full_name,organization,department,position,badge_number,valid_until,card_type,card_status,blocked_reason,is_deleted "
                    "FROM employees WHERE COALESCE(is_deleted,0)=0 ORDER BY full_name COLLATE NOCASE").fetchall()]
                history = list_status_history(con, limit=2000)
            overview = build_access_overview(records, query=search.text(), state=state_filter.currentData())
            current_rows = overview["rows"]
            counts = overview["counts"]
            stats.setText(
                f"Всего: {overview['total']}   ·   Действуют: {counts['active']}   ·   Истекают: {counts['soon']}   ·   Просрочены: {counts['expired']}   ·   Заблокированы: {counts['blocked']}   ·   Проверить дату: {counts['check']}   ·   Показано: {overview['visible_count']}"
            )
            access_table.setRowCount(len(current_rows))
            colors = {"expired": "#ff5b5b", "blocked": "#ff5b5b", "soon": "#F2C230", "check": "#e5a04b"}
            for r, item in enumerate(current_rows):
                values = [item.get("full_name", ""), item.get("organization", ""), item.get("department", ""),
                          item.get("badge_number", ""), item.get("valid_until", ""), item.get("access_label", ""), item.get("blocked_reason", "")]
                for c, value in enumerate(values):
                    cell = QTableWidgetItem(str(value or ""))
                    if c == 5 and item["access_state"] in colors:
                        cell.setForeground(QColor(colors[item["access_state"]]))
                    access_table.setItem(r, c, cell)
            history_table.setRowCount(len(history))
            for r, item in enumerate(history):
                values = [item.get("changed_at", ""), item.get("full_name", ""), item.get("badge_number", ""),
                          item.get("old_status", ""), item.get("new_status", ""), item.get("reason", ""),
                          item.get("operator", ""), item.get("source", "")]
                for c, value in enumerate(values):
                    cell = QTableWidgetItem(str(value or ""))
                    if item.get("new_status") in ("Заблокирован", "Истёк", "Аннулирован"):
                        cell.setForeground(QColor("#ff5b5b"))
                    history_table.setItem(r, c, cell)

        search.textChanged.connect(refresh)
        state_filter.currentIndexChanged.connect(refresh)
        buttons = QHBoxLayout()
        export = QPushButton("Экспорт текущего списка CSV…")
        def export_rows():
            target, _ = QFileDialog.getSaveFileName(dlg, "Экспорт списка состояния пропусков", str(OUTPUT_DIR / "access_overview.csv"), "CSV (*.csv)")
            if not target: return
            try:
                with open(target, "w", newline="", encoding="utf-8-sig") as stream:
                    writer = csv.writer(stream, delimiter=";")
                    writer.writerow(["ФИО", "Организация", "Подразделение", "Должность", "Номер", "Действует до", "Состояние", "Причина блокировки"])
                    for item in current_rows:
                        writer.writerow([item.get(k, "") for k in ("full_name", "organization", "department", "position", "badge_number", "valid_until", "access_label", "blocked_reason")])
                with db_connect() as con:
                    write_audit(con, "access_center_export", details=f"Строк: {len(current_rows)}")
                QMessageBox.information(dlg, APP_NAME, f"Список сохранён:\n{target}")
            except Exception as exc:
                logging.exception("Access overview export failed")
                QMessageBox.critical(dlg, APP_NAME, f"Не удалось сохранить список:\n{exc}")
        export.clicked.connect(export_rows); buttons.addWidget(export)
        reload_button = QPushButton("Обновить"); reload_button.clicked.connect(refresh); buttons.addWidget(reload_button)
        buttons.addStretch()
        close = QPushButton("Закрыть"); close.clicked.connect(dlg.accept); buttons.addWidget(close)
        root.addLayout(buttons)
        refresh()
        dlg.exec()

    def open_analytics_dashboard(self):
        """Offline dashboard: counts, expiry queue and photo completeness."""
        if not self.require_role("export"):
            return
        with db_connect() as con:
            records = [dict(r) for r in con.execute(
                "SELECT id,full_name,department,badge_number,valid_until,photo_path,card_type,card_status "
                "FROM employees WHERE COALESCE(is_deleted,0)=0 ORDER BY full_name COLLATE NOCASE").fetchall()]
            issued = con.execute("SELECT COUNT(*) FROM issuance_history").fetchone()[0]
        metrics = summarize_employees(records)
        queue = expiry_rows(records)
        dlg = QDialog(self); dlg.setWindowTitle("Аналитика и автоматизация"); dlg.resize(820, 590)
        root = QVBoxLayout(dlg)
        heading = QLabel("Сводка по сотрудникам и пропускам")
        heading.setStyleSheet("font-size:15pt;font-weight:700;color:#F2C230;")
        root.addWidget(heading)
        grid = QGridLayout(); root.addLayout(grid)
        cards = [("Сотрудников", metrics["total"]), ("Действуют", metrics["valid"]),
                 ("Истекли", metrics["expired"]), ("Истекают ≤ 30 дней", metrics["expiring_30"]),
                 ("Заблокированы", metrics["blocked"]), ("Выдано по истории", issued),
                 ("С фотографией", metrics["photos"]), ("Без фотографии", metrics["missing_photos"])]
        for i, (label, value) in enumerate(cards):
            box = QGroupBox(label); box_layout = QVBoxLayout(box)
            number = QLabel(str(value)); number.setAlignment(Qt.AlignCenter)
            number.setStyleSheet("font-size:21pt;font-weight:700;color:#F2C230;")
            box_layout.addWidget(number); grid.addWidget(box, i // 4, i % 4)
        type_line = " · ".join(f"{k}: {v}" for k, v in sorted(metrics["by_type"].items())) or "Нет данных"
        type_label = QLabel("Типы пропусков: " + type_line); type_label.setWordWrap(True); root.addWidget(type_label)
        root.addWidget(QLabel("Очередь внимания: просроченные, истекающие и даты, требующие проверки"))
        table = QTableWidget(0, 5); table.setHorizontalHeaderLabels(["ФИО", "Подразделение", "№ пропуска", "Действует до", "Состояние"])
        table.setEditTriggers(QAbstractItemView.NoEditTriggers); table.setSelectionBehavior(QAbstractItemView.SelectRows)
        table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        table.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        for c in (2,3,4): table.horizontalHeader().setSectionResizeMode(c, QHeaderView.ResizeToContents)
        table.setRowCount(len(queue))
        labels = {"expired":"Просрочен", "soon":"Истекает", "unknown":"Проверить дату"}
        for row_idx, rec in enumerate(queue):
            values = [rec["full_name"], rec["department"], rec["badge_number"], rec["valid_until"], labels[rec["status"]]]
            for col, value in enumerate(values):
                item = QTableWidgetItem(str(value or ""))
                if rec["status"] == "expired": item.setForeground(QColor("#ff5b5b"))
                elif rec["status"] == "soon": item.setForeground(QColor("#F2C230"))
                table.setItem(row_idx, col, item)
        root.addWidget(table, 1)
        footer = QHBoxLayout()
        export = QPushButton("Экспорт очереди в CSV…")
        def export_queue():
            target, _ = QFileDialog.getSaveFileName(dlg, "Сохранить очередь сроков", str(OUTPUT_DIR / "expiry_queue.csv"), "CSV (*.csv)")
            if not target: return
            try:
                with open(target, "w", newline="", encoding="utf-8-sig") as f:
                    writer = csv.writer(f, delimiter=";"); writer.writerow(["ФИО", "Подразделение", "Номер", "Действует до", "Статус"])
                    for rec in queue: writer.writerow([rec["full_name"], rec["department"], rec["badge_number"], rec["valid_until"], labels[rec["status"]]])
                with db_connect() as con: write_audit(con, "analytics_expiry_export", details=f"Строк: {len(queue)}")
                QMessageBox.information(dlg, APP_NAME, f"Отчёт сохранён: {target}")
            except Exception as exc: QMessageBox.critical(dlg, APP_NAME, f"Не удалось сохранить отчёт: {exc}")
        export.clicked.connect(export_queue); footer.addWidget(export); footer.addStretch()
        close = QPushButton("Закрыть"); close.clicked.connect(dlg.accept); footer.addWidget(close); root.addLayout(footer)
        dlg.exec()

    def open_backup_center(self):
        dlg = QDialog(self); dlg.setWindowTitle("Резервное копирование и восстановление"); dlg.resize(700, 520)
        layout = QVBoxLayout(dlg)
        intro = QLabel("Полная резервная копия включает базу сотрудников, настройки, историю, фотографии сотрудников и шаблоны. Всё обрабатывается локально, без отправки в интернет.")
        intro.setWordWrap(True); layout.addWidget(intro)
        status = QLabel("Проверяйте резервные копии перед переносом на другой компьютер."); status.setWordWrap(True); layout.addWidget(status)

        create_plain = QPushButton("Создать полную резервную копию…")
        def make_plain():
            target, _ = QFileDialog.getSaveFileName(dlg, "Сохранить полную резервную копию", str(BACKUP_DIR / ("PSPassMaster_full_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".pspmbackup")), "Резервная копия (*.pspmbackup)")
            if not target: return
            try:
                result = create_full_backup(DB_FILE, APP_DATA_DIR, ROOT / "Templates", target)
                prune_full_backups(BACKUP_DIR, keep=int(self.settings.get("backup_retention", 14)))
                with db_connect() as con: write_audit(con, "full_backup_created", details=f"Файлов: {result['files']}; фото: {result['photos']}")
                status.setText(f"Копия создана: {target}\nФайлов: {result['files']}; фотографий: {result['photos']}; отсутствующих фото: {result['missing_photos']}.")
                QMessageBox.information(dlg, APP_NAME, status.text())
            except Exception as exc:
                logging.exception("Full backup creation failed"); QMessageBox.critical(dlg, APP_NAME, f"Не удалось создать копию: {exc}")
        create_plain.clicked.connect(make_plain); layout.addWidget(create_plain)

        create_enc = QPushButton("Создать полную копию с паролем…")
        def make_encrypted_full():
            password, ok = QInputDialog.getText(dlg, "Защита резервной копии", "Пароль (не менее 10 символов):", QLineEdit.Password)
            if not ok: return
            confirm, ok = QInputDialog.getText(dlg, "Подтверждение пароля", "Повторите пароль:", QLineEdit.Password)
            if not ok: return
            if password != confirm:
                QMessageBox.warning(dlg, APP_NAME, "Пароли не совпадают."); return
            target, _ = QFileDialog.getSaveFileName(dlg, "Сохранить зашифрованную полную копию", str(BACKUP_DIR / ("PSPassMaster_full_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".pspmenc")), "Зашифрованная копия (*.pspmenc)")
            if not target: return
            try:
                result = create_full_backup(DB_FILE, APP_DATA_DIR, ROOT / "Templates", target, passphrase=password)
                with db_connect() as con: write_audit(con, "full_backup_encrypted_created", details=Path(target).name)
                status.setText(f"Зашифрованная полная копия создана: {target}. Пароль не сохраняется в программе.")
                QMessageBox.information(dlg, APP_NAME, status.text() + "\nОбязательно сохраните пароль отдельно.")
            except Exception as exc:
                logging.exception("Encrypted full backup failed"); QMessageBox.critical(dlg, APP_NAME, f"Не удалось создать копию: {exc}")
        create_enc.clicked.connect(make_encrypted_full); layout.addWidget(create_enc)

        verify_btn = QPushButton("Проверить резервную копию…")
        def verify_backup_ui():
            source, _ = QFileDialog.getOpenFileName(dlg, "Выберите резервную копию", str(BACKUP_DIR), "Резервные копии (*.pspmbackup *.pspmenc)")
            if not source: return
            password = None
            if source.lower().endswith(".pspmenc"):
                password, ok = QInputDialog.getText(dlg, "Пароль копии", "Введите пароль:", QLineEdit.Password)
                if not ok: return
            try:
                result = inspect_full_backup(source, passphrase=password, temp_dir=TEMP_DIR)
                status.setText(f"Копия исправна. Создана: {result['created_at']}; файлов: {result['files']}; фотографий: {result['photos']}; отсутствующих фото: {result['missing_photos']}.")
                QMessageBox.information(dlg, APP_NAME, status.text())
            except Exception as exc:
                logging.exception("Full backup verification failed"); QMessageBox.critical(dlg, APP_NAME, f"Проверка не пройдена: {exc}")
        verify_btn.clicked.connect(verify_backup_ui); layout.addWidget(verify_btn)

        audit_btn = QPushButton("Проверить все резервные копии в хранилище")
        def audit_backup_storage_ui():
            try:
                report = audit_backup_directory(BACKUP_DIR, temp_dir=TEMP_DIR)
                lines = [f"Найдено архивов: {report['total']}",
                         f"Исправных: {report['valid']}",
                         f"Требуют парольной проверки: {report['password_required']}",
                         f"С ошибками: {report['invalid']}"]
                for item in report["items"][:20]:
                    mark = "OK" if item["status"] == "ok" else "ПАРОЛЬ" if item["status"] == "password_required" else "ОШИБКА"
                    lines.append(f"{mark} — {item['name']} ({item['size'] // 1024} КБ): {item['details']}")
                if report["total"] > 20:
                    lines.append(f"…и ещё {report['total'] - 20} архивов.")
                text = "\n".join(lines)
                status.setText(f"Проверка хранилища завершена: исправных {report['valid']}; требуют пароль {report['password_required']}; ошибок {report['invalid']}.")
                QMessageBox.information(dlg, APP_NAME, text)
            except Exception as exc:
                logging.exception("Backup storage audit failed")
                QMessageBox.critical(dlg, APP_NAME, f"Не удалось проверить хранилище: {exc}")
        audit_btn.clicked.connect(audit_backup_storage_ui); layout.addWidget(audit_btn)

        rehearse_btn = QPushButton("Проверить готовность восстановления…")
        def rehearse_backup_ui():
            source, _ = QFileDialog.getOpenFileName(dlg, "Выберите резервную копию для репетиции восстановления", str(BACKUP_DIR), "Резервные копии (*.pspmbackup *.pspmenc)")
            if not source: return
            password = None
            if source.lower().endswith(".pspmenc"):
                password, ok = QInputDialog.getText(dlg, "Пароль копии", "Введите пароль:", QLineEdit.Password)
                if not ok: return
            try:
                result = rehearse_backup_restore(source, passphrase=password, temp_dir=TEMP_DIR)
                status.setText(f"Репетиция восстановления пройдена: сотрудников {result['employees']}; фото {result['photos_verified']}.")
                QMessageBox.information(dlg, APP_NAME,
                    f"ГОТОВО К ВОССТАНОВЛЕНИЮ\nДата копии: {result['created_at']}\n"
                    f"Файлов проверено: {result['files_verified']}\nСотрудников в базе: {result['employees']}\n"
                    f"Фотографий проверено: {result['photos_verified']}\nОтсутствующих фото: {result['missing_photos']}\n\n"
                    "Рабочая база и текущие файлы не изменялись.")
            except Exception as exc:
                logging.exception("Backup restore rehearsal failed")
                QMessageBox.critical(dlg, APP_NAME, f"Репетиция восстановления не пройдена: {exc}")
        rehearse_btn.clicked.connect(rehearse_backup_ui); layout.addWidget(rehearse_btn)

        restore_btn = QPushButton("Восстановить полную копию…")
        def restore_full_ui():
            if not self.require_role("restore"): return
            source, _ = QFileDialog.getOpenFileName(dlg, "Выберите полную резервную копию", str(BACKUP_DIR), "Резервные копии (*.pspmbackup *.pspmenc)")
            if not source: return
            password = None
            if source.lower().endswith(".pspmenc"):
                password, ok = QInputDialog.getText(dlg, "Пароль копии", "Введите пароль:", QLineEdit.Password)
                if not ok: return
            try:
                verified = inspect_full_backup(source, passphrase=password, temp_dir=TEMP_DIR)
                answer = QMessageBox.question(dlg, APP_NAME,
                    f"Копия прошла проверку.\nДата: {verified['created_at']}\nФайлов: {verified['files']}\nФотографий: {verified['photos']}\n\nТекущие данные будут сохранены в отдельную полную копию, затем заменены данными из архива. Продолжить?",
                    QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
                if answer != QMessageBox.Yes: return
                safety = BACKUP_DIR / ("PSPassMaster_full_before_restore_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".pspmbackup")
                create_full_backup(DB_FILE, APP_DATA_DIR, ROOT / "Templates", safety)
                result = restore_full_backup(source, DB_FILE, APP_DATA_DIR, ROOT / "Templates", passphrase=password)
                with db_connect() as con: write_audit(con, "full_backup_restored", details=Path(source).name)
                self.settings = load_json(SETTINGS_FILE, {}); self.photo_edits = load_json(PHOTO_EDITS_FILE, {})
                self.refresh_employees(); self.refresh_history()
                status.setText(f"Восстановление завершено. Фотографий восстановлено: {result['photos_restored']}; отсутствующих в исходной копии: {result['missing_photos']}.")
                QMessageBox.information(dlg, APP_NAME, status.text() + f"\nПредыдущие данные сохранены: {safety}")
            except Exception as exc:
                logging.exception("Full backup restore failed"); QMessageBox.critical(dlg, APP_NAME, f"Не удалось восстановить копию: {exc}")
        restore_btn.clicked.connect(restore_full_ui); layout.addWidget(restore_btn)

        policy = QGroupBox("Автоматическое резервирование")
        policy_layout = QFormLayout(policy)
        auto = QCheckBox("Создавать полную копию при первом запуске за день")
        auto.setChecked(bool(self.settings.get("full_backup_auto", True)))
        retention = QSpinBox(); retention.setRange(1, 100); retention.setValue(int(self.settings.get("backup_retention", 14)))
        policy_layout.addRow(auto); policy_layout.addRow("Хранить последних копий:", retention)
        save_policy = QPushButton("Сохранить настройки резервирования")
        def save_backup_policy():
            self.settings["full_backup_auto"] = auto.isChecked(); self.settings["backup_retention"] = retention.value()
            save_json(SETTINGS_FILE, self.settings)
            prune_full_backups(BACKUP_DIR, keep=retention.value())
            QMessageBox.information(dlg, APP_NAME, "Настройки резервирования сохранены.")
        save_policy.clicked.connect(save_backup_policy); policy_layout.addRow(save_policy); layout.addWidget(policy)
        close = QDialogButtonBox(QDialogButtonBox.Close); close.rejected.connect(dlg.reject); close.accepted.connect(dlg.accept); layout.addWidget(close)
        dlg.exec()

    def open_security_center(self):
        if not self.require_role("roles"): return
        dlg = QDialog(self); dlg.setWindowTitle("Центр безопасности"); dlg.resize(620, 420)
        layout = QVBoxLayout(dlg)
        info = QLabel("Защита локальной базы, резервных копий и доступа. Данные не отправляются в интернет.")
        info.setWordWrap(True); layout.addWidget(info)
        status = QLabel("Проверка целостности базы ещё не запускалась."); layout.addWidget(status)
        check = QPushButton("Проверить целостность базы")
        def run_check():
            try:
                result = integrity_check(DB_FILE)
                status.setText("База данных исправна (integrity_check: ok)." if result == "ok" else f"Обнаружена проблема: {result}")
                with db_connect() as con: write_audit(con, "database_integrity_check", details=f"Результат: {result}")
            except Exception as exc: status.setText(f"Ошибка проверки: {exc}")
        check.clicked.connect(run_check); layout.addWidget(check)
        account_box = QGroupBox("Управление блокировками операторов")
        account_layout = QHBoxLayout(account_box)
        account_pick = QComboBox()
        with db_connect() as con:
            locked_accounts = con.execute("SELECT id,username,display_name,locked_until FROM operator_accounts ORDER BY username COLLATE NOCASE").fetchall()
        for account in locked_accounts:
            label = f"{account['username']} — {account['display_name']}"
            if account['locked_until']:
                label += f" (блокировка: {account['locked_until']})"
            account_pick.addItem(label, int(account['id']))
        account_layout.addWidget(account_pick, 1)
        unlock_btn = QPushButton("Снять блокировку")
        def unlock_selected_account():
            account_id = account_pick.currentData()
            if account_id is None:
                QMessageBox.information(dlg, APP_NAME, "Нет учётных записей для управления.")
                return
            try:
                with db_connect() as con:
                    unlock_account(con, account_id)
                    row = con.execute("SELECT username FROM operator_accounts WHERE id=?", (account_id,)).fetchone()
                    record_security_event(con, "critical_action_confirmed", actor=(self.current_operator or {}).get("username", "admin"),
                                          permission="roles", details=f"Снята блокировка учётной записи: {row['username'] if row else account_id}")
                QMessageBox.information(dlg, APP_NAME, "Блокировка снята. PIN и права доступа не изменены.")
                account_pick.setCurrentIndex(-1)
            except Exception as exc:
                QMessageBox.warning(dlg, APP_NAME, f"Не удалось снять блокировку: {exc}")
        unlock_btn.clicked.connect(unlock_selected_account)
        account_layout.addWidget(unlock_btn)
        layout.addWidget(account_box)

        events_btn = QPushButton("Журнал входов и событий безопасности…")
        def show_security_events():
            if not self.require_role("roles"):
                return
            with db_connect() as con:
                summary = summarize_security_events(con, 30)
                rows = list_security_events(con, 1000)
            dlg_events = QDialog(dlg); dlg_events.setWindowTitle("Журнал безопасности"); dlg_events.resize(1050, 560)
            vbox = QVBoxLayout(dlg_events)
            vbox.addWidget(QLabel(
                f"За 30 дней: успешные входы — {summary['login_success']}; "
                f"неудачные — {summary['login_failure']}; отказано в доступе — {summary['access_denied']}; "
                f"попытки во время блокировки — {summary['lockout_active']}."
            ))
            grid = QTableWidget(len(rows), 6)
            grid.setHorizontalHeaderLabels(["Дата и время", "Событие", "Пользователь Windows", "Роль", "Разрешение", "Подробности"])
            grid.setEditTriggers(QAbstractItemView.NoEditTriggers); grid.setSelectionBehavior(QAbstractItemView.SelectRows)
            grid.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents); grid.horizontalHeader().setStretchLastSection(True)
            for ri, row in enumerate(rows):
                for ci, value in enumerate(row): grid.setItem(ri, ci, QTableWidgetItem(str(value or "")))
            vbox.addWidget(grid)
            export_events = QPushButton("Экспортировать журнал в CSV…")
            def export_security_csv():
                target, _ = QFileDialog.getSaveFileName(dlg_events, "Экспорт журнала безопасности",
                    str(OUTPUT_DIR / f"security_events_{datetime.now():%Y%m%d_%H%M%S}.csv"), "CSV (*.csv)")
                if not target: return
                if not target.lower().endswith(".csv"): target += ".csv"
                try:
                    with open(target, "w", newline="", encoding="utf-8-sig") as stream:
                        writer = csv.writer(stream, delimiter=';')
                        writer.writerow(["Дата и время", "Событие", "Оператор", "Роль", "Разрешение", "Подробности"])
                        writer.writerows([tuple(row) for row in rows])
                    QMessageBox.information(dlg_events, APP_NAME, f"Журнал сохранён:\n{target}")
                except Exception as exc:
                    QMessageBox.critical(dlg_events, APP_NAME, f"Не удалось экспортировать журнал: {exc}")
            export_events.clicked.connect(export_security_csv); vbox.addWidget(export_events)
            close_events = QDialogButtonBox(QDialogButtonBox.Close); close_events.rejected.connect(dlg_events.reject); close_events.accepted.connect(dlg_events.accept); vbox.addWidget(close_events)
            dlg_events.exec()
        events_btn.clicked.connect(show_security_events); layout.addWidget(events_btn)
        backup = QPushButton("Создать зашифрованную резервную копию…")
        def make_encrypted():
            password, ok = QInputDialog.getText(dlg, "Шифрование копии", "Пароль (не менее 10 символов):", QLineEdit.Password)
            if not ok: return
            confirm, ok = QInputDialog.getText(dlg, "Подтверждение", "Повторите пароль:", QLineEdit.Password)
            if not ok: return
            if password != confirm:
                QMessageBox.warning(dlg, APP_NAME, "Пароли не совпадают."); return
            target, _ = QFileDialog.getSaveFileName(dlg, "Сохранить зашифрованную копию", str(BACKUP_DIR / ("PSPassMaster_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".pspmenc")), "Зашифрованная копия (*.pspmenc)")
            if not target: return
            try:
                plain = create_database_backup("before_encrypted_export")
                encrypt_backup(plain, target, password)
                with db_connect() as con: write_audit(con, "encrypted_backup_created", details=Path(target).name)
                QMessageBox.information(dlg, APP_NAME, "Зашифрованная резервная копия создана. Сохраните пароль отдельно: восстановить копию без него невозможно.")
            except Exception as exc: QMessageBox.critical(dlg, APP_NAME, f"Не удалось создать копию: {exc}")
        backup.clicked.connect(make_encrypted); layout.addWidget(backup)
        restore_enc = QPushButton("Восстановить из зашифрованной копии…")
        def restore_encrypted():
            source, _ = QFileDialog.getOpenFileName(dlg, "Выберите зашифрованную копию", str(BACKUP_DIR), "PS PassMaster encrypted (*.pspmenc)")
            if not source: return
            password, ok = QInputDialog.getText(dlg, "Расшифровка копии", "Введите пароль резервной копии:", QLineEdit.Password)
            if not ok: return
            try:
                staging = TEMP_DIR / "decrypted_restore.sqlite3"
                decrypt_backup(source, staging, password)
                check = sqlite3.connect(staging)
                integrity = check.execute("PRAGMA integrity_check").fetchone()
                tables = {r[0] for r in check.execute("SELECT name FROM sqlite_master WHERE type='table'")}
                check.close()
                if not integrity or integrity[0] != "ok" or "employees" not in tables:
                    raise ValueError("Расшифрованная база не прошла проверку или не содержит таблицу сотрудников.")
                if QMessageBox.question(dlg, APP_NAME, "Текущая база будет сохранена отдельно, затем заменена выбранной копией. Продолжить?", QMessageBox.Yes | QMessageBox.No, QMessageBox.No) != QMessageBox.Yes:
                    staging.unlink(missing_ok=True); return
                create_database_backup("before_encrypted_restore")
                staging.replace(DB_FILE)
                with db_connect() as con: write_audit(con, "encrypted_backup_restored", details=Path(source).name)
                self.refresh_employees(); self.refresh_history()
                QMessageBox.information(dlg, APP_NAME, "Зашифрованная копия восстановлена. Предыдущая база сохранена в Backups.")
            except Exception as exc:
                logging.exception("Encrypted restore failed")
                QMessageBox.critical(dlg, APP_NAME, f"Не удалось восстановить копию: {exc}")
            finally:
                (TEMP_DIR / "decrypted_restore.sqlite3").unlink(missing_ok=True)
        restore_enc.clicked.connect(restore_encrypted); layout.addWidget(restore_enc)
        close = QDialogButtonBox(QDialogButtonBox.Close); close.rejected.connect(dlg.reject); close.accepted.connect(dlg.accept); layout.addWidget(close)
        dlg.exec()

    def configure_roles(self):
        with db_connect() as con: configured = con.execute("SELECT 1 FROM app_roles LIMIT 1").fetchone()
        if configured and not self.require_role("roles"): return
        admin_pin, ok = QInputDialog.getText(self, "Роли и безопасность", "Задайте новый PIN администратора (4–64 цифры):", QLineEdit.Password)
        if not ok: return
        try: admin_record = hash_pin(admin_pin)
        except ValueError as exc:
            QMessageBox.warning(self, APP_NAME, str(exc)); return
        operator_pin, ok = QInputDialog.getText(self, "Роли и безопасность", "PIN оператора (оставьте пустым, чтобы не создавать роль оператора):", QLineEdit.Password)
        if not ok: return
        operator_record = None
        if operator_pin.strip():
            try: operator_record = hash_pin(operator_pin)
            except ValueError as exc:
                QMessageBox.warning(self, APP_NAME, str(exc)); return
        create_database_backup("before_roles")
        with db_connect() as con:
            for role, record in (("admin", admin_record), ("operator", operator_record)):
                if record:
                    con.execute("INSERT INTO app_roles(role,pin_salt,pin_hash,iterations,updated_at) VALUES(?,?,?,?,?) ON CONFLICT(role) DO UPDATE SET pin_salt=excluded.pin_salt,pin_hash=excluded.pin_hash,iterations=excluded.iterations,updated_at=excluded.updated_at",
                                (role, record["salt"], record["hash"], record["iterations"], datetime.now().isoformat(timespec="seconds")))
                elif role == "operator": con.execute("DELETE FROM app_roles WHERE role='operator'")
            write_audit(con, "roles_updated", details="Настроены локальные PIN-роли")
            record_security_event(con, "roles_changed", role="admin", permission="roles", details="Обновлены PIN-роли")
        QMessageBox.information(self, APP_NAME, "PIN сохранены в виде PBKDF2-хеша с солью. Исходные PIN не записываются в базу.")

    def show_employee_trash(self):
        if not self.require_role("restore"): return
        dlg = QDialog(self); dlg.setWindowTitle("Корзина сотрудников"); dlg.resize(760, 420)
        layout = QVBoxLayout(dlg); table = QTableWidget(0, 4)
        table.setHorizontalHeaderLabels(["ID", "ФИО", "№ пропуска", "Дата удаления"])
        table.setSelectionBehavior(QAbstractItemView.SelectRows); table.setSelectionMode(QAbstractItemView.SingleSelection)
        with db_connect() as con: rows = con.execute("SELECT * FROM employees WHERE is_deleted=1 ORDER BY deleted_at DESC").fetchall()
        table.setRowCount(len(rows))
        for i, rec in enumerate(rows):
            for j, key in enumerate(("id", "full_name", "badge_number", "deleted_at")): table.setItem(i, j, QTableWidgetItem(str(rec[key] or "")))
        layout.addWidget(table)
        buttons = QHBoxLayout(); restore = QPushButton("Восстановить"); purge = QPushButton("Удалить навсегда"); close = QPushButton("Закрыть")
        buttons.addWidget(restore); buttons.addWidget(purge); buttons.addStretch(); buttons.addWidget(close); layout.addLayout(buttons)
        def selected():
            row = table.currentRow(); return int(table.item(row, 0).text()) if row >= 0 and table.item(row,0) else None
        def restore_row():
            emp_id = selected()
            if emp_id is None: return
            create_database_backup("before_restore")
            with db_connect() as con:
                rec = con.execute("SELECT * FROM employees WHERE id=?", (emp_id,)).fetchone()
                con.execute("UPDATE employees SET is_deleted=0,deleted_at='',updated_at=? WHERE id=?", (datetime.now().isoformat(timespec="seconds"), emp_id))
                write_audit(con, "employee_restored", emp_id, rec["full_name"], rec["badge_number"], "Сотрудник восстановлен из корзины")
            dlg.accept(); self.refresh_employees()
        def purge_row():
            emp_id = selected()
            if emp_id is None: return
            if not self.require_role("purge"): return
            if QMessageBox.question(dlg, APP_NAME, "Удалить запись навсегда? История выдачи останется, фотография не удаляется.") != QMessageBox.Yes: return
            create_database_backup("before_purge")
            with db_connect() as con:
                rec = con.execute("SELECT * FROM employees WHERE id=?", (emp_id,)).fetchone()
                write_audit(con, "employee_purged", emp_id, rec["full_name"], rec["badge_number"], "Запись удалена окончательно; история сохранена")
                con.execute("DELETE FROM employees WHERE id=?", (emp_id,))
            dlg.accept(); self.refresh_employees()
        restore.clicked.connect(restore_row); purge.clicked.connect(purge_row); close.clicked.connect(dlg.reject)
        dlg.exec()

    def export_expiry_report(self):
        if not self.require_role("export"): return
        path, _ = QFileDialog.getSaveFileName(self, "Отчёт по срокам пропусков", str(OUTPUT_DIR / "badge_expiry_report.csv"), "CSV (*.csv)")
        if not path: return
        with db_connect() as con: rows = con.execute("SELECT full_name,organization,department,position,badge_number,issue_date,valid_until FROM employees WHERE is_deleted=0 ORDER BY full_name COLLATE NOCASE").fetchall()
        with open(path, "w", newline="", encoding="utf-8-sig") as stream:
            writer = csv.writer(stream, delimiter=";"); writer.writerow(["ФИО","Организация","Отдел","Должность","Номер пропуска","Дата выдачи","Действует до","Статус"])
            for rec in rows:
                status = expiry_status(rec["valid_until"])
                writer.writerow([rec["full_name"],rec["organization"],rec["department"],rec["position"],rec["badge_number"],rec["issue_date"],rec["valid_until"],{"expired":"Истёк","soon":"Истекает в 30 дней","valid":"Действует","none":"Без срока","unknown":"Неизвестная дата"}.get(status,status)])
        QMessageBox.information(self, APP_NAME, f"Отчёт сохранён:\n{path}")

    def selected_employee_ids(self):
        rows = sorted({idx.row() for idx in self.employee_table.selectionModel().selectedRows()})
        ids = []
        for row in rows:
            item = self.employee_table.item(row, 0)
            if item:
                try: ids.append(int(item.text()))
                except ValueError: pass
        return ids

    def bulk_update_expiry(self):
        if not self.require_role("edit"): return
        ids = self.selected_employee_ids()
        if not ids:
            QMessageBox.information(self, APP_NAME, "Выберите одного или нескольких сотрудников в реестре."); return
        value, ok = QInputDialog.getText(self, "Массовое изменение срока",
            f"Новая дата действия для {len(ids)} сотрудников (ДД.ММ.ГГГГ). Оставьте пустым для срока без ограничения:")
        if not ok: return
        try: _, normalized = bulk_expiry_update([{"id": x} for x in ids], value.strip())
        except ValueError as exc:
            QMessageBox.warning(self, APP_NAME, str(exc)); return
        if QMessageBox.question(self, APP_NAME, f"Изменить срок действия у {len(ids)} сотрудников на «{normalized or 'без срока'}»?") != QMessageBox.Yes: return
        create_database_backup("before_bulk_expiry")
        operator = os.environ.get("USERNAME") or os.environ.get("USER") or "local"
        with db_connect() as con:
            con.execute("BEGIN IMMEDIATE")
            updated = renew_many(con, ids, normalized, operator)
            write_audit(con, "bulk_expiry_updated", details=f"Массовое продление срока: изменено {updated} из {len(ids)}; дата={normalized or 'без срока'}", operator=operator)
        self.refresh_employees()
        QMessageBox.information(self, APP_NAME, f"Продлено сотрудников: {updated} из {len(ids)}. История изменений сохранена для каждой карточки.")

    def export_expiry_report_excel(self):
        if not self.require_role("export"): return
        path, _ = QFileDialog.getSaveFileName(self, "Excel-отчёт по срокам пропусков", str(OUTPUT_DIR / "badge_expiry_report.xlsx"), "Excel (*.xlsx)")
        if not path: return
        try:
            from openpyxl import Workbook
            from openpyxl.styles import Font, PatternFill, Alignment
            with db_connect() as con:
                rows = con.execute("SELECT full_name,organization,department,position,badge_number,issue_date,valid_until FROM employees WHERE is_deleted=0 ORDER BY full_name COLLATE NOCASE").fetchall()
            wb = Workbook(); ws = wb.active; ws.title = "Сроки пропусков"
            ws.append(["ФИО", "Организация", "Подразделение", "Должность", "Номер пропуска", "Дата выдачи", "Действует до", "Статус"])
            for cell in ws[1]:
                cell.font = Font(bold=True, color="252A30"); cell.fill = PatternFill("solid", fgColor="F2C230"); cell.alignment = Alignment(horizontal="center")
            for rec in rows:
                ws.append([rec["full_name"], rec["organization"], rec["department"], rec["position"], rec["badge_number"], rec["issue_date"], rec["valid_until"], expiry_label(rec["valid_until"])])
                status = expiry_status(rec["valid_until"])
                color = "FF7777" if status == "expired" else "F2C230" if status == "soon" else None
                if color:
                    for cell in ws[ws.max_row]: cell.fill = PatternFill("solid", fgColor=color)
            ws.freeze_panes = "A2"; ws.auto_filter.ref = ws.dimensions
            for col, width in zip("ABCDEFGH", [30,24,22,24,18,16,16,30]): ws.column_dimensions[col].width = width
            wb.save(path)
            QMessageBox.information(self, APP_NAME, f"Excel-отчёт сохранён. Записей: {len(rows)}\n{path}")
        except Exception as exc:
            logging.exception("Expiry XLSX report failed")
            QMessageBox.critical(self, APP_NAME, f"Не удалось создать Excel-отчёт:\n{exc}")

    def prepare_employee_photo(self):
        if not self.require_role("prepare"): return None
        rec = self.get_employee()
        if not rec: return None
        src = Path(rec.get("photo_path", ""))
        if not src.is_file():
            QMessageBox.warning(self, APP_NAME, "У выбранного сотрудника не задана доступная фотография. Откройте «Изменить» и выберите файл."); return None
        try:
            focus = detect_face_focus(str(src))
            fx, fy = (focus[0], focus[1]) if focus else (0.5, 0.5)
            crop_key = str(rec["id"])
            saved_crops = self.settings.setdefault("employee_photo_crops", {})
            saved_crop = saved_crops.get(crop_key, {})
            if saved_crop and saved_crop.get("photo_path") == str(src):
                fx = float(saved_crop.get("focus_x", fx))
                fy = float(saved_crop.get("focus_y", fy))
                initial_zoom = float(saved_crop.get("zoom", 1.08 if focus else 1.0))
            else:
                initial_zoom = 1.08 if focus else 1.0
            crop_dlg = EmployeeCropDialog(str(src), self, fx, fy, initial_zoom, self.photo_edits.get(str(src), {}))
            if crop_dlg.exec() != QDialog.Accepted:
                return None
            fx, fy, zoom = crop_dlg.values()
            saved_crops[crop_key] = {"focus_x": fx, "focus_y": fy, "zoom": zoom, "photo_path": str(src)}
            save_json(SETTINGS_FILE, self.settings)
            im, w, h = make_photo(str(src), VNUKOVO_PHOTO_WIDTH_CM, VNUKOVO_PHOTO_HEIGHT_CM, 300, "Заполнить (обрезать)", fx, fy, zoom, self.photo_edits.get(str(src), {}))
            # Crop is saved locally; full-template preview is shown after template processing.\n            safe = re.sub(r"[^A-Za-zА-Яа-яЁё0-9_-]+", "_", rec.get("badge_number") or str(rec["id"])).strip("_") or str(rec["id"])
            target = OUTPUT_DIR / f"Vnukovo_{safe}_21_8x29mm_300dpi.jpg"
            im.save(target, "JPEG", quality=96, dpi=(300,300), optimize=True)
            im.close()
            with db_connect() as con: con.execute("UPDATE employees SET updated_at=? WHERE id=?", (datetime.now().isoformat(timespec="seconds"), rec["id"]))
            self.magicard_file.setText(str(target))
            self.statusBar().showMessage(f"Фото подготовлено: {target}", 7000)
            return target
        except Exception as e:
            logging.exception("Employee photo prepare failed")
            QMessageBox.critical(self, APP_NAME, f"Не удалось подготовить фотографию:\n{e}"); return None

    def managed_templates_dir(self):
        preferred = APP_DATA_DIR / "Templates"
        try:
            preferred.mkdir(parents=True, exist_ok=True)
            probe = preferred / ".write_test"
            probe.write_text("ok", encoding="utf-8")
            probe.unlink(missing_ok=True)
            return preferred
        except OSError:
            base = Path(os.environ.get("LOCALAPPDATA") or Path.home() / ".local" / "share")
            fallback = base / "PSPassMasterPRO" / "Templates"
            fallback.mkdir(parents=True, exist_ok=True)
            return fallback

    def auto_import_hub_template(self, silent=False):
        current = Path(self.settings.get("hub_card_template", "")) if self.settings.get("hub_card_template") else None
        bundled_default = ROOT / "Templates" / "New Card Design.card"
        if current and current.is_file():
            try:
                inspect_template(current)
                # If the only configured template is the bundled sample, still look
                # for the user's actual HUB design on first launch after upgrading.
                if current.resolve() != bundled_default.resolve():
                    self.template_path_label.setText(str(current))
                    return current
            except Exception:
                logging.warning("Configured HUB template is invalid: %s", current)
        hub_exe = self.settings.get("hub_path") or discover_hub_executable()
        candidates = discover_hub_templates(hub_exe)
        if not candidates:
            fallback = ROOT / "Templates" / "New Card Design.card"
            if fallback.is_file():
                self.settings["hub_card_template"] = str(fallback)
                save_json(SETTINGS_FILE, self.settings)
                self.template_path_label.setText(str(fallback))
            if not silent:
                QMessageBox.information(self, APP_NAME, "Подходящий шаблон HUB автоматически не найден. Выберите файл .card вручную.")
            return fallback if fallback.is_file() else None
        source = candidates[0]
        try:
            managed = copy_hub_template(source, self.managed_templates_dir())
            self.settings["hub_card_template"] = str(managed)
            self.settings["hub_template_source"] = str(source)
            save_json(SETTINGS_FILE, self.settings)
            self.template_path_label.setText(str(managed))
            self.statusBar().showMessage(f"Шаблон Magicard HUB подключён: {managed.name}", 8000)
            if not silent:
                QMessageBox.information(self, APP_NAME, f"Шаблон HUB найден и скопирован в PS PassMaster PRO.\n\nИсточник: {source}\nКопия: {managed}\n\nОригинал не изменён.")
            return managed
        except Exception as exc:
            logging.exception("Automatic HUB template import failed")
            if not silent:
                QMessageBox.warning(self, APP_NAME, f"Не удалось подключить шаблон автоматически:\n{exc}\nВыберите файл .card вручную.")
            return None

    def refresh_hub_template(self):
        self.settings.pop("hub_card_template", None)
        result = self.auto_import_hub_template(silent=False)
        if result:
            self.template_path_label.setText(str(result))

    def choose_hub_card_template(self):
        default = self.settings.get("hub_card_template", str(ROOT / "Templates" / "New Card Design.card"))
        path, _ = QFileDialog.getOpenFileName(
            self, "Выберите шаблон Magicard HUB", default,
            "Шаблоны Magicard HUB (*.card);;Все файлы (*)"
        )
        if path:
            try:
                inspect_template(path)
                managed = copy_hub_template(path, self.managed_templates_dir())
                self.settings["hub_card_template"] = str(managed)
                self.settings["hub_template_source"] = str(Path(path).resolve())
                save_json(SETTINGS_FILE, self.settings)
                return str(managed)
            except Exception as exc:
                QMessageBox.warning(self, APP_NAME, f"Выбранный шаблон не прошёл проверку:\n{exc}")
                return None
        return None

    def export_employee_to_hub_template(self):
        """Create a copy of the supplied HUB design with only its portrait replaced."""
        rec = self.get_employee()
        if not rec:
            return
        if not can_issue(rec.get("card_status", "Активен")):
            QMessageBox.warning(self, APP_NAME, f"Выпуск запрещён: пропуск имеет статус «{rec.get('card_status')}». Сначала измените статус на «Активен».")
            return
        if not can_issue(rec.get("card_status", "Активен")):
            QMessageBox.warning(self, APP_NAME, f"Выпуск запрещён: пропуск имеет статус «{rec.get('card_status')}». Сначала измените статус на «Активен».")
            return
        template = get_profile_template(self.settings.get("hub_template_profiles", {}), rec.get("organization", ""), rec.get("card_type", ""))
        if not template:
            template = self.settings.get("hub_card_template", str(ROOT / "Templates" / "New Card Design.card"))
        if not Path(template).is_file():
            template = self.choose_hub_card_template()
        if not template:
            return
        preflight = preflight_employee_card(rec, template)
        preflight_text = format_preflight(preflight)
        if preflight["errors"]:
            QMessageBox.warning(self, "Выпуск карточки запрещён", preflight_text)
            return
        if preflight["warnings"] and QMessageBox.question(
            self, "Проверка карточки: есть предупреждения",
            preflight_text + "\n\nПродолжить после проверки предупреждений?"
        ) != QMessageBox.Yes:
            return
        # Validate the original portrait before creating the HUB-sized output image.
        photo_qc = assess_photo(rec.get("photo_path") or "")
        if not photo_qc["ok"]:
            QMessageBox.warning(self, APP_NAME, "Проверка качества фото не пройдена:\n\n" + format_report(photo_qc))
            return
        if photo_qc["warnings"] and QMessageBox.question(self, APP_NAME, "Есть замечания к фотографии:\n\n" + format_report(photo_qc) + "\n\nПродолжить создание .card?") != QMessageBox.Yes:
            return
        # Prepare/crop the portrait using the same interactive editor as the normal workflow.
        photo_path = self.prepare_employee_photo()
        if not photo_path:
            return
        try:
            design = load_card(template)
            personalize_design(design, rec)
            elements = design.get("front", {}).get("elements", [])
            photo_elements = [find_photo_element(design)]
            if len(photo_elements) != 1:
                raise ValueError(
                    "В выбранном шаблоне не найден ровно один объект фотографии с именем images.jpg. "
                    "Исходный шаблон не изменён."
                )
            from PIL import ImageOps
            import base64
            element = photo_elements[0]
            def image_data(size, fmt):
                with Image.open(photo_path) as source:
                    im = ImageOps.exif_transpose(source).convert("RGB")
                    im = ImageOps.fit(im, size, method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))
                    buf = io.BytesIO()
                    if fmt == "JPEG":
                        im.save(buf, format="JPEG", quality=96, optimize=True)
                        mime = "image/jpeg"
                    else:
                        im.save(buf, format="PNG", optimize=True)
                        mime = "image/png"
                    im.close()
                return f"data:{mime};base64," + base64.b64encode(buf.getvalue()).decode("ascii")
            # Keep the HUB element geometry, id, transforms, position, and every other
            # object/property exactly as in the supplied template. Only its image payloads change.
            def existing_size(field, fallback):
                try:
                    raw = element.get(field, "")
                    encoded = raw.split(",", 1)[1]
                    with Image.open(io.BytesIO(base64.b64decode(encoded))) as old:
                        return old.size
                except Exception:
                    return fallback
            element["base64image"] = image_data(existing_size("base64image", (109, 145)), "JPEG")
            element["originalBase64Image"] = image_data(existing_size("originalBase64Image", (122, 162)), "PNG")
            # Refresh the embedded HUB thumbnail in the same portrait rectangle so
            # the design preview does not continue showing the previous person's face.
            try:
                preview_raw = design.get("about", {}).get("preview", "")
                if preview_raw.startswith("data:image/"):
                    preview = Image.open(io.BytesIO(base64.b64decode(preview_raw.split(",", 1)[1]))).convert("RGBA")
                    canvas_w = float(design.get("front", {}).get("background", {}).get("width", 1013))
                    canvas_h = float(design.get("front", {}).get("background", {}).get("height", 642))
                    sx, sy = preview.width / canvas_w, preview.height / canvas_h
                    px = round(float(element.get("x", 0)) * sx)
                    py = round(float(element.get("y", 0)) * sy)
                    pw = max(1, round(float(element.get("width", 0)) * sx))
                    ph = max(1, round(float(element.get("height", 0)) * sy))
                    with Image.open(photo_path) as source:
                        thumb = ImageOps.fit(ImageOps.exif_transpose(source).convert("RGB"), (pw, ph), method=Image.Resampling.LANCZOS)
                    preview.paste(thumb, (px, py))
                    preview_buf = io.BytesIO()
                    preview.save(preview_buf, format="PNG", optimize=True)
                    design["about"]["preview"] = "data:image/png;base64," + base64.b64encode(preview_buf.getvalue()).decode("ascii")
            except Exception:
                logging.exception("Could not refresh embedded HUB thumbnail; card image payload is still updated")
            safe = re.sub(r"[^A-Za-zА-Яа-яЁё0-9_-]+", "_", rec.get("badge_number") or str(rec["id"])).strip("_") or str(rec["id"])
            default_out = OUTPUT_DIR / f"Magicard_{safe}.card"
            out_path, _ = QFileDialog.getSaveFileName(
                self, "Сохранить копию шаблона с новой фотографией", str(default_out),
                "Шаблон Magicard HUB (*.card);;Все файлы (*)"
            )
            if not out_path:
                return
            if not out_path.lower().endswith(".card"):
                out_path += ".card"
            out_file = Path(out_path)
            if out_file.resolve() == Path(template).resolve():
                raise ValueError("Нельзя перезаписывать исходный шаблон. Выберите другое имя файла.")
            # Guard against selecting the original template as output.
            if out_file.resolve() == Path(template).resolve():
                raise ValueError("Нельзя перезаписывать исходный шаблон. Выберите другое имя файла.")
            atomic_write_card(out_file, design)
            try:
                preservation = compare_template_preservation(template, out_file)
                if not preservation["structure_unchanged"]:
                    raise ValueError("Контроль сохранности шаблона не пройден.")
            except Exception:
                out_file.unlink(missing_ok=True)
                raise
            # Keep a local issuance/export audit trail and a pre-change DB snapshot.
            create_database_backup("before_issue_log")
            with db_connect() as con:
                created_at = datetime.now().isoformat(timespec="seconds")
                previous_issue = con.execute("SELECT 1 FROM issuance_history WHERE employee_id=? LIMIT 1", (rec["id"],)).fetchone()
                con.execute(
                    "INSERT INTO issuance_history(employee_id,full_name,badge_number,issue_date,card_file,created_at) VALUES(?,?,?,?,?,?)",
                    (rec["id"], rec.get("full_name",""), rec.get("badge_number",""),
                     rec.get("issue_date",""), str(Path(out_path).resolve()), created_at)
                )
                event = "hub_card_reprinted" if previous_issue else "hub_card_issued"
                write_audit(con, event, rec["id"], rec.get("full_name",""),
                            rec.get("badge_number",""), f"Подготовлен .card файл: {Path(out_path).name}")
            # Verify the saved file is valid JSON and the image payload is readable.
            check = json.loads(Path(out_path).read_text(encoding="utf-8"))
            check_element = next(e for e in check["front"]["elements"] if e.get("fileName", "").lower() == "images.jpg")
            raw = base64.b64decode(check_element["base64image"].split(",", 1)[1])
            with Image.open(io.BytesIO(raw)) as verify_image:
                verify_image.verify()
            self.statusBar().showMessage(f"Шаблон с новой фотографией сохранён: {out_path}", 10000)
            self.refresh_history()
            QMessageBox.information(
                self, "Шаблон Magicard HUB готов",
                "Новая фотография подставлена на место старой. Остальные элементы шаблона не изменялись.\n\n"
                f"Файл: {out_path}\n\nОткройте этот .card в Magicard HUB. Исходный шаблон сохранён без изменений."
            )
            try:
                open_card_file(out_path, self.hub_path.text().strip() or None)
            except Exception:
                logging.exception("Could not open exported card via HUB association")
                if os.name == "nt": subprocess.Popen(["explorer", "/select,", str(Path(out_path).resolve())])
        except Exception as e:
            logging.exception("HUB card template photo replacement failed")
            QMessageBox.critical(self, APP_NAME, f"Не удалось заменить фотографию в шаблоне:\n{e}")

    def export_selected_employees_to_hub(self):
        """Batch-create per-employee .card copies; the original template is never modified."""
        rows = sorted({idx.row() for idx in self.employee_table.selectionModel().selectedRows()})
        if not rows:
            QMessageBox.information(self, APP_NAME, "Выберите одного или нескольких сотрудников в таблице.")
            return
        try:
            validate_batch_size(len(rows))
        except ValueError as exc:
            QMessageBox.warning(self, APP_NAME, str(exc))
            return
        template = self.settings.get("hub_card_template", str(ROOT / "Templates" / "New Card Design.card"))
        if not Path(template).is_file():
            template = self.choose_hub_card_template()
        if not template:
            return
        out_dir = QFileDialog.getExistingDirectory(self, "Папка для пакетных .card", str(OUTPUT_DIR))
        if not out_dir:
            return
        import base64
        try:
            create_database_backup("before_batch_issue_log")
            created, skipped, preflight_warnings = [], [], []
            for row in rows:
                item = self.employee_table.item(row, 0)
                rec_id = int(item.text()) if item else None
                with db_connect() as con:
                    record = con.execute("SELECT * FROM employees WHERE id=?", (rec_id,)).fetchone()
                if not record:
                    skipped.append(f"ID {rec_id}: запись не найдена"); continue
                rec = dict(record)
                employee_template = get_profile_template(self.settings.get("hub_template_profiles", {}), rec.get("organization", ""), rec.get("card_type", "")) or template
                preflight = preflight_employee_card(rec, employee_template)
                if preflight["errors"]:
                    skipped.append(f"{rec.get('full_name')}: предварительная проверка не пройдена — {'; '.join(preflight['errors'])}")
                    continue
                if preflight["warnings"]:
                    preflight_warnings.append(f"{rec.get('full_name')}: {'; '.join(preflight['warnings'])}")
                try:
                    base_design = load_card(employee_template)
                    personalize_design(base_design, rec)
                    find_photo_element(base_design)
                except Exception as template_exc:
                    skipped.append(f"{rec.get('full_name')}: шаблон организации/типа не прошёл проверку — {template_exc}")
                    continue
                # Validate each selected employee independently before generating files.
                block_reason = issuance_block_reason(rec)
                if block_reason:
                    skipped.append(f"{rec.get('full_name') or 'ID ' + str(rec_id)}: {block_reason}")
                    continue
                source_path = Path(rec.get("photo_path") or "")
                if not source_path.is_file():
                    skipped.append(f"{rec.get('full_name')}: нет фотографии"); continue
                qc = assess_photo(source_path)
                if not qc["ok"]:
                    skipped.append(f"{rec.get('full_name')}: контроль качества не пройден — {'; '.join(qc['errors'])}")
                    continue
                face = detect_face_focus(str(source_path))
                if qc["warnings"] and qc.get("face_detected") is False:
                    skipped.append(f"{rec.get('full_name')}: лицо не обнаружено; пропуск для ручной проверки")
                    continue
                fx, fy = (face[0], face[1]) if face else (0.5, 0.5)
                im, _, _ = make_photo(str(source_path), VNUKOVO_PHOTO_WIDTH_CM, VNUKOVO_PHOTO_HEIGHT_CM,
                                      300, "Заполнить (обрезать)", fx, fy, 1.08 if face else 1.0)
                safe = re.sub(r"[^A-Za-zА-Яа-яЁё0-9_-]+", "_", rec.get("badge_number") or str(rec_id)).strip("_") or str(rec_id)
                jpg_path = Path(out_dir) / f"Vnukovo_{safe}_21_8x29mm_300dpi.jpg"
                im.save(jpg_path, "JPEG", quality=96, dpi=(300,300), optimize=True)
                im.close()
                design = json.loads(json.dumps(base_design))
                element = next(e for e in design["front"]["elements"]
                               if e.get("name") == "Image" and e.get("fileName", "").lower() == "images.jpg")
                def encoded_image(size, fmt):
                    with Image.open(jpg_path) as img:
                        fitted = ImageOps.fit(img.convert("RGB"), size, method=Image.Resampling.LANCZOS)
                        buf = io.BytesIO()
                        if fmt == "JPEG":
                            fitted.save(buf, "JPEG", quality=96, optimize=True); mime = "image/jpeg"
                        else:
                            fitted.save(buf, "PNG", optimize=True); mime = "image/png"
                        fitted.close()
                    return f"data:{mime};base64," + base64.b64encode(buf.getvalue()).decode("ascii")
                def old_size(field, fallback):
                    try:
                        raw = element[field].split(",",1)[1]
                        with Image.open(io.BytesIO(base64.b64decode(raw))) as old_img: return old_img.size
                    except Exception: return fallback
                element["base64image"] = encoded_image(old_size("base64image",(109,145)), "JPEG")
                element["originalBase64Image"] = encoded_image(old_size("originalBase64Image",(122,162)), "PNG")
                preview_raw = design.get("about",{}).get("preview","")
                if preview_raw.startswith("data:image/"):
                    preview = Image.open(io.BytesIO(base64.b64decode(preview_raw.split(",",1)[1]))).convert("RGBA")
                    bg = design.get("front",{}).get("background",{})
                    sx = preview.width / float(bg.get("width",1013)); sy = preview.height / float(bg.get("height",642))
                    px, py = round(float(element.get("x",0))*sx), round(float(element.get("y",0))*sy)
                    pw, ph = max(1,round(float(element.get("width",0))*sx)), max(1,round(float(element.get("height",0))*sy))
                    with Image.open(jpg_path) as photo:
                        thumb = ImageOps.fit(photo.convert("RGB"),(pw,ph),method=Image.Resampling.LANCZOS)
                    preview.paste(thumb,(px,py)); pb=io.BytesIO(); preview.save(pb,"PNG",optimize=True)
                    design["about"]["preview"]="data:image/png;base64,"+base64.b64encode(pb.getvalue()).decode("ascii")
                    preview.close(); thumb.close()
                card_path = Path(out_dir) / f"Magicard_{safe}.card"
                if card_path.exists():
                    jpg_path.unlink(missing_ok=True)
                    skipped.append(f"{rec.get('full_name')}: файл уже существует ({card_path.name}); перезапись запрещена")
                    continue
                atomic_write_card(card_path, design)
                try:
                    compare_template_preservation(employee_template, card_path)
                except Exception:
                    card_path.unlink(missing_ok=True)
                    raise
                with db_connect() as con:
                    created_at = datetime.now().isoformat(timespec="seconds")
                    con.execute("INSERT INTO issuance_history(employee_id,full_name,badge_number,issue_date,card_file,created_at) VALUES(?,?,?,?,?,?)",
                        (rec_id,rec.get("full_name",""),rec.get("badge_number",""),rec.get("issue_date",""),str(card_path.resolve()),created_at))
                    write_audit(con, "hub_card_prepared", rec_id, rec.get("full_name",""),
                                rec.get("badge_number",""), f"Пакетно подготовлен .card: {card_path.name}")
                created.append(card_path.name)
            self.statusBar().showMessage(f"Пакет HUB: создано {len(created)}, пропущено {len(skipped)}",10000)
            summary = f"Создано файлов .card: {len(created)}"
            # Persist a batch manifest next to the card files for traceability.
            manifest_path = Path(out_dir) / f"PS_PassMaster_batch_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
            with manifest_path.open("w", encoding="utf-8-sig", newline="") as manifest_file:
                writer = csv.writer(manifest_file, delimiter=";")
                writer.writerow(["Результат", "Файл / сотрудник", "Подробности"])
                for filename in created:
                    writer.writerow(["Создано", filename, "Файл .card создан и проверен"])
                for reason in skipped:
                    writer.writerow(["Пропущено", reason.split(":", 1)[0], reason])
                for warning in preflight_warnings:
                    writer.writerow(["Предупреждение", warning.split(":", 1)[0], warning])
            summary += f"\nОтчёт пакета: {manifest_path.name}"
            if preflight_warnings:
                summary += f"\nПредупреждения проверки: {len(preflight_warnings)} (подробности в отчёте)"
            if skipped: summary += "\n\nПропущено:\n" + "\n".join(skipped[:20])
            summary += "\n\nИсходный шаблон не изменён. Проверьте каждый файл в Magicard HUB перед печатью."
            self.refresh_history()
            QMessageBox.information(self,"Пакетная подготовка HUB",summary)
            if os.name == "nt": subprocess.Popen(["explorer",str(Path(out_dir).resolve())])
        except Exception as e:
            logging.exception("Batch HUB card export failed")
            QMessageBox.critical(self,APP_NAME,f"Ошибка пакетной подготовки .card:\\n{e}")

    def send_employee_to_hub(self):
        target = self.prepare_employee_photo()
        if not target: return
        hub = self.hub_path.text().strip()
        try:
            if hub and Path(hub).is_file():
                if os.name == "nt": os.startfile(hub)
                else: subprocess.Popen([hub])
            if os.name == "nt": subprocess.Popen(["explorer", "/select,", str(target.resolve())])
            else: self.open_output()
            QMessageBox.information(self, APP_NAME, "Фото подготовлено и выделено в Проводнике. Magicard HUB открыт. Откройте запись сотрудника и выберите выделенный JPG штатной командой добавления фотографии. Автоматическое заполнение поля изображения зависит от интерфейса/версии HUB и без подтверждённого API не выполняется.")
        except Exception as e: QMessageBox.warning(self, APP_NAME, f"Не удалось открыть HUB или Проводник:\n{e}")

    def export_employee_card(self):
        rec = self.get_employee()
        if not rec: return
        # Превью формата ID-1 (CR80), не является готовым макетом для печати без калибровки HUB.
        W, H = 1016, 638
        card = Image.new("RGB", (W,H), "white"); d = ImageDraw.Draw(card)
        d.rounded_rectangle((8,8,W-8,H-8), radius=26, outline="#2879ad", width=8)
        def font(sz, bold=False):
            candidates = ["C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]
            for f in candidates:
                try: return ImageFont.truetype(f, sz)
                except Exception: pass
            return ImageFont.load_default()
        d.text((420,28), f"№ {rec.get('badge_number') or '—'}", fill="#111111", font=font(34,True))
        d.text((45,80), "ВНУКОВО", fill="#202a35", font=font(56,True))
        d.text((48,143), "Международный аэропорт", fill="#394553", font=font(24))
        d.text((55,235), rec.get("organization") or "Организация", fill="#111111", font=font(34,True))
        name = rec.get("full_name") or "ФИО сотрудника"
        d.text((55,320), name[:32], fill="#111111", font=font(34,True))
        d.text((55,382), (rec.get("department") or "")[:36], fill="#111111", font=font(30,True))
        d.text((55,438), (rec.get("position") or "Должность")[:36], fill="#222222", font=font(28))
        d.text((55,510), f"выдан {rec.get('issue_date') or '—'}", fill="#111111", font=font(24,True))
        d.text((270,560), f"действует до {rec.get('valid_until') or '—'}", fill="#111111", font=font(26,True))
        photo_path = Path(rec.get("photo_path") or "")
        prepared = OUTPUT_DIR / f"Vnukovo_{re.sub(r'[^A-Za-zА-Яа-яЁё0-9_-]+','_', rec.get('badge_number') or str(rec['id']))}.jpg"
        if prepared.is_file(): photo_path = prepared
        if photo_path.is_file():
            try:
                with Image.open(photo_path) as ph:
                    ph = ImageOps.fit(ph.convert("RGB"), (240,300))
                    card.paste(ph, (710,45))
            except Exception: pass
        else:
            d.rectangle((710,45,950,345), outline="#8796a5", width=3)
            d.text((755,180), "ФОТО", fill="#8796a5", font=font(28,True))
        path, _ = QFileDialog.getSaveFileName(self, "Сохранить предварительный макет карты", str(OUTPUT_DIR / "card_preview.png"), "PNG (*.png)")
        if path:
            card.save(path, "PNG", dpi=(300,300))
            QMessageBox.information(self, APP_NAME, "Предварительный макет сохранён. Перед печатью нужно сверить поля, логотипы и физический размер с шаблоном Magicard HUB.")

    def import_employees_csv(self):
        path, _ = QFileDialog.getOpenFileName(self, "Импорт сотрудников CSV", "", "CSV (*.csv)")
        if not path: return
        count = 0
        duplicates = 0
        try:
            create_database_backup("before_csv_import")
            with open(path, "r", encoding="utf-8-sig", newline="") as f:
                sample = f.read(2048); f.seek(0)
                try: dialect = csv.Sniffer().sniff(sample, delimiters=";,\t")
                except csv.Error: dialect = csv.excel; dialect.delimiter = ";"
                reader = csv.DictReader(f, dialect=dialect)
                aliases = {
                    "фио":"full_name", "fullname":"full_name", "организация":"organization", "organization":"organization",
                    "подразделение":"department", "department":"department", "должность":"position", "position":"position",
                    "номер пропуска":"badge_number", "badgenumber":"badge_number", "дата выдачи":"issue_date", "issuedate":"issue_date",
                    "действует до":"valid_until", "validuntil":"valid_until", "фото":"photo_path", "photofile":"photo_path"
                }
                next_auto_text = self.next_badge_number()
                next_auto_number = int(next_auto_text)
                auto_width = len(next_auto_text)
                with db_connect() as con:
                    used_numbers = {str(r[0]).strip() for r in con.execute("SELECT badge_number FROM employees WHERE badge_number<>''").fetchall()}
                    used_names = {" ".join(str(r[0]).casefold().split()) for r in con.execute("SELECT full_name FROM employees").fetchall()}
                    for row in reader:
                        data = {aliases.get(str(k).strip().casefold(), str(k).strip()): (v or "").strip() for k,v in row.items() if k}
                        if not data.get("full_name"): continue
                        badge = data.get("badge_number", "")
                        if not badge:
                            while str(next_auto_number).zfill(auto_width) in used_numbers:
                                next_auto_number += 1
                            badge = str(next_auto_number).zfill(auto_width)
                            next_auto_number += 1
                        normalized_name = " ".join(data["full_name"].casefold().split())
                        if badge in used_numbers or normalized_name in used_names:
                            duplicates += 1
                            continue
                        now = datetime.now().isoformat(timespec="seconds")
                        con.execute("INSERT INTO employees(full_name,organization,department,position,badge_number,valid_until,photo_path,issue_date,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?)",
                            (data.get("full_name"),data.get("organization",""),data.get("department",""),data.get("position",""),
                             badge,data.get("valid_until",""),data.get("photo_path",""),data.get("issue_date") or datetime.now().strftime("%d.%m.%Y"),now,now))
                        used_numbers.add(badge); used_names.add(normalized_name)
                        count += 1
            self.refresh_employees(); QMessageBox.information(self, APP_NAME, f"Импортировано записей: {count}\nПропущено дубликатов ФИО или номеров: {duplicates}")
        except Exception as e: QMessageBox.critical(self, APP_NAME, f"Ошибка импорта CSV:\n{e}")

    def import_employees_excel(self):
        path, _ = QFileDialog.getOpenFileName(self, "Импорт сотрудников Excel", "", "Excel (*.xlsx)")
        if not path: return
        try:
            from openpyxl import load_workbook
            wb = load_workbook(path, read_only=True, data_only=True)
            ws = wb.active
            rows = ws.iter_rows(values_only=True)
            headers = [str(x or "").strip() for x in next(rows, ())]
            aliases = {"ФИО":"full_name", "FullName":"full_name", "Организация":"organization", "Organization":"organization",
                       "Подразделение":"department", "Department":"department", "Должность":"position", "Position":"position",
                       "Номер пропуска":"badge_number", "BadgeNumber":"badge_number", "Дата выдачи":"issue_date", "IssueDate":"issue_date",
                       "Действует до":"valid_until", "ValidUntil":"valid_until", "Фото":"photo_path", "PhotoFile":"photo_path"}
            mapped = [aliases.get(h, h) for h in headers]
            if "full_name" not in mapped:
                raise ValueError("В первой строке должен быть столбец «ФИО» или FullName.")
            create_database_backup("before_excel_import")
            count = duplicates = 0
            next_num = int(self.next_badge_number()); width = len(self.next_badge_number())
            with db_connect() as con:
                used = {str(r[0]).strip() for r in con.execute("SELECT badge_number FROM employees WHERE badge_number<>''").fetchall()}
                for values in rows:
                    data = {key: str(value).strip() if value is not None else "" for key, value in zip(mapped, values) if key}
                    if not data.get("full_name"): continue
                    badge = data.get("badge_number", "")
                    if not badge:
                        while str(next_num).zfill(width) in used: next_num += 1
                        badge = str(next_num).zfill(width); next_num += 1
                    normalized_name = " ".join(data["full_name"].casefold().split())
                    if badge in used or con.execute("SELECT 1 FROM employees WHERE lower(trim(full_name))=? LIMIT 1", (normalized_name,)).fetchone():
                        duplicates += 1; continue
                    now = datetime.now().isoformat(timespec="seconds")
                    con.execute("INSERT INTO employees(full_name,organization,department,position,badge_number,valid_until,photo_path,issue_date,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?)",
                                (data["full_name"], data.get("organization",""), data.get("department",""), data.get("position",""), badge,
                                 data.get("valid_until",""), data.get("photo_path",""), data.get("issue_date") or datetime.now().strftime("%d.%m.%Y"), now, now))
                    used.add(badge); count += 1
            wb.close(); self.refresh_employees()
            QMessageBox.information(self, APP_NAME, f"Импортировано: {count}\nПропущено дубликатов: {duplicates}\nРезервная копия базы создана перед импортом.")
        except Exception as exc:
            logging.exception("Excel employee import failed")
            QMessageBox.critical(self, APP_NAME, f"Ошибка импорта Excel:\n{exc}")

    def export_employees_excel(self):
        if not self.require_role("export"): return
        path, _ = QFileDialog.getSaveFileName(self, "Экспорт сотрудников Excel", str(OUTPUT_DIR / "employees.xlsx"), "Excel (*.xlsx)")
        if not path: return
        try:
            from openpyxl import Workbook
            from openpyxl.styles import Font, PatternFill, Alignment
            with db_connect() as con:
                rows = con.execute("SELECT id,full_name,organization,department,position,badge_number,issue_date,valid_until,photo_path FROM employees ORDER BY full_name COLLATE NOCASE").fetchall()
            wb = Workbook(); ws = wb.active; ws.title = "Сотрудники"
            headers = ["ID", "ФИО", "Организация", "Подразделение", "Должность", "Номер пропуска", "Дата выдачи", "Действует до", "Фото"]
            ws.append(headers)
            for cell in ws[1]:
                cell.font = Font(bold=True, color="252A30"); cell.fill = PatternFill("solid", fgColor="F2C230"); cell.alignment = Alignment(horizontal="center")
            for rec in rows: ws.append(list(rec))
            ws.freeze_panes = "A2"; ws.auto_filter.ref = ws.dimensions
            for col, width in zip("ABCDEFGHI", [9,30,24,22,24,18,16,16,55]): ws.column_dimensions[col].width = width
            wb.save(path)
            QMessageBox.information(self, APP_NAME, f"Экспортировано записей: {len(rows)}\nФайл: {path}")
        except Exception as exc:
            logging.exception("Excel employee export failed")
            QMessageBox.critical(self, APP_NAME, f"Ошибка экспорта Excel:\n{exc}")

    def export_hub_bundle_csv(self):
        """Create a neutral CSV bridge plus locally prepared portrait files.
        HUB field mapping is completed by the operator during its supported import wizard.
        """
        with db_connect() as con:
            rows = [dict(r) for r in con.execute("SELECT * FROM employees ORDER BY full_name COLLATE NOCASE").fetchall()]
        if not rows:
            QMessageBox.information(self, APP_NAME, "Реестр сотрудников пуст."); return
        path, _ = QFileDialog.getSaveFileName(self, "Пакет CSV для Magicard HUB", str(OUTPUT_DIR / "Magicard_HUB_import.csv"), "CSV (*.csv)")
        if not path: return
        count, no_photo = 0, 0
        try:
            with open(path, "w", encoding="utf-8-sig", newline="") as f:
                writer = csv.writer(f, delimiter=";")
                writer.writerow(["FullName", "Organization", "Department", "Position", "BadgeNumber", "IssueDate", "ValidUntil", "PhotoFile"])
                for rec in rows:
                    photo_out = ""
                    source = Path(rec.get("photo_path") or "")
                    if source.is_file():
                        face = detect_face_focus(str(source))
                        fx, fy = (face[0], face[1]) if face else (0.5, 0.5)
                        im, w, h = make_photo(str(source), VNUKOVO_PHOTO_WIDTH_CM, VNUKOVO_PHOTO_HEIGHT_CM, 300, "Заполнить (обрезать)", fx, fy, 1.08 if face else 1.0)
                        safe = re.sub(r"[^A-Za-zА-Яа-яЁё0-9_-]+", "_", rec.get("badge_number") or str(rec["id"])).strip("_") or str(rec["id"])
                        photo_target = OUTPUT_DIR / f"Vnukovo_{safe}_21_8x29mm_300dpi.jpg"
                        im.save(photo_target, "JPEG", quality=96, dpi=(300,300), optimize=True); im.close()
                        photo_out = str(photo_target.resolve())
                    else:
                        no_photo += 1
                    writer.writerow([rec.get("full_name", ""), rec.get("organization", ""), rec.get("department", ""),
                                     rec.get("position", ""), rec.get("badge_number", ""), rec.get("issue_date", ""), rec.get("valid_until", ""), photo_out])
                    count += 1
            QMessageBox.information(self, APP_NAME,
                f"Подготовлен CSV-пакет: {count} записей\nБез исходного фото: {no_photo}\nCSV: {path}\n\nВ Magicard HUB используйте штатный импорт CSV/Excel и сопоставьте текстовые поля. Поле PhotoFile содержит путь к локальному JPG; возможность автоматического связывания этого столбца с image-полем зависит от конкретного мастера импорта HUB и должна быть проверена.")
        except Exception as e:
            logging.exception("Magicard HUB CSV export failed")
            QMessageBox.critical(self, APP_NAME, f"Ошибка создания пакета для HUB:\n{e}")

    def export_employees_csv(self):
        if not self.require_role("export"): return
        path, _ = QFileDialog.getSaveFileName(self, "Экспорт сотрудников CSV", str(OUTPUT_DIR / "employees.csv"), "CSV (*.csv)")
        if not path: return
        with db_connect() as con: rows = con.execute("SELECT full_name,organization,department,position,badge_number,issue_date,valid_until,photo_path FROM employees ORDER BY full_name").fetchall()
        headers = ["ФИО","Организация","Подразделение","Должность","Номер пропуска","Дата выдачи","Действует до","Фото"]
        try:
            with open(path, "w", encoding="utf-8-sig", newline="") as f:
                writer = csv.writer(f, delimiter=";"); writer.writerow(headers)
                for r in rows: writer.writerow(list(r))
            QMessageBox.information(self, APP_NAME, f"Экспортировано записей: {len(rows)}\n{path}")
        except Exception as e: QMessageBox.critical(self, APP_NAME, f"Ошибка экспорта:\n{e}")

    def resizeEvent(self, event):
        """Keep the UI comfortable on compact laptop screens and high-DPI monitors."""
        super().resizeEvent(event)
        if hasattr(self, "main_tabs"):
            compact = self.width() < 1050
            self.main_tabs.tabBar().setIconSize(QSize(16 if compact else 19, 16 if compact else 19))
            for button in self.findChildren(QPushButton):
                button.setMinimumHeight(30 if compact else 34)

    def closeEvent(self, event):
        try:
            with db_connect() as con:
                end_session(con, getattr(self, "active_session_id", None), "application_exit")
                if getattr(self, "active_session_id", None):
                    record_security_event(con, "session_ended", actor=getattr(self, "active_session_username", ""), details="Завершение программы")
        except Exception:
            logging.exception("Could not close operator session")
        self.settings["hub_path"] = self.hub_path.text().strip()
        self.settings["printer_model"] = "Magicard 300"
        self.settings["hub_version"] = "2.0.6"
        self.settings["photo_profile"] = "Vnukovo portrait approx. 21.8x29.0mm 300dpi (estimated from HUB screenshot)"
        save_json(SETTINGS_FILE, self.settings)
        try:
            db_backup = BACKUP_DIR / f"employees_{datetime.now().strftime('%Y%m%d_%H%M%S')}.sqlite3"
            shutil.copy2(DB_FILE, db_backup)
            db_backups = sorted(BACKUP_DIR.glob("employees_*.sqlite3"), key=lambda x: x.stat().st_mtime, reverse=True)
            for old_db in db_backups[10:]: old_db.unlink(missing_ok=True)
            backup_name = BACKUP_DIR / f"settings_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
            shutil.copy2(SETTINGS_FILE, backup_name)
            backups = sorted(BACKUP_DIR.glob("settings_*.json"), key=lambda x: x.stat().st_mtime, reverse=True)
            for old_backup in backups[20:]:
                old_backup.unlink(missing_ok=True)
        except Exception:
            logging.exception("Settings backup failed")
        for im, _, _ in self.prepared.values():
            try: im.close()
            except Exception: pass
        self.prepared.clear(); gc.collect()
        event.accept()

def create_splash_pixmap():
    """Create a compact branded startup screen without external dependencies."""
    pixmap = QPixmap(560, 300)
    pixmap.fill(QColor("#20252B"))
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.Antialiasing)

    gradient = QLinearGradient(0, 0, 560, 300)
    gradient.setColorAt(0.0, QColor("#343B43"))
    gradient.setColorAt(1.0, QColor("#20252B"))
    painter.fillRect(pixmap.rect(), gradient)

    # Subtle decorative accent.
    painter.setPen(Qt.NoPen)
    painter.setBrush(QColor(242, 194, 48, 24))
    painter.drawEllipse(390, -115, 310, 310)
    painter.setBrush(QColor(242, 194, 48, 14))
    painter.drawEllipse(425, -80, 240, 240)
    painter.fillRect(0, 0, 6, 300, QColor("#F2C230"))
    painter.fillRect(6, 0, 2, 300, QColor("#D9A91E"))

    icon_path = RESOURCES_DIR / "PS_PassMaster.png"
    if not icon_path.exists():
        icon_path = RESOURCES_DIR / "PS_PassMaster.ico"
    icon = QPixmap(str(icon_path))
    if not icon.isNull():
        icon = icon.scaled(76, 76, Qt.KeepAspectRatio, Qt.SmoothTransformation)
        painter.drawPixmap(44, 66, icon)

    painter.setPen(QColor("#f4f7f9"))
    title_font = QFont("Arial", 22, QFont.Bold)
    painter.setFont(title_font)
    painter.drawText(140, 105, "PS PASSMASTER PRO")

    painter.setPen(QColor("#D4D9DE"))
    sub_font = QFont("Arial", 10)
    painter.setFont(sub_font)
    painter.drawText(142, 139, "Создание пропусков и карт доступа")

    painter.setPen(QColor("#AEB6BE"))
    painter.drawText(44, 246, "Загрузка программы…")

    painter.setPen(QPen(QColor("#4A535D"), 1))
    painter.drawLine(44, 263, 516, 263)
    painter.setPen(QPen(QColor("#F2C230"), 3))
    painter.drawLine(44, 263, 174, 263)

    painter.setPen(QColor("#AEB6BE"))
    small_font = QFont("Arial", 8)
    painter.setFont(small_font)
    painter.drawText(44, 283, "EMPLOYEE ID & ACCESS CARD STUDIO  •  P.Sveshnikov  •  8.9.0")
    painter.end()
    return pixmap


if __name__ == "__main__":
    try:
        # OS-level lock prevents two GUI instances from writing to the same local data.
        _instance_lock = acquire_single_instance_lock(APP_DATA_DIR / "pspassmaster.instance.lock")
        if _instance_lock is None:
            print("PS PassMaster PRO уже запущена. Повторный запуск отменён.", file=sys.stderr)
            sys.exit(0)
        try:
            QApplication.setHighDpiScaleFactorRoundingPolicy(Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)
        except (AttributeError, TypeError):
            pass  # Qt 6 already enables per-monitor DPI scaling by default.
        app = QApplication(sys.argv)
        app.setApplicationName(APP_NAME)
        app.setApplicationDisplayName("PS PassMaster PRO")
        app.setOrganizationName("P.Sveshnikov")
        app.setWindowIcon(QIcon(str(RESOURCES_DIR / "PS_PassMaster.ico")))
        if sys.platform == "win32":
            try:
                import ctypes
                ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("PSveshnikov.PSPassMasterPRO")
            except Exception:
                pass
        # Use the Windows-native UI font; Qt scales point sizes for monitor DPI.
        ui_font = QFont("Segoe UI", 9)
        ui_font.setStyleStrategy(QFont.PreferAntialias)
        app.setFont(ui_font)

        splash_started = time.perf_counter()
        splash = QSplashScreen(create_splash_pixmap(), Qt.WindowStaysOnTopHint)
        splash.show()
        app.processEvents()

        win = MainWindow()
        remaining_ms = max(0, 2500 - int((time.perf_counter() - splash_started) * 1000))

        def show_main_window():
            win.showMaximized()
            splash.finish(win)

        QTimer.singleShot(remaining_ms, show_main_window)
        sys.exit(app.exec())
    except Exception:
        logging.exception("Fatal application error")
        raise
