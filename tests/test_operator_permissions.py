import sqlite3
from datetime import datetime, timedelta
import pytest
from app.operator_accounts import (ensure_operator_schema, create_account, authenticate,
    get_account_permissions, update_account_permissions, account_allows, PERMISSIONS)

@pytest.fixture
def con():
    db=sqlite3.connect(':memory:'); db.row_factory=sqlite3.Row
    ensure_operator_schema(db); yield db; db.close()

def test_operator_defaults_and_custom_permissions(con):
    aid=create_account(con,'op_test','Оператор','operator','1234')
    user=authenticate(con,'op_test','1234')
    assert account_allows(user,'issue') and not account_allows(user,'roles')
    update_account_permissions(con,aid,{'view','export','roles'})
    user=authenticate(con,'op_test','1234')
    assert set(user['permissions'])=={'view','export','roles'}
    assert account_allows(user,'roles') and not account_allows(user,'issue')

def test_admin_can_have_restricted_permissions(con):
    aid=create_account(con,'admin_test','Администратор','admin','1234')
    update_account_permissions(con,aid,{'view','backup'})
    assert get_account_permissions(con,aid)=={'view','backup'}

def test_invalid_permission_rejected(con):
    aid=create_account(con,'operator2','Оператор 2','operator','1234')
    with pytest.raises(ValueError): update_account_permissions(con,aid,{'view','superuser'})

def test_account_lockout_after_five_bad_pins(con):
    aid=create_account(con,'lock_test','Проверка блокировки','operator','1234')
    now=datetime(2026,1,1,12,0,0)
    for _ in range(5): assert authenticate(con,'lock_test','0000',now=now) is None
    assert authenticate(con,'lock_test','1234',now=now) is None
    assert authenticate(con,'lock_test','1234',now=now+timedelta(seconds=301)) is not None

def test_success_resets_failed_attempts(con):
    create_account(con,'reset_test','Сброс попыток','operator','1234')
    now=datetime(2026,1,1,12,0,0)
    for _ in range(3): authenticate(con,'reset_test','0000',now=now)
    assert authenticate(con,'reset_test','1234',now=now) is not None
    row=con.execute("SELECT failed_attempts,locked_until FROM operator_accounts WHERE username='reset_test'").fetchone()
    assert row['failed_attempts']==0 and row['locked_until']==''

def test_legacy_operator_table_migrates_in_place():
    db=sqlite3.connect(':memory:'); db.row_factory=sqlite3.Row
    db.execute("CREATE TABLE operator_accounts (id INTEGER PRIMARY KEY, username TEXT, display_name TEXT, role TEXT, pin_salt TEXT, pin_hash TEXT, iterations INTEGER, is_enabled INTEGER, created_at TEXT, updated_at TEXT)")
    ensure_operator_schema(db)
    columns={r[1] for r in db.execute('PRAGMA table_info(operator_accounts)')}
    assert {'permissions_json','failed_attempts','locked_until'} <= columns
    db.close()

def test_unlock_account_clears_lock_without_changing_pin_or_permissions(con):
    from app.operator_accounts import unlock_account
    aid=create_account(con,'unlock_test','Снятие блокировки','operator','1234')
    update_account_permissions(con,aid,{'view','export'})
    now=datetime(2026,1,1,12,0,0)
    for _ in range(5): authenticate(con,'unlock_test','0000',now=now)
    row=con.execute('SELECT locked_until FROM operator_accounts WHERE id=?',(aid,)).fetchone()
    assert row['locked_until']
    unlock_account(con,aid,now=now+timedelta(seconds=1))
    user=authenticate(con,'unlock_test','1234',now=now+timedelta(seconds=2))
    assert user is not None and set(user['permissions'])=={'view','export'}

def test_unlock_unknown_account_fails(con):
    from app.operator_accounts import unlock_account
    with pytest.raises(ValueError): unlock_account(con,999)
