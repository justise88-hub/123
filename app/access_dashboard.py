"""Unified offline access-card dashboard model."""
from __future__ import annotations
from collections import Counter
from datetime import date
from employee_lifecycle import expiry_status


def access_state(record: dict, today=None, warning_days=30) -> str:
    status = str(record.get("card_status") or "Активен").strip()
    if status in ("Заблокирован", "Аннулирован", "Заменён"):
        return "blocked"
    expiry = expiry_status(record.get("valid_until", ""), today=today, warning_days=warning_days)
    if status == "Истёк" or expiry == "expired":
        return "expired"
    if expiry == "soon":
        return "soon"
    if expiry == "unknown":
        return "check"
    if status == "Активен":
        return "active"
    return "other"


LABELS = {
    "all": "Все пропуска", "active": "Действуют", "soon": "Скоро истекают",
    "expired": "Просрочены", "blocked": "Заблокированы / аннулированы",
    "check": "Требуют проверки даты", "other": "Другие статусы",
}


def build_access_overview(records, today=None, warning_days=30, query="", state="all"):
    """Return stable metrics and filtered rows; does not mutate source records."""
    if not 1 <= int(warning_days) <= 365:
        raise ValueError("Период предупреждения должен быть от 1 до 365 дней.")
    needle = " ".join(str(query or "").casefold().split())
    prepared = []
    counts = Counter()
    for source in records:
        row = dict(source)
        if row.get("is_deleted"):
            continue
        current = access_state(row, today=today, warning_days=int(warning_days))
        counts[current] += 1
        row["access_state"] = current
        row["access_label"] = LABELS[current]
        prepared.append(row)
    visible = []
    for row in prepared:
        haystack = " ".join(str(row.get(k) or "") for k in
            ("full_name", "organization", "department", "position", "badge_number")).casefold()
        if state not in ("all", "") and row["access_state"] != state:
            continue
        if needle and needle not in haystack:
            continue
        visible.append(row)
    priority = {"expired": 0, "blocked": 1, "soon": 2, "check": 3, "active": 4, "other": 5}
    visible.sort(key=lambda r: (priority[r["access_state"]], str(r.get("full_name") or "").casefold()))
    return {
        "total": len(prepared), "counts": {key: counts.get(key, 0) for key in LABELS if key != "all"},
        "visible_count": len(visible), "rows": visible,
    }
