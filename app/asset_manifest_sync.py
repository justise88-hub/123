"""Manifest-based, opt-in LAN synchronization for photos and Magicard templates.

Files are addressed by relative path and SHA-256. Downloads are atomic and never
silently overwrite a different local file. This module is intentionally a
one-shot service; the GUI/photo_path migration is not enabled automatically.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import sqlite3
import tempfile
from pathlib import Path, PurePosixPath

try:
    from .asset_sync import sha256_file, publish_asset
    from .network_sync import publish_change, fetch_changes, download_file, RevisionConflict, MAX_FILE_SIZE
except ImportError:  # pragma: no cover - direct script compatibility
    from asset_sync import sha256_file, publish_asset
    from network_sync import publish_change, fetch_changes, download_file, RevisionConflict, MAX_FILE_SIZE

_ALLOWED = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".card"}
_MAX_RELATIVE = 512


def _safe_relative(value: str) -> str:
    if not isinstance(value, str) or not value or len(value) > _MAX_RELATIVE or "\\" in value:
        raise ValueError("Некорректный относительный путь вложения.")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise ValueError("Путь вложения должен оставаться внутри каталога.")
    if not path.suffix or path.suffix.lower() not in _ALLOWED:
        raise ValueError("Тип файла не поддерживается синхронизацией.")
    if any(ord(ch) < 32 for ch in value):
        raise ValueError("Путь содержит управляющие символы.")
    return path.as_posix()


def _target(root: Path, relative: str) -> Path:
    relative = _safe_relative(relative)
    base = root.resolve()
    target = (base / Path(*PurePosixPath(relative).parts)).resolve()
    if target != base and base not in target.parents:
        raise ValueError("Путь вложения выходит за пределы каталога.")
    return target


def ensure_schema(db_path: str | Path) -> None:
    with sqlite3.connect(db_path, timeout=15) as con:
        con.execute("PRAGMA busy_timeout=15000")
        con.execute("""CREATE TABLE IF NOT EXISTS sync_assets (
            relative_path TEXT PRIMARY KEY, sha256 TEXT NOT NULL, size INTEGER NOT NULL,
            kind TEXT NOT NULL, mtime_ns INTEGER NOT NULL DEFAULT 0,
            updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)""")
        con.execute("""CREATE TABLE IF NOT EXISTS sync_asset_outbox (
            id INTEGER PRIMARY KEY AUTOINCREMENT, change_id TEXT NOT NULL UNIQUE,
            relative_path TEXT NOT NULL, sha256 TEXT NOT NULL, size INTEGER NOT NULL,
            kind TEXT NOT NULL, base_revision INTEGER NOT NULL DEFAULT 0,
            last_error TEXT NOT NULL DEFAULT '')""")
        con.execute("""CREATE TABLE IF NOT EXISTS sync_asset_state (
            relative_path TEXT PRIMARY KEY, revision INTEGER NOT NULL DEFAULT 0,
            sha256 TEXT NOT NULL DEFAULT '')""")
        con.execute("""CREATE TABLE IF NOT EXISTS sync_asset_conflicts (
            id INTEGER PRIMARY KEY AUTOINCREMENT, relative_path TEXT NOT NULL,
            local_sha256 TEXT NOT NULL DEFAULT '', remote_sha256 TEXT NOT NULL DEFAULT '',
            reason TEXT NOT NULL, remote_revision INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(relative_path, remote_revision, reason))""")
        con.execute("CREATE TABLE IF NOT EXISTS sync_asset_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL)")
        con.execute("INSERT OR IGNORE INTO sync_asset_meta(key,value) VALUES('server_cursor','0')")


def scan_assets(db_path: str | Path, asset_root: str | Path) -> int:
    """Hash supported files and enqueue only new/changed assets."""
    ensure_schema(db_path)
    root = Path(asset_root).resolve()
    if not root.is_dir():
        raise NotADirectoryError(f"Каталог вложений не найден: {root}")
    queued = 0
    with sqlite3.connect(db_path, timeout=15) as con:
        con.row_factory = sqlite3.Row
        for path in root.rglob("*"):
            if path.is_symlink() or not path.is_file() or path.suffix.lower() not in _ALLOWED:
                continue
            try:
                relative = _safe_relative(path.relative_to(root).as_posix())
                size = path.stat().st_size
                if size <= 0 or size > MAX_FILE_SIZE:
                    continue
                digest = sha256_file(path)
            except (OSError, ValueError):
                continue
            old = con.execute("SELECT sha256 FROM sync_assets WHERE relative_path=?", (relative,)).fetchone()
            if old and old["sha256"] == digest:
                continue
            kind = "magicard_template" if path.suffix.lower() == ".card" else "photo"
            state = con.execute("SELECT revision FROM sync_asset_state WHERE relative_path=?", (relative,)).fetchone()
            base_revision = int(state[0]) if state else 0
            con.execute("INSERT INTO sync_assets(relative_path,sha256,size,kind,mtime_ns) VALUES(?,?,?,?,?) "
                        "ON CONFLICT(relative_path) DO UPDATE SET sha256=excluded.sha256,size=excluded.size,kind=excluded.kind,mtime_ns=excluded.mtime_ns,updated_at=CURRENT_TIMESTAMP",
                        (relative, digest, size, kind, path.stat().st_mtime_ns))
            exists = con.execute("SELECT 1 FROM sync_asset_outbox WHERE relative_path=? AND sha256=?", (relative, digest)).fetchone()
            if not exists:
                con.execute("INSERT INTO sync_asset_outbox(change_id,relative_path,sha256,size,kind,base_revision) VALUES(lower(hex(randomblob(16))),?,?,?,?,?)",
                            (relative, digest, size, kind, base_revision))
                queued += 1
    return queued


def _atomic_save(target: Path, data: bytes, expected_sha: str) -> None:
    if len(data) <= 0 or len(data) > MAX_FILE_SIZE or hashlib.sha256(data).hexdigest() != expected_sha:
        raise ValueError("Вложение не прошло проверку размера или SHA-256.")
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=target.name + ".", suffix=".part", dir=str(target.parent))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        # Hard-link creation is atomic and fails if another process created the target.
        os.link(temp_name, target)
    except Exception:
        Path(temp_name).unlink(missing_ok=True)
        raise
    else:
        Path(temp_name).unlink(missing_ok=True)


def sync_assets(db_path: str | Path, asset_root: str | Path, base_url: str, token: str,
                ca_file: str | None = None, push_limit: int = 100, pull_limit: int = 500) -> dict:
    """Push queued files, then apply remote asset events without destructive overwrite."""
    ensure_schema(db_path)
    root = Path(asset_root).resolve()
    if not root.is_dir():
        raise NotADirectoryError(f"Каталог вложений не найден: {root}")
    pushed = pulled = 0
    conflicts: list[dict] = []
    with sqlite3.connect(db_path, timeout=20) as con:
        con.row_factory = sqlite3.Row
        rows = con.execute("SELECT * FROM sync_asset_outbox ORDER BY id LIMIT ?", (max(1, min(int(push_limit), 500)),)).fetchall()
        for row in rows:
            path = _target(root, row["relative_path"])
            try:
                if not path.is_file() or sha256_file(path) != row["sha256"]:
                    con.execute("UPDATE sync_asset_outbox SET last_error='Локальный файл изменился после сканирования' WHERE id=?", (row["id"],))
                    continue
                uploaded = publish_asset(path, base_url, token, ca_file)
                if uploaded["sha256"] != row["sha256"] or int(uploaded["size"]) != int(row["size"]):
                    raise ValueError("Сервер подтвердил другие параметры файла.")
                item = {"change_id": row["change_id"], "entity_type": "asset", "entity_id": row["relative_path"],
                        "base_revision": int(row["base_revision"]), "operation": "upsert",
                        "payload": {"path": row["relative_path"], "sha256": row["sha256"], "size": int(row["size"]), "kind": row["kind"]}}
                result = publish_change(base_url, token, item, ca_file)
                con.execute("INSERT INTO sync_asset_state(relative_path,revision,sha256) VALUES(?,?,?) "
                            "ON CONFLICT(relative_path) DO UPDATE SET revision=excluded.revision,sha256=excluded.sha256",
                            (row["relative_path"], int(result["revision"]), row["sha256"]))
                con.execute("DELETE FROM sync_asset_outbox WHERE id=?", (row["id"],)); pushed += 1
            except RevisionConflict as exc:
                con.execute("UPDATE sync_asset_outbox SET last_error=? WHERE id=?", (str(exc), row["id"]))
                conflicts.append({"path": row["relative_path"], "current_revision": int(exc.current_revision), "local_preserved": True})
            except Exception as exc:
                con.execute("UPDATE sync_asset_outbox SET last_error=? WHERE id=?", (str(exc)[:500], row["id"]))
                raise

        cursor_row = con.execute("SELECT value FROM sync_asset_meta WHERE key='server_cursor'").fetchone()
        cursor = int(cursor_row[0]) if cursor_row else 0
        changes = fetch_changes(base_url, token, cursor, max(1, min(int(pull_limit), 1000)), ca_file)
        for change in changes:
            seq = int(change.get("seq", cursor))
            if change.get("entity_type") == "asset":
                relative = _safe_relative(change.get("entity_id", ""))
                revision = int(change.get("revision", 0))
                if change.get("operation") != "upsert":
                    con.execute("INSERT OR IGNORE INTO sync_asset_conflicts(relative_path,reason,remote_revision) VALUES(?,?,?)",
                                (relative, "Удаление файла с сервера требует ручного подтверждения", revision))
                    conflicts.append({"path": relative, "reason": "remote_delete_requires_confirmation"})
                else:
                    payload = change.get("payload") or {}
                    if payload.get("path") != relative or not re.fullmatch(r"[0-9a-fA-F]{64}", str(payload.get("sha256", ""))):
                        raise ValueError("Сервер прислал некорректное описание вложения.")
                    digest = str(payload["sha256"]).lower()
                    target = _target(root, relative)
                    local_digest = sha256_file(target) if target.is_file() else ""
                    if local_digest and local_digest != digest:
                        con.execute("INSERT OR IGNORE INTO sync_asset_conflicts(relative_path,local_sha256,remote_sha256,reason,remote_revision) VALUES(?,?,?,?,?)",
                                    (relative, local_digest, digest, "Файл с таким путём уже существует и отличается", revision))
                        conflicts.append({"path": relative, "reason": "local_file_differs", "local_preserved": True})
                    elif not local_digest:
                        data = download_file(base_url, token, digest, ca_file)
                        if int(payload.get("size", len(data))) != len(data):
                            raise ValueError("Размер вложения не совпадает с метаданными сервера.")
                        _atomic_save(target, data, digest)
                    con.execute("INSERT INTO sync_asset_state(relative_path,revision,sha256) VALUES(?,?,?) "
                                "ON CONFLICT(relative_path) DO UPDATE SET revision=excluded.revision,sha256=excluded.sha256",
                                (relative, revision, digest))
                    if target.is_file() and sha256_file(target) == digest:
                        con.execute("INSERT INTO sync_assets(relative_path,sha256,size,kind,mtime_ns) VALUES(?,?,?,?,?) "
                                    "ON CONFLICT(relative_path) DO UPDATE SET sha256=excluded.sha256,size=excluded.size,kind=excluded.kind,mtime_ns=excluded.mtime_ns,updated_at=CURRENT_TIMESTAMP",
                                    (relative, digest, int(payload.get("size", target.stat().st_size)),
                                     str(payload.get("kind", "photo")), target.stat().st_mtime_ns))
                pulled += 1
            cursor = max(cursor, seq)
        con.execute("UPDATE sync_asset_meta SET value=? WHERE key='server_cursor'", (str(cursor),))
        con.commit()
        pending = int(con.execute("SELECT COUNT(*) FROM sync_asset_outbox").fetchone()[0])
        return {"pushed": pushed, "pulled": pulled, "pending": pending, "conflicts": conflicts, "cursor": cursor}
