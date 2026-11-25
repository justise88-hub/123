"""Batch photo intelligence: non-destructive quality assessment and CSV reporting."""
from __future__ import annotations

import csv
from pathlib import Path
from typing import Iterable, Mapping

from quality_control import assess_photo


CSV_COLUMNS = [
    "employee_id", "full_name", "badge_number", "photo_path", "status",
    "width_px", "height_px", "megapixels", "brightness", "contrast",
    "sharpness", "face_detected", "errors", "warnings", "recommendation",
]


def assess_employee_photos(records: Iterable[Mapping]) -> list[dict]:
    """Assess each employee portrait and return normalized, export-ready rows."""
    rows: list[dict] = []
    for record in records:
        photo_path = str(record.get("photo_path") or "").strip()
        report = assess_photo(photo_path)
        errors = list(report.get("errors", []))
        warnings = list(report.get("warnings", []))
        if not photo_path:
            errors.insert(0, "Фотография не назначена")
        if errors:
            status = "ЗАМЕНИТЬ"
            recommendation = "Добавить или заменить фотографию; затем повторить проверку"
        elif warnings:
            status = "ПРОВЕРИТЬ"
            recommendation = "Проверить кадрирование, освещение и качество вручную"
        else:
            status = "ГОТОВО"
            recommendation = "Критических замечаний не обнаружено"
        rows.append({
            "employee_id": record.get("id", ""),
            "full_name": record.get("full_name", ""),
            "badge_number": record.get("badge_number", ""),
            "photo_path": photo_path,
            "status": status,
            "width_px": report.get("width", 0),
            "height_px": report.get("height", 0),
            "megapixels": report.get("megapixels", 0),
            "brightness": report.get("brightness", 0),
            "contrast": report.get("contrast", 0),
            "sharpness": report.get("sharpness", ""),
            "face_detected": "Да" if report.get("face_detected") is True else (
                "Нет" if report.get("face_detected") is False else "Не проверено"),
            "errors": "; ".join(errors),
            "warnings": "; ".join(warnings),
            "recommendation": recommendation,
        })
    return rows


def write_photo_quality_csv(rows: Iterable[Mapping], destination: str | Path) -> Path:
    """Write UTF-8-BOM CSV for convenient opening in Microsoft Excel."""
    target = Path(destination)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_COLUMNS, extrasaction="ignore", delimiter=";")
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in CSV_COLUMNS})
    return target
