"""Safe employee-field substitution for Magicard .card text elements."""
from __future__ import annotations
import re
from typing import Any

FIELD_ALIASES = {
    "FULL_NAME": "full_name", "FIO": "full_name", "ФИО": "full_name",
    "ORGANIZATION": "organization", "ORG": "organization", "ОРГАНИЗАЦИЯ": "organization",
    "DEPARTMENT": "department", "ОТДЕЛ": "department",
    "POSITION": "position", "ДОЛЖНОСТЬ": "position",
    "BADGE_NUMBER": "badge_number", "CARD_NUMBER": "badge_number", "НомерПропуска": "badge_number",
    "ISSUE_DATE": "issue_date", "ДАТА_ВЫДАЧИ": "issue_date",
    "VALID_UNTIL": "valid_until", "EXPIRY_DATE": "valid_until", "ДЕЙСТВУЕТ_ДО": "valid_until",
    "CARD_TYPE": "card_type", "ТИП_ПРОПУСКА": "card_type",
}
TOKEN_RE = re.compile(r"\{\{\s*([A-Za-zА-Яа-яЁё_]+)\s*\}\}|\$\{([A-Za-zА-Яа-яЁё_]+)\}")


def employee_tokens(record: dict[str, Any]) -> dict[str, str]:
    result = {}
    for token, key in FIELD_ALIASES.items():
        value = record.get(key, "")
        result[token] = "" if value is None else str(value)
    return result


def personalize_design(design: dict, record: dict[str, Any]) -> dict:
    """Mutate only text values containing explicit {{TOKEN}} or ${TOKEN} markers."""
    values = employee_tokens(record)
    count = 0
    for side in ("front", "back"):
        section = design.get(side, {})
        for element in section.get("elements", []) if isinstance(section, dict) else []:
            if not isinstance(element, dict) or not isinstance(element.get("text"), str):
                continue
            old = element["text"]
            def replace(match):
                nonlocal count
                name = match.group(1) or match.group(2)
                key = name.upper()
                field = FIELD_ALIASES.get(key)
                if field is None:
                    return match.group(0)
                count += 1
                value = record.get(field, "")
                return "" if value is None else str(value)
            element["text"] = TOKEN_RE.sub(replace, old)
    return design


def has_personalization_tokens(design: dict) -> bool:
    for side in ("front", "back"):
        section = design.get(side, {})
        for element in section.get("elements", []) if isinstance(section, dict) else []:
            if isinstance(element, dict) and isinstance(element.get("text"), str) and TOKEN_RE.search(element["text"]):
                return True
    return False
