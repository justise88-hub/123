"""Employee-level LAN synchronization using the authenticated HTTPS journal."""
from __future__ import annotations
import json, sqlite3, uuid
from datetime import datetime
from pathlib import Path
try:
    from .network_sync import fetch_changes, publish_change, RevisionConflict
except ImportError:
    from network_sync import fetch_changes, publish_change, RevisionConflict

EMPLOYEE_FIELDS = ("full_name", "organization", "department", "position", "badge_number", "valid_until", "photo_path", "issue_date", "created_at", "updated_at", "is_deleted", "deleted_at", "card_type", "card_status", "blocked_reason")

def ensure_employee_sync_schema(con: sqlite3.Connection) -> None:
    cols = {r[1] for r in con.execute("PRAGMA table_info(employees)")}
    if "sync_uid" not in cols:
        con.execute("ALTER TABLE employees ADD COLUMN sync_uid TEXT")
    for row_id, in con.execute("SELECT id FROM employees WHERE COALESCE(sync_uid,'')='' ").fetchall():
        con.execute("UPDATE employees SET sync_uid=? WHERE id=?", (str(uuid.uuid4()), row_id))
    con.execute("CREATE UNIQUE INDEX IF NOT EXISTS ux_employees_sync_uid ON employees(sync_uid)")
    con.execute("CREATE TABLE IF NOT EXISTS sync_entity_state(entity_type TEXT NOT NULL, entity_id TEXT NOT NULL, revision INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(entity_type,entity_id))")
    con.execute("CREATE TABLE IF NOT EXISTS sync_outbox(id INTEGER PRIMARY KEY AUTOINCREMENT, change_id TEXT NOT NULL UNIQUE, entity_type TEXT NOT NULL, entity_id TEXT NOT NULL, base_revision INTEGER NOT NULL, operation TEXT NOT NULL, payload TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, last_error TEXT NOT NULL DEFAULT '')")
    con.execute("CREATE TABLE IF NOT EXISTS sync_conflicts(id INTEGER PRIMARY KEY AUTOINCREMENT, entity_type TEXT NOT NULL, entity_id TEXT NOT NULL, remote_revision INTEGER NOT NULL, local_payload TEXT NOT NULL, remote_payload TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, resolved INTEGER NOT NULL DEFAULT 0, UNIQUE(entity_type,entity_id,remote_revision))")
    con.execute("CREATE TABLE IF NOT EXISTS sync_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)")
    con.execute("INSERT OR IGNORE INTO sync_meta(key,value) VALUES('applying_remote','0'),('server_cursor','0'),('bootstrap_done','0')")
    con.execute("DROP TRIGGER IF EXISTS trg_sync_employee_insert")
    con.execute("DROP TRIGGER IF EXISTS trg_sync_employee_update")
    con.execute("DROP TRIGGER IF EXISTS trg_sync_employee_delete")
    payload = ",".join("'" + f + "'," + f for f in EMPLOYEE_FIELDS)
    payload_new = ",".join("'" + f + "',NEW." + f for f in EMPLOYEE_FIELDS)
    base_new = "COALESCE((SELECT revision FROM sync_entity_state WHERE entity_type='employee' AND entity_id=NEW.sync_uid),0)+(SELECT COUNT(*) FROM sync_outbox WHERE entity_type='employee' AND entity_id=NEW.sync_uid)"
    con.execute(f"""CREATE TRIGGER trg_sync_employee_insert AFTER INSERT ON employees
      WHEN COALESCE((SELECT value FROM sync_meta WHERE key='applying_remote'),'0') <> '1'
      BEGIN
        UPDATE sync_meta SET value='1' WHERE key='applying_remote';
        UPDATE employees SET sync_uid=COALESCE(NEW.sync_uid,lower(hex(randomblob(16)))) WHERE id=NEW.id;
        UPDATE sync_meta SET value='0' WHERE key='applying_remote';
        INSERT INTO sync_outbox(change_id,entity_type,entity_id,base_revision,operation,payload)
        SELECT lower(hex(randomblob(16))),'employee',sync_uid,
          COALESCE((SELECT revision FROM sync_entity_state WHERE entity_type='employee' AND entity_id=sync_uid),0)+
          (SELECT COUNT(*) FROM sync_outbox WHERE entity_type='employee' AND entity_id=employees.sync_uid),
          'upsert',json_object('sync_uid',sync_uid,{payload}) FROM employees WHERE id=NEW.id;
      END""")
    con.execute(f"""CREATE TRIGGER trg_sync_employee_update AFTER UPDATE ON employees
      WHEN COALESCE((SELECT value FROM sync_meta WHERE key='applying_remote'),'0') <> '1'
      BEGIN INSERT INTO sync_outbox(change_id,entity_type,entity_id,base_revision,operation,payload)
      VALUES(lower(hex(randomblob(16))),'employee',NEW.sync_uid,{base_new},'upsert',json_object('sync_uid',NEW.sync_uid,{payload_new})); END""")
    con.execute("""CREATE TRIGGER trg_sync_employee_delete AFTER DELETE ON employees
      WHEN COALESCE((SELECT value FROM sync_meta WHERE key='applying_remote'),'0') <> '1'
      BEGIN INSERT INTO sync_outbox(change_id,entity_type,entity_id,base_revision,operation,payload)
      VALUES(lower(hex(randomblob(16))),'employee',OLD.sync_uid,
        COALESCE((SELECT revision FROM sync_entity_state WHERE entity_type='employee' AND entity_id=OLD.sync_uid),0)+
        (SELECT COUNT(*) FROM sync_outbox WHERE entity_type='employee' AND entity_id=OLD.sync_uid),
        'delete',json_object('sync_uid',OLD.sync_uid)); END""")

def queue_existing_employees(con: sqlite3.Connection) -> int:
    ensure_employee_sync_schema(con)
    done = con.execute("SELECT value FROM sync_meta WHERE key='bootstrap_done'").fetchone()[0]
    if done == "1":
        return 0
    before = con.execute("SELECT COUNT(*) FROM sync_outbox").fetchone()[0]
    fields = ",".join("'" + f + "',e." + f for f in EMPLOYEE_FIELDS)
    con.execute("INSERT INTO sync_outbox(change_id,entity_type,entity_id,base_revision,operation,payload) "
                "SELECT lower(hex(randomblob(16))),'employee',e.sync_uid,"
                "COALESCE((SELECT revision FROM sync_entity_state s WHERE s.entity_type='employee' AND s.entity_id=e.sync_uid),0),"
                "'upsert',json_object('sync_uid',e.sync_uid," + fields + ") FROM employees e "
                "WHERE NOT EXISTS(SELECT 1 FROM sync_outbox o WHERE o.entity_type='employee' AND o.entity_id=e.sync_uid)")
    con.execute("UPDATE sync_meta SET value='1' WHERE key='bootstrap_done'")
    return con.execute("SELECT COUNT(*) FROM sync_outbox").fetchone()[0] - before

def _apply_remote(con: sqlite3.Connection, change: dict) -> None:
    uid = str(change["entity_id"])
    con.execute("UPDATE sync_meta SET value='1' WHERE key='applying_remote'")
    try:
        if change["operation"] == "delete":
            stamp = datetime.now().isoformat(timespec="seconds")
            con.execute("UPDATE employees SET is_deleted=1,deleted_at=?,updated_at=? WHERE sync_uid=?", (stamp, stamp, uid))
        elif change["operation"] == "upsert":
            payload = change["payload"]
            if not isinstance(payload, dict) or payload.get("sync_uid") != uid:
                raise ValueError("Некорректная карточка в пакете синхронизации.")
            values = [payload.get(f, "") for f in EMPLOYEE_FIELDS]
            exists = con.execute("SELECT 1 FROM employees WHERE sync_uid=?", (uid,)).fetchone()
            if exists:
                assignments = ",".join(f + "=?" for f in EMPLOYEE_FIELDS)
                con.execute(f"UPDATE employees SET {assignments} WHERE sync_uid=?", (*values, uid))
            else:
                names = ",".join(("sync_uid",) + EMPLOYEE_FIELDS)
                marks = ",".join("?" for _ in range(len(EMPLOYEE_FIELDS)+1))
                con.execute(f"INSERT INTO employees({names}) VALUES({marks})", (uid, *values))
        else:
            raise ValueError("Неизвестная операция синхронизации.")
        con.execute("INSERT INTO sync_entity_state(entity_type,entity_id,revision) VALUES('employee',?,?) ON CONFLICT(entity_type,entity_id) DO UPDATE SET revision=excluded.revision", (uid, int(change["revision"])))
    finally:
        con.execute("UPDATE sync_meta SET value='0' WHERE key='applying_remote'")

def sync_once(db_path: Path | str, base_url: str, token: str, ca_file: str | None = None, push_limit: int = 100, pull_limit: int = 500) -> dict:
    db_path = Path(db_path); con = sqlite3.connect(db_path, timeout=20); con.row_factory = sqlite3.Row
    pushed = pulled = 0; conflicts = []
    try:
        ensure_employee_sync_schema(con)
        pending = con.execute("SELECT * FROM sync_outbox ORDER BY id LIMIT ?", (max(1,min(int(push_limit),500)),)).fetchall()
        for row in pending:
            item = {"change_id":row["change_id"],"entity_type":row["entity_type"],"entity_id":row["entity_id"],"base_revision":row["base_revision"],"operation":row["operation"],"payload":json.loads(row["payload"])}
            try: result = publish_change(base_url,token,item,ca_file)
            except RevisionConflict as exc:
                con.execute("UPDATE sync_outbox SET last_error=? WHERE id=?",(str(exc),row["id"]))
                conflicts.append({"entity_id":row["entity_id"],"current_revision":exc.current_revision}); continue
            con.execute("INSERT INTO sync_entity_state(entity_type,entity_id,revision) VALUES(?,?,?) ON CONFLICT(entity_type,entity_id) DO UPDATE SET revision=excluded.revision",(row["entity_type"],row["entity_id"],int(result["revision"])))
            con.execute("DELETE FROM sync_outbox WHERE id=?",(row["id"],)); pushed += 1
        cursor = int(con.execute("SELECT value FROM sync_meta WHERE key='server_cursor'").fetchone()[0])
        changes = fetch_changes(base_url,token,cursor,pull_limit,ca_file)
        for change in changes:
            local_pending = con.execute("SELECT payload FROM sync_outbox WHERE entity_type=? AND entity_id=? ORDER BY id DESC LIMIT 1", (change["entity_type"], change["entity_id"])).fetchone()
            if local_pending:
                con.execute("INSERT OR IGNORE INTO sync_conflicts(entity_type,entity_id,remote_revision,local_payload,remote_payload) VALUES(?,?,?,?,?)",
                            (change["entity_type"], change["entity_id"], int(change["revision"]), local_pending[0], json.dumps(change, ensure_ascii=False)))
                conflicts.append({"entity_id": change["entity_id"], "current_revision": int(change["revision"]), "local_preserved": True})
            else:
                _apply_remote(con,change)
            cursor=max(cursor,int(change["seq"])); pulled += 1
        con.execute("UPDATE sync_meta SET value=? WHERE key='server_cursor'",(str(cursor),)); con.commit()
        remaining = con.execute("SELECT COUNT(*) FROM sync_outbox").fetchone()[0]
        return {"pushed":pushed,"pulled":pulled,"pending":remaining,"conflicts":conflicts,"cursor":cursor}
    except Exception:
        con.rollback(); raise
    finally: con.close()
