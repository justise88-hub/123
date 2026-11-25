"""Privacy-preserving diagnostic bundle for PS PassMaster PRO.

The bundle deliberately excludes employee records, photographs, templates, PINs,
settings values, audit rows, and raw application logs.
"""
from __future__ import annotations

import importlib.metadata
import json
import platform
import sqlite3
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path


def _distribution_version(name: str) -> str | None:
    try:
        return importlib.metadata.version(name)
    except importlib.metadata.PackageNotFoundError:
        return None


def build_diagnostics(*, app_version: str, app_data_dir: Path, db_path: Path,
                      template_path: Path | None = None, hub_path: str = "") -> dict:
    """Collect technical metadata only; never read user content or configuration values."""
    data_dir = Path(app_data_dir)
    db = Path(db_path)
    db_check = "not_present"
    if db.is_file():
        try:
            con = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True, timeout=3)
            row = con.execute("PRAGMA quick_check").fetchone()
            con.close()
            db_check = "ok" if row and row[0] == "ok" else "failed"
        except Exception:
            db_check = "failed"

    template = Path(template_path) if template_path else None
    hub = Path(hub_path) if hub_path else None
    return {
        "schema": 1,
        "created_at_utc": datetime.now(timezone.utc).isoformat(),
        "application": {"name": "PS PassMaster PRO", "version": str(app_version)},
        "runtime": {
            "python": platform.python_version(),
            "implementation": platform.python_implementation(),
            "platform": platform.system(),
            "platform_release": platform.release(),
            "architecture": platform.machine(),
            "frozen_build": bool(getattr(sys, "frozen", False)),
        },
        "dependencies": {
            "PySide6": _distribution_version("PySide6"),
            "Pillow": _distribution_version("Pillow"),
            "opencv-python-headless": _distribution_version("opencv-python-headless"),
            "openpyxl": _distribution_version("openpyxl"),
            "cryptography": _distribution_version("cryptography"),
            "reportlab": _distribution_version("reportlab"),
        },
        "storage": {
            "data_directory_exists": data_dir.is_dir(),
            "database_exists": db.is_file(),
            "database_integrity": db_check,
            "template_exists": bool(template and template.is_file()),
            "hub_executable_exists": bool(hub and hub.is_file()),
        },
        "privacy": {
            "employee_database_included": False,
            "photographs_included": False,
            "templates_included": False,
            "settings_values_included": False,
            "pin_or_password_material_included": False,
            "audit_records_included": False,
            "raw_logs_included": False,
        },
    }


def create_support_bundle(destination: Path, *, app_version: str, app_data_dir: Path,
                          db_path: Path, template_path: Path | None = None,
                          hub_path: str = "") -> Path:
    """Write a small ZIP containing sanitized diagnostics and a privacy note."""
    target = Path(destination)
    target.parent.mkdir(parents=True, exist_ok=True)
    diagnostics = build_diagnostics(app_version=app_version, app_data_dir=app_data_dir,
                                    db_path=db_path, template_path=template_path,
                                    hub_path=hub_path)
    note = (
        "PS PassMaster PRO — диагностический пакет\n\n"
        "Пакет содержит только технические сведения о версии, среде выполнения, "
        "наличии компонентов и результат проверки целостности SQLite.\n"
        "База сотрудников, фотографии, шаблоны, настройки, PIN, журнал действий "
        "и исходные логи не включены.\n"
        "Перед отправкой третьим лицам всё равно проверьте содержимое архива.\n"
    )
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=7) as zf:
        zf.writestr("diagnostics.json", json.dumps(diagnostics, ensure_ascii=False, indent=2))
        zf.writestr("README_PRIVACY_RU.txt", note)
    # Re-open and validate the archive before reporting success.
    with zipfile.ZipFile(target, "r") as zf:
        bad = zf.testzip()
        if bad:
            target.unlink(missing_ok=True)
            raise zipfile.BadZipFile(f"Повреждён файл внутри пакета: {bad}")
    return target
