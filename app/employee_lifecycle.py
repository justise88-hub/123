"""Offline employee lifecycle, badge expiry and local role PIN helpers."""
from __future__ import annotations
import hashlib, hmac, os
from datetime import date, datetime

PIN_ITERATIONS = 310_000


def parse_date(value):
    value = (value or "").strip()
    if not value:
        return None
    for fmt in ("%d.%m.%Y", "%Y-%m-%d", "%d/%m/%Y"):
        try:
            return datetime.strptime(value, fmt).date()
        except ValueError:
            pass
    return None


def expiry_status(value, today=None, warning_days=30):
    end = parse_date(value)
    if end is None:
        return "none" if not (value or "").strip() else "unknown"
    today = today or date.today()
    days = (end - today).days
    if days < 0:
        return "expired"
    if days <= warning_days:
        return "soon"
    return "valid"


def hash_pin(pin, salt=None):
    pin = str(pin)
    if len(pin) < 4 or len(pin) > 64 or not pin.isdigit():
        raise ValueError("PIN должен содержать от 4 до 64 цифр.")
    salt = salt or os.urandom(16)
    digest = hashlib.pbkdf2_hmac("sha256", pin.encode("utf-8"), salt, PIN_ITERATIONS)
    return {"salt": salt.hex(), "hash": digest.hex(), "iterations": PIN_ITERATIONS}


def verify_pin(pin, record):
    try:
        salt = bytes.fromhex(record["salt"])
        expected = bytes.fromhex(record["hash"])
        iterations = int(record.get("iterations", PIN_ITERATIONS))
        actual = hashlib.pbkdf2_hmac("sha256", str(pin).encode("utf-8"), salt, iterations)
        return hmac.compare_digest(actual, expected)
    except (KeyError, TypeError, ValueError):
        return False


def role_allows(role, permission):
    matrix = {
        "operator": {"view", "add", "edit", "delete", "prepare", "issue", "export"},
        "admin": {"view", "add", "edit", "prepare", "issue", "export", "restore", "purge", "roles", "backup"},
    }
    return permission in matrix.get(role, set())


def bulk_expiry_update(records, new_date):
    """Return normalized employee IDs for a safe batch expiry update."""
    parsed = parse_date(new_date)
    if new_date and parsed is None:
        raise ValueError("Некорректная дата срока действия.")
    ids = []
    for item in records:
        try:
            value = int(item["id"] if isinstance(item, dict) else item[0])
        except (KeyError, TypeError, ValueError, IndexError):
            continue
        if value > 0 and value not in ids:
            ids.append(value)
    return ids, (parsed.strftime("%d.%m.%Y") if parsed else "")


def expiry_label(value, today=None):
    return {
        "expired": "Просрочен", "soon": "Истекает в ближайшие 30 дней",
        "valid": "Действует", "none": "Без срока", "unknown": "Неизвестная дата",
    }[expiry_status(value, today)]
