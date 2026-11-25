"""Cross-platform process lock and read-only runtime safety checks."""
from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import Any


class SingleInstanceLock:
    """Owns an OS-level non-blocking lock until close() or process exit."""
    def __init__(self, handle, backend: str):
        self._handle = handle
        self._backend = backend

    def close(self) -> None:
        if self._handle is not None:
            try:
                if self._backend == "windows":
                    import msvcrt
                    self._handle.seek(0)
                    msvcrt.locking(self._handle.fileno(), msvcrt.LK_UNLCK, 1)
                elif self._backend == "posix":
                    import fcntl
                    fcntl.flock(self._handle.fileno(), fcntl.LOCK_UN)
            except (OSError, ImportError):
                pass
            try:
                self._handle.close()
            finally:
                self._handle = None


def acquire_single_instance_lock(path: str | Path) -> SingleInstanceLock | None:
    """Return a held lock, or None if another process already owns it."""
    lock_path = Path(path)
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    handle = lock_path.open("a+b")
    try:
        if os.name == "nt":
            import msvcrt
            handle.seek(0, os.SEEK_END)
            if handle.tell() == 0:
                handle.write(b"0")
                handle.flush()
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            backend = "windows"
        else:
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            backend = "posix"
        return SingleInstanceLock(handle, backend)
    except (OSError, BlockingIOError):
        handle.close()
        return None


def runtime_health_checks(*, app_data_dir: str | Path, backup_dir: str | Path,
                          photo_dir: str | Path, template_dir: str | Path,
                          warning_free_mb: int = 500, critical_free_mb: int = 100) -> list[dict[str, str]]:
    """Check storage and expected folders without creating or modifying anything."""
    checks: list[dict[str, str]] = []
    paths = (("Папка фотографий", Path(photo_dir)), ("Папка шаблонов", Path(template_dir)),
             ("Папка резервных копий", Path(backup_dir)))
    for label, path in paths:
        exists = path.exists() and path.is_dir()
        checks.append({"name": label, "status": "ok" if exists else "warning",
                       "details": "Папка доступна." if exists else "Папка отсутствует; проверьте путь или создайте её."})
    probe = Path(app_data_dir)
    try:
        usage = shutil.disk_usage(probe if probe.exists() else probe.parent)
        free_mb = usage.free // (1024 * 1024)
        status = "error" if free_mb < critical_free_mb else "warning" if free_mb < warning_free_mb else "ok"
        detail = f"Свободно {free_mb} МБ."
    except OSError:
        status, detail = "warning", "Не удалось определить свободное место на диске."
    checks.append({"name": "Свободное место на диске", "status": status, "details": detail})
    return checks
