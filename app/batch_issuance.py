"""Pure preflight helpers for safe bulk Magicard card preparation."""
from __future__ import annotations

ISSUABLE_STATUSES = {"Активен", "Действует"}


def issuance_block_reason(record: dict) -> str:
    """Return a human-readable reason when a record must not be issued."""
    status = str(record.get("card_status") or "Активен").strip()
    if status not in ISSUABLE_STATUSES:
        return f"выпуск запрещён — статус «{status}»"
    if not str(record.get("full_name") or "").strip():
        return "не заполнено ФИО"
    if not str(record.get("badge_number") or "").strip():
        return "не указан номер пропуска"
    return ""
