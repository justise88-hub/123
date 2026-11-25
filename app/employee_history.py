"""Transactional, privacy-conscious version history for employee cards."""
from __future__ import annotations

import json
import sqlite3
from datetime import datetime

# Only business fields editable on the employee form are versioned/restored.
TRACKED_FIELDS = (
    "full_name", "organization", "department", "position", "badge_number",
    "valid_until", "photo_path", "issue_date",
)


def ensure_employee_history_schema(con: sqlite3.Connection) -> None:
    con.execute("""CREATE TABLE IF NOT EXISTS employee_change_history (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        employee_id INTEGER NOT NULL,
        change_kind TEXT NOT NULL DEFAULT 'edit',
        before_json TEXT NOT NULL,
        after_json TEXT NOT NULL,
        changed_at TEXT NOT NULL,
        changed_by TEXT NOT NULL DEFAULT '',
        source_history_id INTEGER
    )""")
    con.execute("CREATE INDEX IF NOT EXISTS ix_employee_change_history_employee ON employee_change_history(employee_id, id DESC)")


def employee_snapshot(row) -> dict:
    data = dict(row)
    return {key: str(data.get(key) or "") for key in TRACKED_FIELDS}


def record_employee_change(con: sqlite3.Connection, employee_id: int, before, after,
                           changed_by: str = "", change_kind: str = "edit",
                           source_history_id: int | None = None) -> int | None:
    old = employee_snapshot(before)
    new = employee_snapshot(after)
    if old == new:
        return None
    ensure_employee_history_schema(con)
    cur = con.execute(
        "INSERT INTO employee_change_history(employee_id,change_kind,before_json,after_json,changed_at,changed_by,source_history_id) VALUES(?,?,?,?,?,?,?)",
        (int(employee_id), change_kind, json.dumps(old, ensure_ascii=False, sort_keys=True),
         json.dumps(new, ensure_ascii=False, sort_keys=True),
         datetime.now().isoformat(timespec="seconds"), str(changed_by or "")[:128], source_history_id),
    )
    return int(cur.lastrowid)


def list_employee_changes(con: sqlite3.Connection, employee_id: int, limit: int = 100) -> list[dict]:
    ensure_employee_history_schema(con)
    rows = con.execute(
        "SELECT id,employee_id,change_kind,before_json,after_json,changed_at,changed_by,source_history_id "
        "FROM employee_change_history WHERE employee_id=? ORDER BY id DESC LIMIT ?",
        (int(employee_id), max(1, min(int(limit), 500))),
    ).fetchall()
    result = []
    for row in rows:
        item = dict(row)
        item["before"] = json.loads(item.pop("before_json"))
        item["after"] = json.loads(item.pop("after_json"))
        result.append(item)
    return result


def restore_employee_change(con: sqlite3.Connection, history_id: int, changed_by: str = "") -> dict:
    """Restore the before-snapshot of one history row; never erase history."""
    ensure_employee_history_schema(con)
    history = con.execute(
        "SELECT id,employee_id,before_json FROM employee_change_history WHERE id=?",
        (int(history_id),),
    ).fetchone()
    if history is None:
        raise LookupError("Запись истории не найдена.")
    employee_id = int(history["employee_id"] if isinstance(history, sqlite3.Row) else history[1])
    before_json = history["before_json"] if isinstance(history, sqlite3.Row) else history[2]
    target = json.loads(before_json)
    if not isinstance(target, dict) or set(target) != set(TRACKED_FIELDS):
        raise ValueError("Снимок истории имеет неверный формат.")
    current = con.execute("SELECT * FROM employees WHERE id=?", (employee_id,)).fetchone()
    if current is None:
        raise LookupError("Карточка сотрудника больше не существует.")
    if bool(dict(current).get("is_deleted", 0)):
        raise ValueError("Сначала восстановите сотрудника из корзины.")
    duplicate = con.execute(
        "SELECT 1 FROM employees WHERE badge_number=? AND id<>? AND badge_number<>'' LIMIT 1",
        (target["badge_number"], employee_id),
    ).fetchone()
    if duplicate:
        raise ValueError("Нельзя восстановить версию: этот номер пропуска уже используется.")
    old = employee_snapshot(current)
    assignments = ",".join(f"{field}=?" for field in TRACKED_FIELDS)
    con.execute(
        f"UPDATE employees SET {assignments}, updated_at=? WHERE id=?",
        tuple(target[field] for field in TRACKED_FIELDS) + (datetime.now().isoformat(timespec="seconds"), employee_id),
    )
    after = con.execute("SELECT * FROM employees WHERE id=?", (employee_id,)).fetchone()
    new_history_id = record_employee_change(
        con, employee_id, old, after, changed_by=changed_by,
        change_kind="restore", source_history_id=int(history_id),
    )
    return {"employee_id": employee_id, "history_id": new_history_id, "restored_from": int(history_id)}
