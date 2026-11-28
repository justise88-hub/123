"""Offline smart-card lifecycle helpers for PS GuardPRO."""
from __future__ import annotations

CARD_TYPES = ("Постоянный", "Временный", "Посетитель")
CARD_STATUSES = ("Активен", "Заблокирован", "Заменён", "Истёк")

def normalize_card_type(value):
    value = (value or "Постоянный").strip()
    if value not in CARD_TYPES:
        raise ValueError("Неизвестный тип пропуска.")
    return value

def normalize_card_status(value):
    value = (value or "Активен").strip()
    if value not in CARD_STATUSES:
        raise ValueError("Неизвестный статус пропуска.")
    return value

def can_issue(status):
    return normalize_card_status(status) == "Активен"

def reissue_reason(value):
    value = " ".join((value or "").split())
    if len(value) < 3:
        raise ValueError("Укажите причину перевыпуска (не менее 3 символов).")
    if len(value) > 240:
        raise ValueError("Причина перевыпуска слишком длинная (максимум 240 символов).")
    return value
