import sqlite3, shutil, subprocess, threading
from pathlib import Path
import pytest
from app.employee_sync import ensure_employee_sync_schema, queue_existing_employees, sync_once
from app.network_sync import ChangeStore, SyncHTTPServer

FIELDS = """id INTEGER PRIMARY KEY AUTOINCREMENT, sync_uid TEXT, full_name TEXT NOT NULL,
organization TEXT DEFAULT '', department TEXT DEFAULT '', position TEXT DEFAULT '',
badge_number TEXT DEFAULT '', valid_until TEXT DEFAULT '', photo_path TEXT DEFAULT '',
issue_date TEXT DEFAULT '', created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
is_deleted INTEGER NOT NULL DEFAULT 0, deleted_at TEXT DEFAULT '', card_type TEXT DEFAULT 'Постоянный',
card_status TEXT DEFAULT 'Активен', blocked_reason TEXT DEFAULT ''"""

def make_db(path):
    con=sqlite3.connect(path)
    con.execute(f"CREATE TABLE employees({FIELDS})")
    ensure_employee_sync_schema(con)
    con.commit(); con.close()

def test_insert_update_delete_are_queued_with_stable_uid(tmp_path):
    db=tmp_path/'a.db'; make_db(db)
    with sqlite3.connect(db) as con:
        con.execute("INSERT INTO employees(full_name,created_at,updated_at) VALUES(?,?,?)",('Иванов Иван','now','now'))
        uid=con.execute('SELECT sync_uid FROM employees').fetchone()[0]
        assert uid and len(uid)==32
        assert con.execute('SELECT COUNT(*) FROM sync_outbox').fetchone()[0]==1
        con.execute("UPDATE employees SET department='Охрана' WHERE sync_uid=?",(uid,))
        assert con.execute('SELECT COUNT(*) FROM sync_outbox').fetchone()[0]==2
        con.execute('DELETE FROM employees WHERE sync_uid=?',(uid,))
        assert con.execute('SELECT COUNT(*) FROM sync_outbox').fetchone()[0]==3
        ops=[r[0] for r in con.execute('SELECT operation FROM sync_outbox ORDER BY id')]
        assert ops==['upsert','upsert','delete']

def test_queue_existing_is_idempotent(tmp_path):
    db=tmp_path/'a.db'; make_db(db)
    with sqlite3.connect(db) as con:
        con.execute("INSERT INTO employees(full_name,created_at,updated_at) VALUES('Петров','now','now')")
        con.execute('DELETE FROM sync_outbox')
        assert queue_existing_employees(con)==1
        con.execute('DELETE FROM sync_outbox')
        assert queue_existing_employees(con)==0

def test_two_local_databases_exchange_employee_changes(tmp_path):
    openssl=shutil.which('openssl')
    if not openssl: pytest.skip('openssl is not installed')
    cert=tmp_path/'cert.pem'; key=tmp_path/'key.pem'
    subprocess.run([openssl,'req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],check=True,capture_output=True)
    store=ChangeStore(tmp_path/'server.db','office')
    server=SyncHTTPServer(('127.0.0.1',0),store,'a-very-long-test-token-123456',str(cert),str(key))
    thread=threading.Thread(target=server.serve_forever,daemon=True); thread.start()
    url=f'https://127.0.0.1:{server.server_address[1]}'
    a=tmp_path/'a.db'; b=tmp_path/'b.db'; make_db(a); make_db(b)
    try:
        with sqlite3.connect(a) as con:
            con.execute("INSERT INTO employees(full_name,organization,created_at,updated_at) VALUES('Иванов Иван','Охрана','now','now')")
        r=sync_once(a,url,'a-very-long-test-token-123456',str(cert))
        assert r['pushed']==1 and r['pending']==0
        r=sync_once(b,url,'a-very-long-test-token-123456',str(cert))
        assert r['pulled']==1
        with sqlite3.connect(b) as con:
            row=con.execute('SELECT full_name,organization,sync_uid FROM employees').fetchone()
            assert row[:2]==('Иванов Иван','Охрана')
            con.execute("UPDATE employees SET department='КПП' WHERE sync_uid=?",(row[2],))
        r=sync_once(b,url,'a-very-long-test-token-123456',str(cert))
        assert r['pushed']==1
        r=sync_once(a,url,'a-very-long-test-token-123456',str(cert))
        with sqlite3.connect(a) as con:
            assert con.execute('SELECT department FROM employees').fetchone()[0]=='КПП'
    finally:
        server.shutdown(); server.server_close(); thread.join(timeout=2)

def test_conflicting_local_edit_is_preserved_and_recorded(tmp_path):
    openssl=shutil.which('openssl')
    if not openssl: pytest.skip('openssl is not installed')
    cert=tmp_path/'cert.pem'; key=tmp_path/'key.pem'
    subprocess.run([openssl,'req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],check=True,capture_output=True)
    store=ChangeStore(tmp_path/'server.db','office')
    server=SyncHTTPServer(('127.0.0.1',0),store,'a-very-long-test-token-123456',str(cert),str(key))
    thread=threading.Thread(target=server.serve_forever,daemon=True); thread.start()
    url=f'https://127.0.0.1:{server.server_address[1]}'
    a=tmp_path/'a.db'; b=tmp_path/'b.db'; make_db(a); make_db(b)
    token='a-very-long-test-token-123456'
    try:
        with sqlite3.connect(a) as con:
            con.execute("INSERT INTO employees(full_name,organization,created_at,updated_at) VALUES('Иванов','Охрана','now','now')")
        sync_once(a,url,token,str(cert)); sync_once(b,url,token,str(cert))
        with sqlite3.connect(a) as con: con.execute("UPDATE employees SET department='Смена А'")
        with sqlite3.connect(b) as con: con.execute("UPDATE employees SET department='Смена Б'")
        sync_once(a,url,token,str(cert))
        result=sync_once(b,url,token,str(cert))
        assert result['conflicts']
        with sqlite3.connect(b) as con:
            assert con.execute('SELECT department FROM employees').fetchone()[0]=='Смена Б'
            conflict=con.execute('SELECT local_payload,remote_payload,resolved FROM sync_conflicts').fetchone()
            assert conflict and 'Смена Б' in conflict[0] and 'Смена А' in conflict[1] and conflict[2]==0
    finally:
        server.shutdown(); server.server_close(); thread.join(timeout=2)
