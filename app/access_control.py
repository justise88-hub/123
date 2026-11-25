"""Local access-card custody ledger (not a physical access-control connector)."""
from __future__ import annotations

from datetime import datetime

CARD_EVENTS = ("Выдан", "Возвращён", "Утерян", "Аннулирован")


def normalize_card_event(value: str) -> str:
    value = " ".join((value or "").split())
    if value not in CARD_EVENTS:
        raise ValueError("Неизвестная операция с пропуском.")
    return value


def validate_card_event(event: str, badge_number: str, note: str = "") -> tuple[str, str, str]:
    event = normalize_card_event(event)
    badge_number = " ".join((badge_number or "").split())
    note = " ".join((note or "").split())
    if not badge_number:
        raise ValueError("У сотрудника не указан номер пропуска.")
    if event in ("Утерян", "Аннулирован") and len(note) < 3:
        raise ValueError("Для утери или аннулирования укажите причину (не менее 3 символов).")
    if len(note) > 500:
        raise ValueError("Комментарий слишком длинный (максимум 500 символов).")
    return event, badge_number, note


def insert_card_event(con, employee: dict, event: str, note: str = "", operator: str = "") -> int:
    """Insert a custody event in the caller's transaction and return its ID."""
    event, badge, note = validate_card_event(event, employee.get("badge_number", ""), note)
    now = datetime.now().isoformat(timespec="seconds")
    cur = con.execute(
        "INSERT INTO card_movements(employee_id,full_name,badge_number,event_type,note,operator,created_at) VALUES(?,?,?,?,?,?,?)",
        (employee.get("id"), employee.get("full_name", ""), badge, event, note, operator, now),
    )
    return int(cur.lastrowid)
