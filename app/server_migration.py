"""Portable, verifiable migration bundles for PS PassMaster PRO server data.

The bundle can carry the LAN change journal and an optional legacy employee
SQLite database plus its companion files. It deliberately excludes TLS private
keys and access tokens; those must be provisioned securely on the destination.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import sqlite3
import tempfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath

FORMAT_VERSION = 1
MAX_FILES = 50000
MAX_UNPACKED = 4 * 1024 * 1024 * 1024


class MigrationError(RuntimeError):
    pass


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _sqlite_snapshot(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise MigrationError(f"Файл базы данных не найден: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    src = sqlite3.connect(str(source), timeout=30)
    dst = sqlite3.connect(str(destination))
    try:
        src.backup(dst, pages=256, sleep=0.05)
        result = dst.execute("PRAGMA integrity_check").fetchone()
        if not result or result[0] != "ok":
            raise MigrationError(f"Проверка SQLite не пройдена: {result}")
    finally:
        dst.close()
        src.close()


def _safe_files(root: Path):
    if not root.exists():
        return []
    if not root.is_dir():
        raise MigrationError(f"Ожидалась папка: {root}")
    found = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise MigrationError(f"Символические ссылки не поддерживаются: {path}")
        if path.is_file():
            found.append(path)
            if len(found) > MAX_FILES:
                raise MigrationError("Слишком много файлов для одного пакета миграции.")
    return found


def create_migration_bundle(*, sync_db: Path, output_zip: Path,
                            employee_db: Path | None = None,
                            employee_files: Path | None = None,
                            sync_files: Path | None = None,
                            workspace: str = "default") -> dict:
    """Create an immutable ZIP snapshot; source databases are never modified."""
    sync_db, output_zip = Path(sync_db), Path(output_zip)
    if not workspace or len(workspace) > 64:
        raise MigrationError("Некорректное имя рабочей группы.")
    output_zip.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pspm-migrate-") as tmp_name:
        stage = Path(tmp_name)
        sync_snapshot = stage / "server_data" / "sync" / "pspassmaster_sync.sqlite3"
        _sqlite_snapshot(sync_db, sync_snapshot)
        sources: list[tuple[Path, str]] = [(sync_snapshot, "server_data/sync/pspassmaster_sync.sqlite3")]
        if employee_db is not None:
            employee_snapshot = stage / "server_data" / "legacy" / "employees.sqlite3"
            _sqlite_snapshot(Path(employee_db), employee_snapshot)
            sources.append((employee_snapshot, "server_data/legacy/employees.sqlite3"))
        if sync_files is not None:
            root = Path(sync_files)
            for file_path in _safe_files(root):
                relative = file_path.relative_to(root).as_posix()
                if ".." in PurePosixPath(relative).parts:
                    raise MigrationError("Обнаружен небезопасный путь файла синхронизации.")
                sources.append((file_path, "server_data/sync_files/" + relative))
        if employee_files is not None:
            root = Path(employee_files)
            for file_path in _safe_files(root):
                relative = file_path.relative_to(root).as_posix()
                # The database itself is copied with SQLite Backup API above;
                # never bundle raw DB/WAL/SHM files from a live directory.
                if file_path.name.lower() in {"employees.sqlite3", "employees.sqlite3-wal", "employees.sqlite3-shm"}:
                    continue
                if ".." in PurePosixPath(relative).parts:
                    raise MigrationError("Обнаружен небезопасный путь файла.")
                sources.append((file_path, "server_data/legacy_files/" + relative))
        total = sum(p.stat().st_size for p, _ in sources)
        if total > MAX_UNPACKED:
            raise MigrationError("Общий размер данных превышает лимит 4 ГБ.")
        manifest = {
            "format": "PSPassMasterServerMigration",
            "format_version": FORMAT_VERSION,
            "created_at": datetime.now(timezone.utc).isoformat(),
            "workspace": workspace,
            "contains_sync_journal": True,
            "contains_employee_database": employee_db is not None,
            "contains_employee_files": employee_files is not None,
            "contains_sync_files": sync_files is not None,
            "files": [{"path": arc, "size": path.stat().st_size, "sha256": _sha256(path)}
                      for path, arc in sources],
            "security_note": "TLS private keys and authentication tokens are intentionally excluded.",
        }
        temp_zip = output_zip.with_suffix(output_zip.suffix + ".tmp")
        try:
            with zipfile.ZipFile(temp_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
                zf.writestr("manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2))
                for path, arc in sources:
                    zf.write(path, arc)
            with zipfile.ZipFile(temp_zip, "r") as zf:
                bad = zf.testzip()
                if bad:
                    raise MigrationError(f"Ошибка ZIP CRC: {bad}")
            os.replace(temp_zip, output_zip)
        except Exception:
            temp_zip.unlink(missing_ok=True)
            raise
    return manifest


def verify_migration_bundle(bundle: Path) -> dict:
    bundle = Path(bundle)
    if not bundle.is_file():
        raise MigrationError("Пакет миграции не найден.")
    try:
        with zipfile.ZipFile(bundle, "r") as zf:
            infos = zf.infolist()
            if len(infos) > MAX_FILES + 1 or sum(i.file_size for i in infos) > MAX_UNPACKED:
                raise MigrationError("Пакет превышает ограничения размера или числа файлов.")
            names = set()
            for info in infos:
                path = PurePosixPath(info.filename)
                if path.is_absolute() or ".." in path.parts or "\\" in info.filename:
                    raise MigrationError("В пакете обнаружен небезопасный путь.")
                if info.filename in names:
                    raise MigrationError("В пакете есть повторяющиеся имена файлов.")
                names.add(info.filename)
            manifest = json.loads(zf.read("manifest.json").decode("utf-8"))
            if manifest.get("format") != "PSPassMasterServerMigration" or manifest.get("format_version") != FORMAT_VERSION:
                raise MigrationError("Неподдерживаемый формат пакета миграции.")
            expected = {item["path"]: item for item in manifest.get("files", [])}
            if not expected or not set(expected).issubset(names):
                raise MigrationError("В пакете отсутствуют файлы, указанные в манифесте.")
            for name, item in expected.items():
                data = zf.read(name)
                if len(data) != int(item["size"]) or hashlib.sha256(data).hexdigest() != item["sha256"]:
                    raise MigrationError(f"Контрольная сумма не совпала: {name}")
            if "server_data/sync/pspassmaster_sync.sqlite3" not in expected:
                raise MigrationError("В пакете нет центрального журнала синхронизации.")
            return manifest
    except (zipfile.BadZipFile, KeyError, json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise MigrationError(f"Повреждённый пакет миграции: {exc}") from exc


def restore_migration_bundle(bundle: Path, destination: Path) -> dict:
    """Restore to a new directory only; never overwrite an existing server."""
    bundle, destination = Path(bundle), Path(destination)
    manifest = verify_migration_bundle(bundle)
    if destination.exists():
        if not destination.is_dir() or any(destination.iterdir()):
            raise MigrationError("Папка назначения уже существует и не пуста. Перезапись запрещена.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=destination.name + ".staging-", dir=str(destination.parent)))
    try:
        with zipfile.ZipFile(bundle, "r") as zf:
            for item in manifest["files"]:
                rel = PurePosixPath(item["path"])
                target = stage.joinpath(*rel.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(item["path"]) as src, target.open("wb") as dst:
                    shutil.copyfileobj(src, dst)
        sync_db = stage / "server_data" / "sync" / "pspassmaster_sync.sqlite3"
        con = sqlite3.connect(str(sync_db))
        try:
            check = con.execute("PRAGMA integrity_check").fetchone()
            if not check or check[0] != "ok":
                raise MigrationError("Восстановленная база SQLite не прошла integrity_check.")
        finally:
            con.close()
        if destination.exists() and not any(destination.iterdir()):
            destination.rmdir()
        os.replace(stage, destination)
    except Exception:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    return {"destination": str(destination), "workspace": manifest["workspace"],
            "files_restored": len(manifest["files"]),
            "contains_employee_database": manifest["contains_employee_database"],
            "contains_employee_files": manifest["contains_employee_files"],
            "contains_sync_files": manifest.get("contains_sync_files", False)}
