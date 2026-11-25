"""Read-only database and employee-data health diagnostics."""
from __future__ import annotations
import sqlite3
from collections import defaultdict
from datetime import date
from pathlib import Path
from employee_lifecycle import parse_date

VALID_STATUSES = {"Активен", "Заблокирован", "Аннулирован", "Заменён", "Истёк"}


def inspect_database_health(database_path, records):
    """Inspect SQLite structure and employee rows without modifying any data.

    The report intentionally contains issue kinds and employee IDs, not names,
    badge values, or absolute paths.
    """
    db = Path(database_path)
    sqlite_result = "не проверено"
    foreign_key_errors = []
    if db.is_file():
        con = sqlite3.connect(str(db), timeout=10)
        try:
            row = con.execute("PRAGMA integrity_check").fetchone()
            sqlite_result = str(row[0]) if row else "нет результата"
            foreign_key_errors = [tuple(x) for x in con.execute("PRAGMA foreign_key_check").fetchall()]
        finally:
            con.close()
    active = [dict(r) for r in records if not bool(dict(r).get("is_deleted", 0))]
    findings = []
    badges = defaultdict(list)
    for row in active:
        rid = row.get("id")
        badge = " ".join(str(row.get("badge_number") or "").casefold().split())
        if badge:
            badges[badge].append(rid)
        if not str(row.get("full_name") or "").strip():
            findings.append({"kind": "name_missing", "employee_id": rid})
        status = str(row.get("card_status") or "").strip()
        if status and status not in VALID_STATUSES:
            findings.append({"kind": "status_unknown", "employee_id": rid})
        raw_date = str(row.get("valid_until") or "").strip()
        if raw_date and parse_date(raw_date) is None:
            findings.append({"kind": "date_invalid", "employee_id": rid})
        photo = str(row.get("photo_path") or "").strip()
        if photo and not Path(photo).expanduser().is_file():
            findings.append({"kind": "photo_missing_file", "employee_id": rid})
    for ids in badges.values():
        if len(ids) > 1:
            findings.append({"kind": "badge_duplicate", "employee_ids": sorted(ids, key=str)})
    if sqlite_result != "ok":
        findings.append({"kind": "sqlite_integrity_error"})
    if foreign_key_errors:
        findings.append({"kind": "foreign_key_error", "count": len(foreign_key_errors)})
    counts = defaultdict(int)
    for finding in findings:
        counts[finding["kind"]] += 1
    return {
        "sqlite_integrity": sqlite_result,
        "foreign_key_errors": len(foreign_key_errors),
        "employees_checked": len(active),
        "findings": findings,
        "counts": dict(sorted(counts.items())),
        "ok": not findings,
    }


def format_database_health_report(report):
    labels = {
        "name_missing": "Не заполнено ФИО", "status_unknown": "Неизвестный статус пропуска",
        "date_invalid": "Некорректный формат срока действия", "photo_missing_file": "Не найден файл фотографии",
        "badge_duplicate": "Повторяющиеся номера пропусков", "sqlite_integrity_error": "Ошибка целостности SQLite",
        "foreign_key_error": "Нарушения внешних ключей",
    }
    lines = [f"Проверено сотрудников: {report.get('employees_checked', 0)}",
             f"Проверка SQLite: {report.get('sqlite_integrity', 'не проверено')}",
             f"Ошибок внешних ключей: {report.get('foreign_key_errors', 0)}"]
    if not report.get("findings"):
        lines.append("Критических замечаний не обнаружено.")
    else:
        lines.append("Найденные замечания:")
        for kind, count in report.get("counts", {}).items():
            lines.append(f"• {labels.get(kind, kind)}: {count}")
    lines.extend(["", "Проверка работает только на чтение и не исправляет записи автоматически.",
                  "Перед ручным исправлением рекомендуется создать резервную копию."])
    return "\n".join(lines)
