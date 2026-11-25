"""Operator work-session ledger and shift reporting."""
from __future__ import annotations
from datetime import datetime, timedelta
import csv


def ensure_operator_sessions_schema(con):
    con.execute("""CREATE TABLE IF NOT EXISTS operator_sessions (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        operator_id INTEGER,
        username TEXT NOT NULL DEFAULT '',
        display_name TEXT NOT NULL DEFAULT '',
        role TEXT NOT NULL DEFAULT '',
        started_at TEXT NOT NULL,
        ended_at TEXT NOT NULL DEFAULT '',
        end_reason TEXT NOT NULL DEFAULT '',
        duration_seconds INTEGER NOT NULL DEFAULT 0
    )""")
    con.execute("CREATE INDEX IF NOT EXISTS ix_operator_sessions_started ON operator_sessions(started_at DESC)")
    con.execute("CREATE INDEX IF NOT EXISTS ix_operator_sessions_user ON operator_sessions(username, started_at DESC)")


def start_session(con, account=None, now=None):
    account = account or {}
    stamp = (now or datetime.now()).isoformat(timespec='seconds')
    cur = con.execute("""INSERT INTO operator_sessions
        (operator_id,username,display_name,role,started_at)
        VALUES(?,?,?,?,?)""", (account.get('id'), account.get('username', 'Локальный оператор'),
        account.get('display_name', account.get('username', 'Локальный оператор')),
        account.get('role', 'legacy'), stamp))
    return int(cur.lastrowid)


def end_session(con, session_id, reason='application_exit', now=None):
    if not session_id:
        return False
    row = con.execute('SELECT started_at,ended_at FROM operator_sessions WHERE id=?', (int(session_id),)).fetchone()
    if not row or row['ended_at']:
        return False
    ended = now or datetime.now()
    try:
        started = datetime.fromisoformat(row['started_at'])
        seconds = max(0, int((ended - started).total_seconds()))
    except (TypeError, ValueError):
        seconds = 0
    con.execute("UPDATE operator_sessions SET ended_at=?,end_reason=?,duration_seconds=? WHERE id=? AND ended_at=''",
                (ended.isoformat(timespec='seconds'), str(reason)[:64], seconds, int(session_id)))
    return True


def recover_interrupted_sessions(con, now=None):
    """Close stale sessions left open by a crash or forced power-off."""
    rows = con.execute("SELECT id FROM operator_sessions WHERE ended_at='' ").fetchall()
    for row in rows:
        end_session(con, row['id'], 'interrupted_previous_run', now)
    return len(rows)


def list_sessions(con, limit=2000, username=None, since_days=90):
    days = max(1, min(3650, int(since_days)))
    cutoff = (datetime.now() - timedelta(days=days)).isoformat(timespec='seconds')
    sql = """SELECT id,operator_id,username,display_name,role,started_at,ended_at,end_reason,duration_seconds
             FROM operator_sessions WHERE started_at>=?"""
    args = [cutoff]
    if username:
        sql += ' AND username=?'; args.append(username)
    sql += ' ORDER BY started_at DESC,id DESC LIMIT ?'
    args.append(max(1, min(20000, int(limit))))
    return con.execute(sql, args).fetchall()


def duration_seconds(row, now=None):
    if row['ended_at']:
        try:
            return max(0, int(row['duration_seconds'] or 0))
        except (TypeError, ValueError):
            pass
    try:
        return max(0, int(((now or datetime.now()) - datetime.fromisoformat(row['started_at'])).total_seconds()))
    except (TypeError, ValueError):
        return 0


def format_duration(seconds):
    seconds = max(0, int(seconds))
    hours, rem = divmod(seconds, 3600)
    minutes, _ = divmod(rem, 60)
    return f'{hours:02d}:{minutes:02d}'


def session_summary(rows, now=None):
    totals = {}
    for row in rows:
        key = row['username'] or 'Локальный оператор'
        totals[key] = totals.get(key, 0) + duration_seconds(row, now)
    return {'sessions': len(rows), 'active': sum(1 for r in rows if not r['ended_at']),
            'total_seconds': sum(totals.values()), 'by_operator': totals}


def export_sessions_csv(rows, destination):
    with open(destination, 'w', newline='', encoding='utf-8-sig') as stream:
        writer = csv.writer(stream, delimiter=';')
        writer.writerow(['ID', 'Логин', 'Оператор', 'Роль', 'Начало', 'Окончание', 'Длительность, чч:мм', 'Причина завершения'])
        for row in rows:
            writer.writerow([row['id'], row['username'], row['display_name'], row['role'], row['started_at'],
                             row['ended_at'] or 'Активна', format_duration(duration_seconds(row)), row['end_reason']])
    return str(destination)
