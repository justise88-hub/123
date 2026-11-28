"""Offline security helpers for PS GuardPRO."""
from __future__ import annotations
import hashlib, hmac, os, sqlite3, struct
from pathlib import Path
from datetime import datetime
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from expiry_control import record_status_change

MAGIC = b'PSPMENC1'

class PinAttemptGuard:
    """Persistent-friendly lockout state; caller stores the returned dict locally."""
    def __init__(self, state=None, max_attempts=5, lock_seconds=300):
        state = state or {}
        self.failures = int(state.get('failures', 0))
        self.locked_until = float(state.get('locked_until', 0))
        self.max_attempts = max_attempts
        self.lock_seconds = lock_seconds
    def remaining(self, now=None):
        now = datetime.now().timestamp() if now is None else now
        return max(0, int(self.locked_until - now + .999))
    def fail(self, now=None):
        now = datetime.now().timestamp() if now is None else now
        self.failures += 1
        if self.failures >= self.max_attempts:
            self.locked_until = now + self.lock_seconds
            self.failures = 0
        return self.state()
    def success(self):
        self.failures = 0; self.locked_until = 0
        return self.state()
    def state(self):
        return {'failures': self.failures, 'locked_until': self.locked_until}

def encrypt_backup(source, destination, passphrase):
    """AES-256-GCM backup. Passphrase is never persisted; format is versioned."""
    if not isinstance(passphrase, str) or len(passphrase) < 10:
        raise ValueError('Пароль шифрования должен содержать не менее 10 символов.')
    source, destination = Path(source), Path(destination)
    salt, nonce = os.urandom(16), os.urandom(12)
    key = hashlib.scrypt(passphrase.encode('utf-8'), salt=salt, n=2**15, r=8, p=1, dklen=32, maxmem=64*1024*1024)
    plaintext = source.read_bytes()
    encrypted = AESGCM(key).encrypt(nonce, plaintext, MAGIC)
    destination.write_bytes(MAGIC + salt + nonce + encrypted)
    return destination

def decrypt_backup(source, destination, passphrase):
    raw = Path(source).read_bytes()
    if len(raw) < 52 or raw[:8] != MAGIC:
        raise ValueError('Неизвестный формат зашифрованной копии.')
    salt, nonce, payload = raw[8:24], raw[24:36], raw[36:]
    key = hashlib.scrypt(passphrase.encode('utf-8'), salt=salt, n=2**15, r=8, p=1, dklen=32, maxmem=64*1024*1024)
    plain = AESGCM(key).decrypt(nonce, payload, MAGIC)
    Path(destination).write_bytes(plain)
    return Path(destination)

def integrity_check(database):
    con = sqlite3.connect(database)
    try:
        result = con.execute('PRAGMA integrity_check').fetchone()
        return result[0] if result else 'no result'
    finally:
        con.close()

def expire_active_cards(database, now_date):
    """Mark expired active cards as Истёк; returns affected (id, name, badge)."""
    con = sqlite3.connect(database)
    try:
        rows = con.execute("SELECT id,full_name,badge_number,valid_until,card_status FROM employees WHERE is_deleted=0 AND card_status='Активен' AND COALESCE(valid_until,'')<>''").fetchall()
        changed=[]
        for emp_id, name, badge, valid_until, old_status in rows:
            try: expired = datetime.strptime(str(valid_until), '%d.%m.%Y').date() < now_date
            except (ValueError, TypeError): expired=False
            if expired:
                con.execute("UPDATE employees SET card_status='Истёк',updated_at=? WHERE id=?", (datetime.now().isoformat(timespec='seconds'), emp_id))
                now = datetime.now().isoformat(timespec='seconds')
                con.execute("INSERT INTO audit_log(event_type,employee_id,full_name,badge_number,details,operator,created_at) VALUES(?,?,?,?,?,?,?)", ('auto_expired',emp_id,name,badge,'Автоматическая смена статуса по окончании срока действия','system',now))
                record_status_change(con, emp_id, name, badge, old_status, 'Истёк', 'Автоматически: закончился срок действия пропуска', 'system', source='auto_expiry', changed_at=now)
                changed.append((emp_id,name,badge))
        con.commit(); return changed
    except Exception:
        con.rollback(); raise
    finally: con.close()
