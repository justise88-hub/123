"""Offline photo quality checks for employee badge portraits."""
from __future__ import annotations
from pathlib import Path
from PIL import Image, ImageOps, ImageStat


def assess_photo(path: str | Path, min_width: int = 450, min_height: int = 600) -> dict:
    p = Path(path)
    result = {"ok": False, "errors": [], "warnings": [], "width": 0, "height": 0,
              "megapixels": 0.0, "brightness": 0.0, "contrast": 0.0, "face_detected": None}
    if not p.is_file():
        result["errors"].append("Файл фотографии не найден")
        return result
    try:
        with Image.open(p) as raw:
            raw.verify()
        with Image.open(p) as raw:
            im = ImageOps.exif_transpose(raw).convert("RGB")
            result["width"], result["height"] = im.size
            result["megapixels"] = round(im.width * im.height / 1_000_000, 2)
            gray = im.convert("L")
            stats = ImageStat.Stat(gray)
            result["brightness"] = round(stats.mean[0], 1)
            result["contrast"] = round(stats.stddev[0], 1)
            if im.width < min_width or im.height < min_height:
                result["errors"].append(f"Низкое разрешение: {im.width}×{im.height}; рекомендуется не менее {min_width}×{min_height}")
            if stats.mean[0] < 45:
                result["warnings"].append("Фотография может быть слишком тёмной")
            elif stats.mean[0] > 215:
                result["warnings"].append("Фотография может быть пересвечена")
            if stats.stddev[0] < 22:
                result["warnings"].append("Низкий контраст изображения")
            im.thumbnail((900, 900))
            try:
                import cv2
                import numpy as np
                gray_cv = cv2.cvtColor(np.asarray(im), cv2.COLOR_RGB2GRAY)
                blur = float(cv2.Laplacian(gray_cv, cv2.CV_64F).var())
                result["sharpness"] = round(blur, 1)
                if blur < 35:
                    result["warnings"].append("Возможно, фотография смазана")
                cascade = cv2.CascadeClassifier(str(Path(cv2.data.haarcascades) / "haarcascade_frontalface_default.xml"))
                faces = cascade.detectMultiScale(gray_cv, scaleFactor=1.1, minNeighbors=5, minSize=(30, 30))
                result["face_detected"] = bool(len(faces))
                if not len(faces):
                    result["warnings"].append("Лицо автоматически не обнаружено — проверьте кадрирование вручную")
                elif len(faces) > 1:
                    result["warnings"].append("Обнаружено несколько лиц — проверьте, что выбрано нужное")
            except ImportError:
                result["warnings"].append("Проверка лица и резкости недоступна: OpenCV не установлен")
            finally:
                im.close()
    except Exception as exc:
        result["errors"].append(f"Файл повреждён или не читается: {exc}")
    result["ok"] = not result["errors"]
    return result


def format_report(report: dict) -> str:
    lines = [f"Размер: {report.get('width', 0)}×{report.get('height', 0)} px",
             f"Яркость: {report.get('brightness', 0)} · Контраст: {report.get('contrast', 0)}"]
    if report.get("sharpness") is not None:
        lines.append(f"Резкость: {report['sharpness']}")
    lines.extend("ОШИБКА: " + x for x in report.get("errors", []))
    lines.extend("Предупреждение: " + x for x in report.get("warnings", []))
    if report.get("ok") and not report.get("warnings"):
        lines.append("Критических замечаний не обнаружено")
    return "\n".join(lines)
