"""Offline helpers for safe bulk card preparation and photo matching."""
from __future__ import annotations
import re
from pathlib import Path

PHOTO_EXTENSIONS = {'.jpg', '.jpeg', '.png', '.bmp', '.webp'}
MAX_BATCH_SIZE = 500


def normalize_match_key(value: str) -> str:
    """Normalize a badge number/name for conservative filename matching."""
    value = str(value or '').casefold().strip()
    return re.sub(r'[^0-9a-zа-яё]+', '', value)


def index_photo_files(folder: str | Path) -> dict[str, list[Path]]:
    """Index supported photos by normalized stem; preserve collisions as lists."""
    root = Path(folder)
    result: dict[str, list[Path]] = {}
    if not root.is_dir():
        return result
    for path in sorted(root.iterdir(), key=lambda p: p.name.casefold()):
        if path.is_file() and path.suffix.casefold() in PHOTO_EXTENSIONS:
            key = normalize_match_key(path.stem)
            if key:
                result.setdefault(key, []).append(path)
    return result


def match_employee_photo(record: dict, photo_index: dict[str, list[Path]]) -> tuple[Path | None, str]:
    """Match by badge number first, then full name; never guess ambiguous matches."""
    candidates = []
    for field in ('badge_number', 'full_name'):
        key = normalize_match_key(record.get(field, ''))
        if key and key in photo_index:
            matches = photo_index[key]
            if len(matches) == 1:
                return matches[0], f'Совпадение по {field}'
            candidates.extend(matches)
    unique = list(dict.fromkeys(candidates))
    if len(unique) > 1:
        return None, 'Неоднозначное совпадение: найдено несколько фотографий'
    if len(unique) == 1:
        return unique[0], 'Совпадение по имени файла'
    return None, 'Фотография не найдена'


def validate_batch_size(count: int, limit: int = MAX_BATCH_SIZE) -> None:
    if count < 1:
        raise ValueError('Выберите хотя бы одного сотрудника')
    if count > limit:
        raise ValueError(f'За одну операцию можно подготовить не более {limit} карточек')


def is_retryable_result(result: str) -> bool:
    """Only failed/skipped rows are eligible for retry; created rows are not."""
    return str(result or '').strip().casefold() in {'пропущено', 'ошибка', 'не создано'}
