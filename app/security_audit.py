"""Local security event ledger for authentication and authorization decisions."""
from __future__ import annotations
from datetime import datetime, timedelta
import os


def ensure_security_audit_schema(con):
    con.execute("""CREATE TABLE IF NOT EXISTS security_events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        event_type TEXT NOT NULL,
        actor TEXT NOT NULL DEFAULT '',
        role TEXT NOT NULL DEFAULT '',
        permission TEXT NOT NULL DEFAULT '',
        details TEXT NOT NULL DEFAULT '',
        created_at TEXT NOT NULL
    )""")
    con.execute("CREATE INDEX IF NOT EXISTS ix_security_events_created ON security_events(created_at DESC, id DESC)")
    con.execute("CREATE INDEX IF NOT EXISTS ix_security_events_type ON security_events(event_type, created_at DESC)")


def local_actor():
    return (os.environ.get('USERNAME') or os.environ.get('USER') or 'local')[:128]


def record_security_event(con, event_type: str, *, actor: str = '', role: str = '',
                          permission: str = '', details: str = '', now: datetime | None = None) -> int:
    allowed = {'login_success', 'login_failure', 'access_denied', 'lockout_active',
               'roles_changed', 'critical_action_confirmed', 'critical_action_cancelled', 'session_ended'}
    if event_type not in allowed:
        raise ValueError('Неизвестный тип события безопасности.')
    stamp = (now or datetime.now()).isoformat(timespec='seconds')
    cur = con.execute(
        'INSERT INTO security_events(event_type,actor,role,permission,details,created_at) VALUES(?,?,?,?,?,?)',
        (event_type, (actor or local_actor())[:128], (role or '')[:32],
         (permission or '')[:64], (details or '')[:500], stamp))
    return int(cur.lastrowid)


def summarize_security_events(con, since_days: int = 30) -> dict:
    since_days = max(1, min(3650, int(since_days)))
    cutoff = (datetime.now() - timedelta(days=since_days)).isoformat(timespec='seconds')
    rows = con.execute(
        'SELECT event_type,COUNT(*) AS n FROM security_events WHERE created_at>=? GROUP BY event_type',
        (cutoff,)).fetchall()
    counts = {str(r['event_type']): int(r['n']) for r in rows}
    return {'since_days': since_days, 'total': sum(counts.values()),
            'login_success': counts.get('login_success', 0),
            'login_failure': counts.get('login_failure', 0),
            'access_denied': counts.get('access_denied', 0),
            'lockout_active': counts.get('lockout_active', 0),
            'roles_changed': counts.get('roles_changed', 0), 'counts': counts}


def list_security_events(con, limit: int = 500):
    limit = max(1, min(10000, int(limit)))
    return con.execute(
        'SELECT created_at,event_type,actor,role,permission,details FROM security_events ORDER BY id DESC LIMIT ?',
        (limit,)).fetchall()


def prune_security_events(con, keep_days: int = 365) -> int:
    keep_days = max(30, min(3650, int(keep_days)))
    cutoff = (datetime.now() - timedelta(days=keep_days)).isoformat(timespec='seconds')
    cur = con.execute('DELETE FROM security_events WHERE created_at < ?', (cutoff,))
    return int(cur.rowcount)
