import sys, tempfile, sqlite3, unittest
from pathlib import Path
from datetime import date
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'app'))
from security_center import PinAttemptGuard, encrypt_backup, decrypt_backup, integrity_check, expire_active_cards

class SecurityCenterTests(unittest.TestCase):
    def test_pin_lockout_and_reset(self):
        g=PinAttemptGuard(max_attempts=3, lock_seconds=60)
        g.fail(now=100); g.fail(now=101); state=g.fail(now=102)
        self.assertEqual(PinAttemptGuard(state, max_attempts=3, lock_seconds=60).remaining(now=110), 52)
        g.success(); self.assertEqual(g.remaining(now=110), 0)
    def test_encrypted_backup_roundtrip_and_wrong_password(self):
        with tempfile.TemporaryDirectory() as d:
            src=Path(d)/'a.db'; enc=Path(d)/'a.pspmenc'; out=Path(d)/'b.db'
            src.write_bytes(b'private database bytes')
            encrypt_backup(src,enc,'correct horse battery')
            with self.assertRaises(Exception): decrypt_backup(enc,out,'wrong password')
            decrypt_backup(enc,out,'correct horse battery')
            self.assertEqual(src.read_bytes(),out.read_bytes())
    def test_integrity_and_expiry(self):
        with tempfile.TemporaryDirectory() as d:
            db=Path(d)/'x.db'; con=sqlite3.connect(db)
            con.executescript("CREATE TABLE employees(id INTEGER PRIMARY KEY,full_name TEXT,badge_number TEXT,is_deleted INTEGER,card_status TEXT,valid_until TEXT,updated_at TEXT); CREATE TABLE audit_log(event_type TEXT,employee_id INTEGER,full_name TEXT,badge_number TEXT,details TEXT,operator TEXT,created_at TEXT);")
            con.execute("INSERT INTO employees VALUES(1,'Иванов','10',0,'Активен','01.01.2020','')"); con.commit(); con.close()
            self.assertEqual(integrity_check(db),'ok')
            changed=expire_active_cards(db,date(2026,1,1))
            self.assertEqual(len(changed),1)
            con=sqlite3.connect(db); self.assertEqual(con.execute('SELECT card_status FROM employees').fetchone()[0],'Истёк'); con.close()
if __name__=='__main__': unittest.main()
