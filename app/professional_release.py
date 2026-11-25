"""Read-only readiness checks and dashboard metrics for PS PassMaster PRO 7.x."""
from __future__ import annotations

import sqlite3
from pathlib import Path
from typing import Iterable
from runtime_safety import runtime_health_checks


def readiness_checks(*, app_data_dir: Path, template_path: Path, db_path: Path,
                     hub_path: str = "", require_hub: bool = False, backup_dir: Path | None = None,
                     photo_dir: Path | None = None, template_dir: Path | None = None) -> list[dict[str, str]]:
    """Return actionable, read-only checks. Missing optional HUB is a warning."""
    checks: list[dict[str, str]] = []
    data = Path(app_data_dir)
    checks.append({"name": "Папка пользовательских данных", "status": "ok" if data.exists() and data.is_dir() else "error",
                   "details": str(data)})
    template = Path(template_path)
    checks.append({"name": "Шаблон Magicard HUB", "status": "ok" if template.is_file() else "warning",
                   "details": str(template) if template.is_file() else "Шаблон не выбран или не найден; выберите .card в настройках HUB."})
    db = Path(db_path)
    if db.exists():
        try:
            with sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True, timeout=5) as con:
                result = con.execute("PRAGMA quick_check").fetchone()
            ok = bool(result and result[0] == "ok")
            checks.append({"name": "База сотрудников", "status": "ok" if ok else "error",
                           "details": "Проверка SQLite пройдена." if ok else str(result[0] if result else "Нет результата")})
        except Exception as exc:
            checks.append({"name": "База сотрудников", "status": "error", "details": str(exc)})
    else:
        checks.append({"name": "База сотрудников", "status": "warning", "details": "База будет создана при первом запуске."})
    hub = Path(hub_path) if hub_path else None
    hub_ok = bool(hub and hub.is_file())
    checks.append({"name": "Magicard HUB", "status": "ok" if hub_ok else ("error" if require_hub else "warning"),
                   "details": str(hub) if hub_ok else "Путь к HUB не настроен. Карточки .card можно подготовить и открыть вручную."})
    if backup_dir is not None and photo_dir is not None and template_dir is not None:
        checks.extend(runtime_health_checks(app_data_dir=data, backup_dir=backup_dir,
                                            photo_dir=photo_dir, template_dir=template_dir))
    return checks


def summarize_checks(checks: Iterable[dict[str, str]]) -> dict[str, int]:
    result = {"ok": 0, "warning": 0, "error": 0}
    for check in checks:
        status = check.get("status", "warning")
        if status not in result:
            status = "warning"
        result[status] += 1
    return result
