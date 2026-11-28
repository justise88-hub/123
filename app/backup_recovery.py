"""Full offline backup, verification and recovery for PS GuardPRO."""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import sqlite3
import tempfile
import zipfile
from datetime import datetime
from pathlib import Path, PurePosixPath

from security_center import encrypt_backup, decrypt_backup

FORMAT = "PSPM-FULL-BACKUP"
FORMAT_VERSION = 1
MANIFEST_NAME = "backup_manifest.json"


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _safe_archive_name(name: str) -> str:
    p = PurePosixPath(name)
    if (p.is_absolute() or ".." in p.parts or not p.parts or "\\" in name
            or ":" in name or "\x00" in name):
        raise ValueError(f"Недопустимый путь в резервной копии: {name}")
    return p.as_posix()


def _sqlite_check(path: Path) -> None:
    con = sqlite3.connect(f"file:{path.resolve().as_posix()}?mode=ro", uri=True)
    try:
        integrity = con.execute("PRAGMA integrity_check").fetchone()
        tables = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        if not integrity or integrity[0] != "ok" or "employees" not in tables:
            raise ValueError("База в резервной копии не прошла проверку или не содержит реестр сотрудников.")
    finally:
        con.close()


def create_full_backup(database, app_data_dir, templates_dir, destination, passphrase=None):
    """Create a consistent portable archive; optionally encrypt the entire archive."""
    database, data_root = Path(database), Path(app_data_dir)
    templates_dir, destination = Path(templates_dir), Path(destination)
    if not database.is_file():
        raise FileNotFoundError("База сотрудников не найдена.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix="pspm_backup_", dir=str(destination.parent)))
    plain_zip = staging / "full_backup.zip"
    try:
        db_snapshot = staging / "employees.sqlite3"
        src = sqlite3.connect(database)
        dst = sqlite3.connect(db_snapshot)
        try:
            src.backup(dst)
            dst.commit()
        finally:
            dst.close(); src.close()
        _sqlite_check(db_snapshot)

        files: dict[str, Path] = {"database/employees.sqlite3": db_snapshot}
        config_root = data_root / "Config"
        if config_root.is_dir():
            for p in config_root.rglob("*"):
                if p.is_file() and not p.is_symlink():
                    files[(Path("Config") / p.relative_to(config_root)).as_posix()] = p
        history_file = data_root / "History" / "history.json"
        if history_file.is_file():
            files["History/history.json"] = history_file
        # Preserve the active Magicard templates shipped or maintained beside the app.
        if templates_dir.is_dir():
            for p in templates_dir.rglob("*"):
                if p.is_file() and not p.is_symlink():
                    files[(Path("Templates") / p.relative_to(templates_dir)).as_posix()] = p

        con = sqlite3.connect(db_snapshot)
        try:
            rows = con.execute("SELECT id, photo_path FROM employees WHERE COALESCE(photo_path,'')<>''").fetchall()
        finally:
            con.close()
        photo_map, missing_photos = {}, []
        for employee_id, raw_path in rows:
            original = Path(str(raw_path)).expanduser()
            if not original.is_file():
                missing_photos.append({"employee_id": employee_id, "path": str(raw_path)})
                continue
            ext = original.suffix.lower()[:12] or ".img"
            archive_name = f"Employees/Photos/{int(employee_id)}{ext}"
            # Avoid collisions if an employee has a same-ID duplicate record in a malformed DB.
            files[archive_name] = original
            photo_map[str(employee_id)] = {"archive_path": archive_name, "original_path": str(original)}

        manifest_files = []
        for archive_name, source in sorted(files.items()):
            manifest_files.append({"path": _safe_archive_name(archive_name), "size": source.stat().st_size, "sha256": _sha256(source)})
        manifest = {
            "format": FORMAT, "format_version": FORMAT_VERSION,
            "created_at": datetime.now().astimezone().isoformat(timespec="seconds"),
            "application": "PS GuardPRO", "database": "database/employees.sqlite3",
            "photo_map": photo_map, "missing_photos": missing_photos,
            "files": manifest_files,
        }
        with zipfile.ZipFile(plain_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
            zf.writestr(MANIFEST_NAME, json.dumps(manifest, ensure_ascii=False, indent=2).encode("utf-8"))
            for archive_name, source in sorted(files.items()):
                zf.write(source, _safe_archive_name(archive_name))
        if passphrase:
            encrypt_backup(plain_zip, destination, passphrase)
        else:
            shutil.copy2(plain_zip, destination)
        return {"path": destination, "files": len(files), "photos": len(photo_map),
                "missing_photos": len(missing_photos), "encrypted": bool(passphrase),
                "size": destination.stat().st_size}
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def inspect_full_backup(source, passphrase=None, temp_dir=None):
    """Verify container, manifest, every payload checksum and embedded SQLite database."""
    source = Path(source)
    temp_root = Path(tempfile.mkdtemp(prefix="pspm_verify_", dir=str(temp_dir) if temp_dir else None))
    try:
        candidate = source
        if source.suffix.lower() == ".pspmenc":
            if not passphrase:
                raise ValueError("Для этой копии требуется пароль.")
            candidate = temp_root / "decrypted.zip"
            decrypt_backup(source, candidate, passphrase)
        if not zipfile.is_zipfile(candidate):
            raise ValueError("Файл не является архивом резервной копии PS GuardPRO.")
        with zipfile.ZipFile(candidate, "r") as zf:
            bad = zf.testzip()
            if bad:
                raise ValueError(f"Повреждён файл внутри архива: {bad}")
            name_list = zf.namelist()
            names = set(name_list)
            if len(name_list) != len(names):
                raise ValueError("В архиве обнаружены повторяющиеся имена файлов.")
            if MANIFEST_NAME not in names:
                raise ValueError("В архиве отсутствует манифест.")
            manifest = json.loads(zf.read(MANIFEST_NAME).decode("utf-8"))
            if manifest.get("format") != FORMAT or manifest.get("format_version") != FORMAT_VERSION:
                raise ValueError("Версия формата резервной копии не поддерживается.")
            records = manifest.get("files")
            if not isinstance(records, list) or not records:
                raise ValueError("Манифест не содержит списка файлов.")
            seen = set()
            sizes = {}
            database_path = manifest.get("database")
            for item in records:
                if not isinstance(item, dict) or not {"path", "size", "sha256"}.issubset(item):
                    raise ValueError("Некорректная запись файла в манифесте.")
                name = _safe_archive_name(item["path"])
                if name in seen or name not in names:
                    raise ValueError(f"Файл отсутствует или повторяется в манифесте: {name}")
                try:
                    expected_size = int(item["size"])
                except (TypeError, ValueError):
                    raise ValueError(f"Некорректный размер файла в манифесте: {name}")
                digest = str(item["sha256"])
                if expected_size < 0 or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest.lower()):
                    raise ValueError(f"Некорректные контрольные данные файла: {name}")
                seen.add(name)
                sizes[name] = expected_size
                payload = zf.read(name)
                if len(payload) != expected_size or hashlib.sha256(payload).hexdigest() != digest.lower():
                    raise ValueError(f"Контрольная сумма не совпадает: {name}")
                if name == database_path:
                    db_path = temp_root / "check.sqlite3"
                    db_path.write_bytes(payload)
                    _sqlite_check(db_path)
            if database_path not in seen:
                raise ValueError("В резервной копии отсутствует база сотрудников.")
            if names != seen | {MANIFEST_NAME}:
                raise ValueError("Архив содержит файлы, не описанные в манифесте.")
            # Validate every photo reference against the actual archive inventory.
            photo_map = manifest.get("photo_map", {})
            if not isinstance(photo_map, dict):
                raise ValueError("Некорректная карта фотографий в манифесте.")
            for employee_id, record in photo_map.items():
                if not isinstance(record, dict) or "archive_path" not in record:
                    raise ValueError(f"Некорректная запись фотографии сотрудника {employee_id}.")
                photo_name = _safe_archive_name(record["archive_path"])
                if photo_name not in seen or not photo_name.startswith("Employees/Photos/"):
                    raise ValueError(f"Фотография сотрудника {employee_id} отсутствует в архиве.")
            db_check = sqlite3.connect(f"file:{(temp_root / 'check.sqlite3').resolve().as_posix()}?mode=ro", uri=True)
            try:
                employee_count = int(db_check.execute("SELECT COUNT(*) FROM employees").fetchone()[0])
            finally:
                db_check.close()
            return {"manifest": manifest, "files": len(seen), "photos": len(photo_map),
                    "missing_photos": len(manifest.get("missing_photos", [])),
                    "employee_count": employee_count, "total_payload_bytes": sum(sizes.values()),
                    "created_at": manifest.get("created_at"), "encrypted": source.suffix.lower() == ".pspmenc"}
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)



def audit_backup_directory(folder, temp_dir=None):
    """Verify every recognized full backup and return a safe inventory report."""
    folder = Path(folder)
    candidates = sorted(
        [*folder.glob("PSPassMaster_full_*.pspmbackup"), *folder.glob("PSPassMaster_full_*.pspmenc")],
        key=lambda p: p.stat().st_mtime, reverse=True,
    ) if folder.is_dir() else []
    results = []
    for path in candidates:
        item = {"name": path.name, "size": path.stat().st_size, "status": "error", "details": "Проверка не выполнена."}
        if path.suffix.lower() == ".pspmenc":
            item.update(status="password_required", details="Зашифрованная копия: для проверки требуется пароль.")
            results.append(item)
            continue
        try:
            checked = inspect_full_backup(path, temp_dir=temp_dir)
            item.update(status="ok", details="Архив, контрольные суммы и база данных исправны.",
                        created_at=checked.get("created_at"), files=checked["files"],
                        employees=checked["employee_count"], photos=checked["photos"],
                        missing_photos=checked["missing_photos"], encrypted=checked["encrypted"])
        except Exception as exc:
            # Keep report concise and avoid leaking full local paths.
            item["details"] = str(exc)[:300]
        results.append(item)
    return {"folder_exists": folder.is_dir(), "total": len(results),
            "valid": sum(x["status"] == "ok" for x in results),
            "password_required": sum(x["status"] == "password_required" for x in results),
            "invalid": sum(x["status"] == "error" for x in results), "items": results}


def rehearse_backup_restore(source, passphrase=None, temp_dir=None):
    """Stage every verified payload in an isolated temp directory; never touch live data."""
    source = Path(source)
    checked = inspect_full_backup(source, passphrase=passphrase, temp_dir=temp_dir)
    work = Path(tempfile.mkdtemp(prefix="pspm_rehearsal_", dir=str(temp_dir) if temp_dir else None))
    try:
        candidate = source
        if source.suffix.lower() == ".pspmenc":
            candidate = work / "decrypted.zip"
            decrypt_backup(source, candidate, passphrase)
        payload_root = work / "staged_payload"
        with zipfile.ZipFile(candidate, "r") as zf:
            manifest = json.loads(zf.read(MANIFEST_NAME).decode("utf-8"))
            for item in manifest["files"]:
                name = _safe_archive_name(item["path"])
                target = payload_root.joinpath(*PurePosixPath(name).parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(zf.read(name))
        staged_db = payload_root.joinpath(*PurePosixPath(_safe_archive_name(manifest["database"])).parts)
        _sqlite_check(staged_db)
        for record in manifest.get("photo_map", {}).values():
            photo = payload_root.joinpath(*PurePosixPath(_safe_archive_name(record["archive_path"])).parts)
            if not photo.is_file():
                raise ValueError("Не удалось подготовить фотографию из резервной копии.")
        return {"ok": True, "created_at": checked["created_at"],
                "files_verified": checked["files"], "employees": checked["employee_count"],
                "photos_verified": len(manifest.get("photo_map", {})),
                "missing_photos": checked["missing_photos"], "encrypted": checked["encrypted"],
                "details": "Подготовка восстановления пройдена. Рабочая база, настройки и фотографии не изменялись."}
    finally:
        shutil.rmtree(work, ignore_errors=True)


def restore_full_backup(source, database, app_data_dir, templates_dir, passphrase=None):
    """Restore verified files; caller must snapshot current app data before invoking."""
    source, database, data_root, templates_dir = Path(source), Path(database), Path(app_data_dir), Path(templates_dir)
    work = Path(tempfile.mkdtemp(prefix="pspm_restore_", dir=str(data_root.parent)))
    try:
        candidate = source
        if source.suffix.lower() == ".pspmenc":
            if not passphrase:
                raise ValueError("Для этой копии требуется пароль.")
            candidate = work / "decrypted.zip"
            decrypt_backup(source, candidate, passphrase)
        verified = inspect_full_backup(candidate, temp_dir=work)
        with zipfile.ZipFile(candidate, "r") as zf:
            manifest = json.loads(zf.read(MANIFEST_NAME).decode("utf-8"))
            # Extract only paths that were verified against the manifest; never use extractall.
            payload_root = work / "payload"
            for item in manifest["files"]:
                name = _safe_archive_name(item["path"])
                target = payload_root.joinpath(*PurePosixPath(name).parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(zf.read(name))
        staged_db = payload_root / manifest["database"]
        _sqlite_check(staged_db)
        photo_root = data_root / "Employees" / "Photos"
        photo_root.mkdir(parents=True, exist_ok=True)
        restored_photos = 0
        con = sqlite3.connect(staged_db)
        try:
            # Never leave old-machine absolute photo paths in the restored database.
            con.execute("UPDATE employees SET photo_path='' ")
            for employee_id, record in manifest.get("photo_map", {}).items():
                archive_name = _safe_archive_name(record["archive_path"])
                photo_src = payload_root.joinpath(*PurePosixPath(archive_name).parts)
                if not photo_src.is_file():
                    continue
                photo_dst = photo_root / photo_src.name
                photo_dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(photo_src, photo_dst)
                con.execute("UPDATE employees SET photo_path=? WHERE id=?", (str(photo_dst), int(employee_id)))
                restored_photos += 1
            con.commit()
        finally:
            con.close()
        _sqlite_check(staged_db)
        database.parent.mkdir(parents=True, exist_ok=True)
        db_new = database.with_suffix(database.suffix + ".new")
        shutil.copy2(staged_db, db_new)
        _sqlite_check(db_new)
        db_new.replace(database)

        # Restore settings/history files without deleting unrelated local files.
        for source_root_name, target_root_name in (("Config", "Config"), ("History", "History")):
            source_root = payload_root / source_root_name
            if source_root.is_dir():
                for src_file in source_root.rglob("*"):
                    if not src_file.is_file():
                        continue
                    rel = src_file.relative_to(source_root)
                    target = data_root / target_root_name / rel
                    target.parent.mkdir(parents=True, exist_ok=True)
                    temp_target = target.with_suffix(target.suffix + ".restore")
                    shutil.copy2(src_file, temp_target); temp_target.replace(target)
        # Restore template payload into app data's Templates folder (do not overwrite install folder).
        template_payload = payload_root / "Templates"
        if template_payload.is_dir():
            target_root = data_root / "Templates"
            for p in template_payload.rglob("*"):
                if p.is_file():
                    rel = p.relative_to(template_payload)
                    target = target_root / rel
                    if p.resolve() == target.resolve():
                        continue
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(p, target)
        return {"files": verified["files"], "photos_restored": restored_photos,
                "missing_photos": verified["missing_photos"], "created_at": verified["created_at"]}
    finally:
        shutil.rmtree(work, ignore_errors=True)


def prune_full_backups(folder, keep=14):
    """Keep newest full archives, independently of SQLite safety snapshots."""
    folder = Path(folder)
    files = sorted([*folder.glob("PSPassMaster_full_*.pspmbackup"), *folder.glob("PSPassMaster_full_*.pspmenc")],
                   key=lambda p: p.stat().st_mtime, reverse=True)
    removed = []
    for path in files[max(1, int(keep)):]:
        try:
            path.unlink(); removed.append(path)
        except OSError:
            pass
    return removed
