"""Organization/card-type template profile helpers."""
from __future__ import annotations
from pathlib import Path
from hub_integration import inspect_template


def profile_key(organization: str, card_type: str) -> str:
    org = " ".join((organization or "").split()).casefold()
    kind = " ".join((card_type or "").split()).casefold()
    if not org or not kind:
        raise ValueError("Для профиля шаблона нужны организация и тип пропуска.")
    return f"{org}::{kind}"


def set_profile(profiles: dict, organization: str, card_type: str, template_path: str) -> dict:
    path = Path(template_path).expanduser().resolve()
    inspect_template(path)
    result = dict(profiles or {})
    result[profile_key(organization, card_type)] = {
        "organization": " ".join(organization.split()),
        "card_type": " ".join(card_type.split()),
        "template_path": str(path),
    }
    return result


def get_profile_template(profiles: dict, organization: str, card_type: str) -> str | None:
    item = (profiles or {}).get(profile_key(organization, card_type))
    if not isinstance(item, dict):
        return None
    path = Path(str(item.get("template_path", "")))
    if not path.is_file():
        return None
    inspect_template(path)
    return str(path.resolve())


def remove_profile(profiles: dict, organization: str, card_type: str) -> dict:
    result = dict(profiles or {})
    result.pop(profile_key(organization, card_type), None)
    return result
