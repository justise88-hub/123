"""Named local operator accounts with per-user permissions and lockout."""
from __future__ import annotations
from datetime import datetime, timedelta
import json, re
from employee_lifecycle import hash_pin, verify_pin

USERNAME_RE = re.compile(r"^[A-Za-zА-Яа-яЁё0-9_.-]{2,32}$")
PERMISSIONS = ('view','add','edit','delete','prepare','issue','export','restore','purge','roles','backup')
ROLE_DEFAULTS = {
    'operator': {'view','add','edit','delete','prepare','issue','export'},
    'admin': {'view','add','edit','prepare','issue','export','restore','purge','roles','backup'},
}

def ensure_operator_schema(con):
    con.execute("""CREATE TABLE IF NOT EXISTS operator_accounts (
        id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT NOT NULL COLLATE NOCASE UNIQUE,
        display_name TEXT NOT NULL, role TEXT NOT NULL CHECK(role IN ('admin','operator')),
        pin_salt TEXT NOT NULL, pin_hash TEXT NOT NULL, iterations INTEGER NOT NULL,
        is_enabled INTEGER NOT NULL DEFAULT 1, permissions_json TEXT NOT NULL DEFAULT '{}',
        failed_attempts INTEGER NOT NULL DEFAULT 0, locked_until TEXT NOT NULL DEFAULT '',
        created_at TEXT NOT NULL, updated_at TEXT NOT NULL)""")
    con.execute("CREATE INDEX IF NOT EXISTS ix_operator_accounts_enabled ON operator_accounts(is_enabled, username)")
    columns={r[1] for r in con.execute('PRAGMA table_info(operator_accounts)')}
    for name,decl in (("permissions_json", "TEXT NOT NULL DEFAULT '{}'"),("failed_attempts","INTEGER NOT NULL DEFAULT 0"),("locked_until","TEXT NOT NULL DEFAULT ''")):
        if name not in columns: con.execute(f'ALTER TABLE operator_accounts ADD COLUMN {name} {decl}')

def validate_account(username, display_name, role):
    username=(username or '').strip(); display_name=' '.join((display_name or '').split())
    if not USERNAME_RE.fullmatch(username): raise ValueError('Логин: 2–32 символа, буквы, цифры, точка, дефис или подчёркивание.')
    if not display_name or len(display_name)>80: raise ValueError('Укажите отображаемое имя (до 80 символов).')
    if role not in ROLE_DEFAULTS: raise ValueError('Недопустимая роль.')
    return username, display_name, role

def create_account(con, username, display_name, role, pin, now=None):
    username,display_name,role=validate_account(username,display_name,role)
    record=hash_pin(pin); stamp=(now or datetime.now()).isoformat(timespec='seconds')
    cur=con.execute('INSERT INTO operator_accounts(username,display_name,role,pin_salt,pin_hash,iterations,is_enabled,created_at,updated_at) VALUES(?,?,?,?,?,?,1,?,?)',
        (username,display_name,role,record['salt'],record['hash'],record['iterations'],stamp,stamp))
    return int(cur.lastrowid)

def update_account_pin(con, account_id, pin, now=None):
    record=hash_pin(pin); stamp=(now or datetime.now()).isoformat(timespec='seconds')
    cur=con.execute("UPDATE operator_accounts SET pin_salt=?,pin_hash=?,iterations=?,failed_attempts=0,locked_until='',updated_at=? WHERE id=?",
        (record['salt'],record['hash'],record['iterations'],stamp,int(account_id)))
    if not cur.rowcount: raise ValueError('Учётная запись не найдена.')

def set_account_enabled(con, account_id, enabled, now=None):
    cur=con.execute('UPDATE operator_accounts SET is_enabled=?,updated_at=? WHERE id=?',
        (1 if enabled else 0,(now or datetime.now()).isoformat(timespec='seconds'),int(account_id)))
    if not cur.rowcount: raise ValueError('Учётная запись не найдена.')

def delete_account(con, account_id):
    cur=con.execute('DELETE FROM operator_accounts WHERE id=?',(int(account_id),))
    if not cur.rowcount: raise ValueError('Учётная запись не найдена.')

def list_accounts(con, include_disabled=True):
    sql='SELECT id,username,display_name,role,is_enabled,permissions_json,failed_attempts,locked_until,created_at,updated_at FROM operator_accounts'
    if not include_disabled: sql+=' WHERE is_enabled=1'
    return con.execute(sql+' ORDER BY username COLLATE NOCASE').fetchall()

def get_account_permissions(con, account_id):
    row=con.execute('SELECT role,permissions_json FROM operator_accounts WHERE id=?',(int(account_id),)).fetchone()
    if not row: raise ValueError('Учётная запись не найдена.')
    try: overrides=json.loads(row['permissions_json'] or '{}')
    except (TypeError,ValueError): overrides={}
    allowed=set(ROLE_DEFAULTS.get(row['role'],set()))
    if isinstance(overrides,dict):
        for key,value in overrides.items():
            if key in PERMISSIONS:
                if value is True: allowed.add(key)
                elif value is False: allowed.discard(key)
    return allowed

def update_account_permissions(con, account_id, permissions, now=None):
    if not isinstance(permissions,(set,list,tuple)): raise ValueError('Права должны быть списком разрешений.')
    requested=set(permissions); unknown=requested-set(PERMISSIONS)
    if unknown: raise ValueError('Неизвестные разрешения: '+', '.join(sorted(unknown)))
    row=con.execute('SELECT role FROM operator_accounts WHERE id=?',(int(account_id),)).fetchone()
    if not row: raise ValueError('Учётная запись не найдена.')
    defaults=ROLE_DEFAULTS[row['role']]
    overrides={key:(key in requested) for key in PERMISSIONS if (key in requested)!=(key in defaults)}
    con.execute('UPDATE operator_accounts SET permissions_json=?,updated_at=? WHERE id=?',
        (json.dumps(overrides,ensure_ascii=False,sort_keys=True),(now or datetime.now()).isoformat(timespec='seconds'),int(account_id)))

def authenticate(con, username, pin, now=None, max_attempts=5, lock_seconds=300):
    row=con.execute('SELECT * FROM operator_accounts WHERE username=? COLLATE NOCASE',(str(username or '').strip(),)).fetchone()
    if not row or not row['is_enabled']: return None
    current=now or datetime.now(); locked=str(row['locked_until'] or '')
    if locked:
        try:
            if datetime.fromisoformat(locked)>current: return None
        except ValueError: pass
    record={'salt':row['pin_salt'],'hash':row['pin_hash'],'iterations':row['iterations']}
    if not verify_pin(pin,record):
        attempts=int(row['failed_attempts'] or 0)+1
        until=(current+timedelta(seconds=lock_seconds)).isoformat(timespec='seconds') if attempts>=max_attempts else ''
        con.execute('UPDATE operator_accounts SET failed_attempts=?,locked_until=?,updated_at=? WHERE id=?',
            (0 if until else attempts,until,current.isoformat(timespec='seconds'),row['id']))
        return None
    con.execute("UPDATE operator_accounts SET failed_attempts=0,locked_until='',updated_at=? WHERE id=?",(current.isoformat(timespec='seconds'),row['id']))
    return {'id':int(row['id']),'username':row['username'],'display_name':row['display_name'],'role':row['role'],
            'permissions':sorted(get_account_permissions(con,row['id']))}

def account_allows(account, permission):
    return bool(account and permission in set(account.get('permissions',ROLE_DEFAULTS.get(account.get('role'),set()))))


def unlock_account(con, account_id, now=None):
    """Clear a local account lockout without changing its PIN or permissions."""
    cur=con.execute("UPDATE operator_accounts SET failed_attempts=0,locked_until='',updated_at=? WHERE id=?",
        ((now or datetime.now()).isoformat(timespec='seconds'),int(account_id)))
    if not cur.rowcount: raise ValueError('Учётная запись не найдена.')
    return True
