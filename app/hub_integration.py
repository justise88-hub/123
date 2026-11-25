"""Safe, version-agnostic helpers for preparing Magicard HUB .card JSON files.

This module deliberately does not automate printer control: it validates the
HUB design container and replaces only the embedded portrait payload.
"""
from __future__ import annotations
import base64
import binascii
import json
import re
import os
import shutil
from datetime import datetime
from pathlib import Path
from typing import Any
from PIL import Image
import io

PHOTO_OBJECT_NAME = "images.jpg"


def load_card(path: str | Path) -> dict[str, Any]:
    p = Path(path)
    try:
        data = json.loads(p.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"Не удалось прочитать .card как JSON: {exc}") from exc
    validate_card(data)
    return data


def find_photo_element(design: dict[str, Any]) -> dict[str, Any]:
    front = design.get("front")
    if not isinstance(front, dict) or not isinstance(front.get("elements"), list):
        raise ValueError("В .card отсутствует раздел front/elements. Возможно, формат шаблона другой.")
    matches = [e for e in front["elements"] if isinstance(e, dict)
               and str(e.get("fileName", "")).lower() == PHOTO_OBJECT_NAME]
    if len(matches) != 1:
        raise ValueError(f"Ожидался ровно один фотообъект {PHOTO_OBJECT_NAME}, найдено: {len(matches)}.")
    return matches[0]


def _decode_data_uri(value: Any, field: str) -> bytes:
    if not isinstance(value, str) or "," not in value or not value.startswith("data:image/"):
        raise ValueError(f"В фотообъекте отсутствует корректное поле {field}.")
    try:
        raw = base64.b64decode(value.split(",", 1)[1], validate=True)
        with Image.open(io.BytesIO(raw)) as im:
            im.verify()
        return raw
    except (binascii.Error, ValueError, OSError) as exc:
        raise ValueError(f"Встроенное изображение {field} повреждено.") from exc


def validate_card(design: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(design, dict):
        raise ValueError("Корневой объект .card должен быть JSON-объектом.")
    element = find_photo_element(design)
    image_info = {}
    for field in ("base64image", "originalBase64Image"):
        raw = _decode_data_uri(element.get(field), field)
        with Image.open(io.BytesIO(raw)) as image:
            image_info[field] = {"width": image.width, "height": image.height, "format": image.format}
            if image.width < 32 or image.height < 32:
                raise ValueError(f"Встроенное изображение {field} имеет подозрительно маленький размер.")
    about = design.get("about", {})
    if about is None:
        about = {}
    if not isinstance(about, dict):
        raise ValueError("Раздел about в .card имеет неверный формат.")
    preview_value = about.get("preview", "")
    has_preview = isinstance(preview_value, str) and preview_value.startswith("data:image/")
    if has_preview:
        _decode_data_uri(preview_value, "about.preview")
    return {"photo_object": PHOTO_OBJECT_NAME, "elements": len(design["front"]["elements"]),
            "has_preview": has_preview, "photo_images": image_info,
            "card_name": about.get("name", "")}


def safe_component(value: str, fallback: str = "employee") -> str:
    cleaned = re.sub(r"[^A-Za-zА-Яа-яЁё0-9_-]+", "_", str(value)).strip("_.-")
    return cleaned[:80] or fallback


def atomic_write_card(path: str | Path, design: dict[str, Any]) -> Path:
    """Validate and atomically save; never overwrite source template by accident."""
    target = Path(path).resolve()
    validate_card(design)
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_name(target.name + ".tmp")
    try:
        temp.write_text(json.dumps(design, ensure_ascii=False, indent=2), encoding="utf-8")
        # Re-open and validate the actual bytes before replacing the destination.
        validate_card(json.loads(temp.read_text(encoding="utf-8")))
        temp.replace(target)
    finally:
        try:
            if temp.exists():
                temp.unlink()
        except OSError:
            pass
    return target


def inspect_template(path: str | Path) -> dict[str, Any]:
    p = Path(path)
    if not p.is_file():
        raise ValueError("Файл шаблона не найден.")
    if p.suffix.lower() != ".card":
        raise ValueError("Для Magicard HUB требуется файл с расширением .card.")
    result = validate_card(load_card(p))
    result["file"] = str(p)
    result["size_bytes"] = p.stat().st_size
    return result



def discover_hub_templates(hub_executable: str | Path | None = None, extra_dirs=()) -> list[Path]:
    """Find valid HUB .card templates in likely installation/profile folders.

    Search is intentionally bounded to likely Magicard folders; it never scans whole drives.
    Invalid .card files are ignored. Results are ordered by recency, then filename preference.
    """
    roots: list[Path] = []
    if hub_executable:
        exe = Path(hub_executable)
        if exe.is_file():
            roots.extend([exe.parent, exe.parent / "Templates", exe.parent / "Card Designs",
                          exe.parent.parent / "Templates"])
    env_roots = {
        "APPDATA": ["Magicard", "Magicard HUB"],
        "LOCALAPPDATA": ["Magicard", "Magicard HUB"],
        "PROGRAMDATA": ["Magicard", "Magicard HUB"],
    }
    for env_name, children in env_roots.items():
        base = os.environ.get(env_name)
        if base:
            roots.extend(Path(base) / child for child in children)
    home = Path.home()
    roots.extend([home / "Documents" / "Magicard", home / "Documents" / "Magicard HUB",
                  home / "Documents" / "Card Designs"])
    roots.extend(Path(x) for x in extra_dirs if x)

    found: dict[str, tuple[Path, float]] = {}
    skip_names = {".git", ".venv", "venv", "__pycache__", "node_modules", "cache", "temp"}
    for root in roots:
        try:
            if not root.is_dir():
                continue
            # Bound recursion to four levels and avoid caches/build output.
            root_depth = len(root.parts)
            for current, dirs, files in os.walk(root):
                current_path = Path(current)
                depth = len(current_path.parts) - root_depth
                dirs[:] = [d for d in dirs if d.lower() not in skip_names and depth < 3]
                for name in files:
                    if not name.lower().endswith(".card"):
                        continue
                    candidate = current_path / name
                    try:
                        stat = candidate.stat()
                        if stat.st_size > 50 * 1024 * 1024:
                            continue
                        inspect_template(candidate)
                        found[str(candidate.resolve()).casefold()] = (candidate.resolve(), stat.st_mtime)
                    except Exception:
                        continue
        except (OSError, PermissionError):
            continue
    results = list(found.values())
    results.sort(key=lambda item: (
        1 if any(token in item[0].name.casefold() for token in ("new card design", "template", "шаблон")) else 0,
        item[1]
    ), reverse=True)
    return [item[0] for item in results]


def copy_hub_template(source: str | Path, managed_dir: str | Path) -> Path:
    """Validate and copy a HUB template into app-managed storage without touching source."""
    source_path = Path(source).resolve()
    inspect_template(source_path)
    target_dir = Path(managed_dir)
    target_dir.mkdir(parents=True, exist_ok=True)
    target = target_dir / source_path.name
    if target.resolve() == source_path:
        return target
    if target.exists():
        try:
            if target.read_bytes() == source_path.read_bytes():
                inspect_template(target)
                return target
        except OSError:
            pass
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        target = target_dir / f"{source_path.stem}_imported_{stamp}{source_path.suffix}"
    temp = target.with_name(target.stem + ".tmp" + target.suffix)
    try:
        shutil.copy2(source_path, temp)
        inspect_template(temp)
        temp.replace(target)
        inspect_template(target)
    finally:
        try:
            temp.unlink(missing_ok=True)
        except OSError:
            pass
    return target

def discover_hub_executable() -> Path | None:
    """Best-effort Windows discovery; never assumes a particular HUB install path."""
    import os
    candidates: list[Path] = []
    if os.name == "nt":
        import winreg
        for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
            for key_name in (
                r"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\MagicardHUB.exe",
                r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\MagicardHUB.exe",
            ):
                try:
                    with winreg.OpenKey(hive, key_name) as key:
                        value, _ = winreg.QueryValueEx(key, None)
                        if value:
                            candidates.append(Path(value.strip('"')))
                except OSError:
                    pass
        for env_name in ("ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"):
            root = os.environ.get(env_name)
            if root:
                base = Path(root)
                candidates.extend((base / "Magicard" / "Magicard HUB" / "MagicardHUB.exe",
                                   base / "Magicard" / "HUB" / "MagicardHUB.exe"))
    for candidate in candidates:
        if candidate.is_file() and candidate.suffix.lower() == ".exe":
            return candidate.resolve()
    return None


def open_card_file(card_path: str | Path, hub_executable: str | Path | None = None) -> None:
    """Open a .card through the OS file association (preferred) or the configured HUB app."""
    import os
    import subprocess
    import sys
    target = Path(card_path).resolve()
    if not target.is_file() or target.suffix.lower() != ".card":
        raise ValueError("Готовый файл .card не найден или имеет неверное расширение.")
    if os.name == "nt":
        try:
            os.startfile(str(target))  # Uses the user's registered Magicard HUB association.
            return
        except OSError:
            if hub_executable:
                exe = Path(hub_executable)
                if exe.is_file():
                    subprocess.Popen([str(exe), str(target)], close_fds=True)
                    return
            raise RuntimeError("Windows не знает, какой программой открыть .card. Укажите Magicard HUB.exe или назначьте ассоциацию .card.")
    if sys.platform == "darwin":
        subprocess.Popen(["open", str(target)])
    else:
        subprocess.Popen(["xdg-open", str(target)])


def compare_template_preservation(template: str | Path, output: str | Path) -> dict[str, Any]:
    """Verify output keeps template structure and all properties except embedded photo/preview."""
    original = load_card(template)
    result = load_card(output)
    orig_copy = json.loads(json.dumps(original))
    out_copy = json.loads(json.dumps(result))
    orig_photo = find_photo_element(orig_copy)
    out_photo = find_photo_element(out_copy)
    allowed_photo_keys = {"base64image", "originalBase64Image"}
    for key in allowed_photo_keys:
        orig_photo.pop(key, None)
        out_photo.pop(key, None)
    orig_preview = orig_copy.get("about", {}).pop("preview", None) if isinstance(orig_copy.get("about"), dict) else None
    out_preview = out_copy.get("about", {}).pop("preview", None) if isinstance(out_copy.get("about"), dict) else None
    # Text fields containing explicit personalization markers may legitimately differ
    # after employee data has been inserted. All other template properties stay strict.
    from card_personalization import TOKEN_RE
    for side in ("front", "back"):
        old_elements = orig_copy.get(side, {}).get("elements", [])
        new_elements = out_copy.get(side, {}).get("elements", [])
        if len(old_elements) != len(new_elements):
            continue
        for old_element, new_element in zip(old_elements, new_elements):
            if isinstance(old_element, dict) and isinstance(new_element, dict):
                old_text = old_element.get("text")
                if isinstance(old_text, str) and TOKEN_RE.search(old_text):
                    old_element["text"] = new_element.get("text")
    if orig_copy != out_copy:
        raise ValueError("Проверка выявила изменение структуры или свойств шаблона за пределами фотографии и персонализируемого текста. Файл не прошёл контроль.")
    return {"structure_unchanged": True, "preview_updated": orig_preview != out_preview,
            "photo_payload_updated": orig_photo != out_photo}


def extract_card_preview(card_path: str | Path, output_path: str | Path) -> Path:
    """Save the embedded HUB preview image for operator review; does not imply print proof."""
    import base64
    design = load_card(card_path)
    value = design.get("about", {}).get("preview", "")
    if not isinstance(value, str) or not value.startswith("data:image/"):
        raise ValueError("В карточке нет встроенного предпросмотра HUB.")
    try:
        raw = base64.b64decode(value.split(",", 1)[1], validate=True)
        with Image.open(io.BytesIO(raw)) as image:
            image.verify()
        target = Path(output_path)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(raw)
        return target
    except Exception as exc:
        raise ValueError("Не удалось извлечь предпросмотр из .card.") from exc
