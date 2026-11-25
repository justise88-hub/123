"""Read-only integrity checks for employee records and linked portrait files."""
from __future__ import annotations

import re
from collections import defaultdict
from pathlib import Path
from PIL import Image


def _norm(value: object) -> str:
    return re.sub(r"\s+", " ", str(value or "")).strip().casefold()


def inspect_employee_records(records) -> dict:
    """Inspect employee rows without modifying the database or any files.

    Findings contain record IDs and issue kinds only; personal data and absolute
    photo paths are deliberately excluded from the report.
    """
    rows = [dict(row) for row in records if not bool(dict(row).get("is_deleted", 0))]
    findings = []
    by_badge = defaultdict(list)
    by_identity = defaultdict(list)

    for row in rows:
        rid = row.get("id")
        badge = _norm(row.get("badge_number"))
        name = _norm(row.get("full_name"))
        organization = _norm(row.get("organization"))
        if badge:
            by_badge[badge].append(rid)
        if name:
            by_identity[(name, organization)].append(rid)

        photo_value = str(row.get("photo_path") or "").strip()
        if not photo_value:
            findings.append({"kind": "photo_missing", "employee_id": rid})
            continue
        photo = Path(photo_value).expanduser()
        if not photo.is_file():
            findings.append({"kind": "photo_not_found", "employee_id": rid})
            continue
        try:
            with Image.open(photo) as image:
                image.verify()
        except Exception:
            findings.append({"kind": "photo_unreadable", "employee_id": rid})

    for ids in by_badge.values():
        if len(ids) > 1:
            findings.append({"kind": "duplicate_badge", "employee_ids": sorted(ids, key=str)})
    for ids in by_identity.values():
        if len(ids) > 1:
            findings.append({"kind": "possible_duplicate_identity", "employee_ids": sorted(ids, key=str)})

    counts = defaultdict(int)
    for item in findings:
        counts[item["kind"]] += 1
    return {
        "employees_checked": len(rows),
        "findings": findings,
        "counts": dict(sorted(counts.items())),
        "ok": not findings,
    }


def format_employee_integrity_report(report: dict) -> str:
    labels = {
        "photo_missing": "Не указана фотография",
        "photo_not_found": "Файл фотографии не найден",
        "photo_unreadable": "Фотография повреждена или не читается",
        "duplicate_badge": "Повторяется номер пропуска",
        "possible_duplicate_identity": "Возможный дубликат ФИО и организации",
    }
    lines = [f"Проверено активных сотрудников: {int(report.get('employees_checked', 0))}"]
    counts = report.get("counts", {})
    if not counts:
        lines.append("Проблем не обнаружено.")
        return "\n".join(lines)
    lines.append("Найденные замечания:")
    for kind, count in counts.items():
        lines.append(f"• {labels.get(kind, kind)}: {int(count)}")
    lines.append("\nПроверка не изменяет базу, фотографии или записи сотрудников.")
    lines.append("Совпадение ФИО и организации — предупреждение для ручной проверки, не автоматическое объединение.")
    return "\n".join(lines)
