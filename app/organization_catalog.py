"""Validation helpers for the local organization directory."""

MAX_ORGANIZATION_NAME = 120


def normalize_organization_name(value: str) -> str:
    """Collapse whitespace and validate an organization name."""
    name = " ".join((value or "").split())
    if not name:
        raise ValueError("Введите название организации.")
    if len(name) > MAX_ORGANIZATION_NAME:
        raise ValueError("Название не должно превышать 120 символов.")
    return name
