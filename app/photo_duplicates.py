"""Privacy-friendly local duplicate photo detection (exact and perceptual)."""
from __future__ import annotations
import hashlib
from pathlib import Path
from PIL import Image, ImageOps


def image_signature(path: str | Path) -> dict:
    """Return SHA-256 and 64-bit difference hash for a readable image."""
    p = Path(path)
    digest = hashlib.sha256()
    with p.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    with Image.open(p) as raw:
        image = ImageOps.exif_transpose(raw).convert("L").resize((9, 8), Image.Resampling.LANCZOS)
        pixels = list(image.tobytes())
    bits = 0
    for y in range(8):
        for x in range(8):
            bits = (bits << 1) | int(pixels[y * 9 + x] > pixels[y * 9 + x + 1])
    return {"sha256": digest.hexdigest(), "dhash": bits}


def hamming_distance(left: int, right: int) -> int:
    return (int(left) ^ int(right)).bit_count()


def find_photo_duplicates(candidate: str | Path, records, exclude_employee_id=None, max_distance=5):
    """Find existing employee photos matching exactly or visually (dHash <= max_distance)."""
    candidate_path = Path(candidate)
    if not candidate_path.is_file():
        return []
    try:
        signature = image_signature(candidate_path)
    except Exception:
        return []
    matches = []
    for record in records:
        employee_id = record["id"]
        if exclude_employee_id is not None and int(employee_id) == int(exclude_employee_id):
            continue
        other_path = Path(str(record["photo_path"] or ""))
        if not other_path.is_file() or other_path.resolve() == candidate_path.resolve():
            continue
        try:
            other = image_signature(other_path)
        except Exception:
            continue
        distance = hamming_distance(signature["dhash"], other["dhash"])
        if signature["sha256"] == other["sha256"]:
            matches.append({"id": employee_id, "name": record["full_name"], "badge": record["badge_number"], "kind": "exact", "distance": 0})
        elif distance <= max_distance:
            matches.append({"id": employee_id, "name": record["full_name"], "badge": record["badge_number"], "kind": "visual", "distance": distance})
    return sorted(matches, key=lambda item: (item["kind"] != "exact", item["distance"], str(item["name"])))
