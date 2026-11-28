"""Offline analytics and quality summaries for PS GuardPRO."""
from __future__ import annotations
from collections import Counter
from datetime import date
from employee_lifecycle import expiry_status


def summarize_employees(records, today=None):
    """Return stable dashboard metrics for active (non-trashed) employee rows."""
    today = today or date.today()
    rows = list(records)
    statuses = Counter(expiry_status(r.get("valid_until", ""), today=today) for r in rows)
    card_statuses = Counter((r.get("card_status") or "Активен") for r in rows)
    card_types = Counter((r.get("card_type") or "Постоянный") for r in rows)
    photos = sum(bool((r.get("photo_path") or "").strip()) for r in rows)
    return {
        "total": len(rows), "valid": statuses["valid"], "expired": statuses["expired"],
        "expiring_30": statuses["soon"], "no_expiry": statuses["none"],
        "unknown_expiry": statuses["unknown"], "photos": photos,
        "missing_photos": len(rows) - photos,
        "blocked": card_statuses["Заблокирован"], "replaced": card_statuses["Заменён"],
        "by_type": dict(card_types), "by_status": dict(card_statuses),
    }


def expiry_rows(records, today=None, warning_days=30):
    """Sorted actionable expiry list; no expiry is excluded."""
    today = today or date.today()
    result = []
    for row in records:
        status = expiry_status(row.get("valid_until", ""), today=today, warning_days=warning_days)
        if status in ("expired", "soon", "unknown"):
            result.append({"id": row.get("id"), "full_name": row.get("full_name", ""),
                           "badge_number": row.get("badge_number", ""),
                           "valid_until": row.get("valid_until", ""), "status": status,
                           "department": row.get("department", "")})
    priority = {"expired": 0, "soon": 1, "unknown": 2}
    return sorted(result, key=lambda x: (priority[x["status"]], x["valid_until"], x["full_name"].casefold()))
