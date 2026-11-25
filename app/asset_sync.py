"""Verified file-transfer helpers for LAN attachments (photos/templates).

This module deliberately does not rewrite employee photo_path values or replace
Magicard templates. Callers must explicitly map an asset SHA-256 to its local
record/template and choose when the downloaded copy becomes active.
"""
from __future__ import annotations
import hashlib, os, tempfile
from pathlib import Path
from .network_sync import upload_file, download_file, MAX_FILE_SIZE


def sha256_file(path: str | Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def publish_asset(path: str | Path, base_url: str, token: str, ca_file: str | None = None) -> dict:
    source = Path(path)
    if not source.is_file():
        raise FileNotFoundError(f"Файл вложения не найден: {source}")
    size = source.stat().st_size
    if size <= 0 or size > MAX_FILE_SIZE:
        raise ValueError("Размер вложения должен быть от 1 байта до 25 МиБ.")
    data = source.read_bytes()
    result = upload_file(base_url, token, data, ca_file)
    return {"sha256": result["sha256"], "size": result["size"], "name": source.name}


def receive_asset(digest: str, destination: str | Path, base_url: str, token: str,
                  ca_file: str | None = None, overwrite: bool = False) -> Path:
    target = Path(destination)
    if target.exists() and not overwrite:
        raise FileExistsError(f"Файл уже существует, перезапись запрещена: {target}")
    data = download_file(base_url, token, digest, ca_file)
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=target.name + ".", suffix=".part", dir=str(target.parent))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        if sha256_file(temporary) != digest.lower():
            raise ValueError("Контрольная сумма сохранённого вложения не совпадает.")
        Path(temporary).replace(target)
    except Exception:
        Path(temporary).unlink(missing_ok=True)
        raise
    return target
