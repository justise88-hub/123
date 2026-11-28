"""Expiry renewal and access-status history for PS GuardPRO."""
from __future__ import annotations
import sqlite3
from datetime import datetime
from employee_history import record_employee_change


def ensure_expiry_control_schema(con: sqlite3.Connection) -> None:
    con.execute("""CREATE TABLE IF NOT EXISTS card_status_history (
        id INTEGER PRIMARY KEY AUTOINCREMENT, employee_id INTEGER,
        full_name TEXT NOT NULL DEFAULT '', badge_number TEXT NOT NULL DEFAULT '',
        old_status TEXT NOT NULL DEFAULT '', new_status TEXT NOT NULL DEFAULT '',
        reason TEXT NOT NULL DEFAULT '', operator TEXT NOT NULL DEFAULT '',
        changed_at TEXT NOT NULL, source TEXT NOT NULL DEFAULT 'manual'
    )""")
    con.execute("CREATE INDEX IF NOT EXISTS ix_card_status_history_employee ON card_status_history(employee_id,id DESC)")
    con.execute("CREATE INDEX IF NOT EXISTS ix_card_status_history_date ON card_status_history(changed_at DESC)")


def record_status_change(con, employee_id, full_name, badge_number, old_status, new_status,
                         reason='', operator='', source='manual', changed_at=None):
    ensure_expiry_control_schema(con)
    if str(old_status or '') == str(new_status or '') and str(reason or '') == '':
        return None
    cur = con.execute(
        "INSERT INTO card_status_history(employee_id,full_name,badge_number,old_status,new_status,reason,operator,changed_at,source) VALUES(?,?,?,?,?,?,?,?,?)",
        (employee_id, str(full_name or ''), str(badge_number or ''), str(old_status or ''),
         str(new_status or ''), str(reason or ''), str(operator or '')[:128],
         changed_at or datetime.now().isoformat(timespec='seconds'), str(source or 'manual')))
    return int(cur.lastrowid)


def renew_employee_expiry(con, employee_id, new_valid_until, operator=''):
    """Renew one employee transactionally; preserves before/after version history."""
    row = con.execute("SELECT * FROM employees WHERE id=?", (int(employee_id),)).fetchone()
    if row is None:
        raise LookupError('Сотрудник не найден.')
    data = dict(row)
    if data.get('is_deleted'):
        raise ValueError('Нельзя продлить срок сотруднику из корзины.')
    old_expiry = str(data.get('valid_until') or '')
    new_expiry = str(new_valid_until or '')
    if old_expiry == new_expiry:
        return False
    now = datetime.now().isoformat(timespec='seconds')
    con.execute("UPDATE employees SET valid_until=?,updated_at=? WHERE id=?", (new_expiry, now, int(employee_id)))
    after = con.execute("SELECT * FROM employees WHERE id=?", (int(employee_id),)).fetchone()
    record_employee_change(con, employee_id, row, after, changed_by=operator, change_kind='expiry_renewal')
    con.execute(
        "INSERT INTO audit_log(event_type,employee_id,full_name,badge_number,details,operator,created_at) VALUES(?,?,?,?,?,?,?)",
        ('expiry_renewed', int(employee_id), data.get('full_name',''), data.get('badge_number',''),
         f"Срок действия: {old_expiry or 'без срока'} → {new_expiry or 'без срока'}", str(operator or ''), now))
    return True


def renew_many(con, employee_ids, new_valid_until, operator=''):
    """Renew selected active employees; returns number actually changed."""
    count = 0
    for employee_id in dict.fromkeys(int(x) for x in employee_ids):
        if renew_employee_expiry(con, employee_id, new_valid_until, operator):
            count += 1
    return count


def list_status_history(con, employee_id=None, limit=1000):
    ensure_expiry_control_schema(con)
    limit = max(1, min(int(limit), 5000))
    if employee_id is None:
        rows = con.execute("SELECT * FROM card_status_history ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
    else:
        rows = con.execute("SELECT * FROM card_status_history WHERE employee_id=? ORDER BY id DESC LIMIT ?", (int(employee_id), limit)).fetchall()
    return [dict(row) for row in rows]
