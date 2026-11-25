"""Pre-issue validation for employee records and Magicard HUB card designs.

Checks are offline and read-only. They never rewrite the source template.
"""
from __future__ import annotations
from pathlib import Path
import copy
import re

from batch_issuance import issuance_block_reason
from card_personalization import personalize_design, TOKEN_RE
from card_preview import inspect_card_layout
from hub_integration import load_card, inspect_template, find_photo_element
from quality_control import assess_photo
from smart_cards import CARD_TYPES


def _clean(value):
    return " ".join(str(value or "").split())


def preflight_employee_card(record: dict, template_path: str | Path, photo_path: str | Path | None = None) -> dict:
    """Return {ok, errors, warnings, checks}; errors block card generation."""
    errors: list[str] = []
    warnings: list[str] = []
    checks: list[str] = []

    reason = issuance_block_reason(record)
    if reason:
        errors.append(reason)
    else:
        checks.append("Статус пропуска, ФИО и номер проверены")

    if not _clean(record.get("organization")):
        warnings.append("Не указана организация")
    if not _clean(record.get("position")):
        warnings.append("Не указана должность")
    card_type = _clean(record.get("card_type")) or "Постоянный"
    if card_type not in CARD_TYPES:
        errors.append(f"Неизвестный тип пропуска: {card_type}")
    else:
        checks.append(f"Тип пропуска: {card_type}")

    photo = Path(photo_path or record.get("photo_path") or "")
    if not photo.is_file():
        errors.append("Не найдена фотография сотрудника")
    else:
        photo_report = assess_photo(photo)
        if not photo_report.get("ok"):
            errors.extend("Фото: " + x for x in photo_report.get("errors", []))
        else:
            checks.append(f"Фотография читается: {photo_report['width']}×{photo_report['height']} px")
            warnings.extend("Фото: " + x for x in photo_report.get("warnings", []))

    try:
        inspect_template(template_path)
        design = load_card(template_path)
        personalized = copy.deepcopy(design)
        personalize_design(personalized, record)
        find_photo_element(design)
        checks.append("Структура .card и встроенный фотообъект проверены")
        for side in ("front", "back"):
            section = design.get(side)
            if section is None:
                continue
            if not isinstance(section, dict):
                errors.append(f"Раздел {side} имеет неверный формат")
                continue
            bg = section.get("background")
            if isinstance(bg, dict):
                width, height = bg.get("width", 1013), bg.get("height", 642)
                try:
                    width, height = float(width), float(height)
                except (TypeError, ValueError):
                    width, height = 1013, 642
                if width <= 0 or height <= 0:
                    errors.append(f"Некорректный размер холста стороны {side}")
                elif abs(width / height - 1013 / 642) > 0.03:
                    warnings.append(f"Пропорции холста стороны {side} отличаются от контрольного CR80 1013×642; сверьте в HUB")
            unresolved = []
            for element in personalized.get(side, {}).get("elements", []) if isinstance(personalized.get(side), dict) else []:
                text = element.get("text") if isinstance(element, dict) else None
                if isinstance(text, str) and TOKEN_RE.search(text):
                    unresolved.extend(m.group(0) for m in TOKEN_RE.finditer(text))
            if unresolved:
                errors.append(f"На стороне {side} остались неизвестные маркеры персонализации: {', '.join(sorted(set(unresolved)))}")
            for issue in inspect_card_layout(personalized, side):
                # Geometry violations block issue; font-fit notices are warnings.
                if "выходит за пределы" in issue or "неверные размеры" in issue:
                    errors.append(f"Макет ({side}): {issue}")
                else:
                    warnings.append(f"Макет ({side}): {issue}")
        checks.append("Геометрия лицевой и оборотной сторон проверена")
    except Exception as exc:
        errors.append(f"Шаблон Magicard не прошёл проверку: {exc}")

    # A date string is optional for permanent cards, but malformed supplied dates are not.
    valid_until = _clean(record.get("valid_until"))
    if not valid_until and card_type in {"Временный", "Посетитель"}:
        errors.append(f"Для пропуска типа «{card_type}» необходимо указать дату окончания действия")
    elif valid_until:
        from employee_lifecycle import parse_date
        if parse_date(valid_until) is None:
            errors.append(f"Некорректная дата окончания действия: {valid_until}")
        else:
            checks.append("Дата окончания действия распознана")

    return {"ok": not errors, "errors": errors, "warnings": list(dict.fromkeys(warnings)),
            "checks": checks, "template": str(template_path), "photo": str(photo)}


def format_preflight(report: dict) -> str:
    lines = ["ПРОВЕРКА ПЕРЕД ВЫПУСКОМ", ""]
    lines.extend("✓ " + x for x in report.get("checks", []))
    if report.get("errors"):
        lines += ["", "КРИТИЧЕСКИЕ ОШИБКИ:"]
        lines.extend("✗ " + x for x in report["errors"])
    if report.get("warnings"):
        lines += ["", "ПРЕДУПРЕЖДЕНИЯ:"]
        lines.extend("! " + x for x in report["warnings"])
    if report.get("ok") and not report.get("warnings"):
        lines += ["", "Критических ошибок и предупреждений не обнаружено."]
    return "\n".join(lines)
