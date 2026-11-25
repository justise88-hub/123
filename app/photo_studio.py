"""Offline portrait background replacement using OpenCV GrabCut."""
from __future__ import annotations
from PIL import Image, ImageFilter

BACKGROUNDS = {
    "Оригинал": None,
    "Белый": (255, 255, 255),
    "Светло-серый": (232, 235, 238),
    "Серый": (190, 195, 200),
}


def replace_background(image: Image.Image, color_name: str) -> Image.Image:
    """Return RGB portrait with a neutral background; raises if segmentation unavailable."""
    color = BACKGROUNDS.get(color_name)
    source = image.convert("RGB")
    if color is None:
        return source.copy()
    try:
        import cv2
        import numpy as np
    except ImportError as exc:
        raise RuntimeError("Для замены фона требуется установленный OpenCV") from exc

    # Work on a bounded preview-sized copy for predictable memory and speed.
    scale = min(1.0, 1200 / max(source.size))
    size = (max(2, round(source.width * scale)), max(2, round(source.height * scale)))
    small = source.resize(size, Image.Resampling.LANCZOS) if size != source.size else source
    rgb = np.asarray(small)
    h, w = rgb.shape[:2]
    if min(w, h) < 40:
        raise RuntimeError("Фотография слишком мала для автоматического отделения фона")
    mask = np.zeros((h, w), np.uint8)
    bg_model = np.zeros((1, 65), np.float64)
    fg_model = np.zeros((1, 65), np.float64)
    margin_x, margin_y = max(2, int(w * .035)), max(2, int(h * .025))
    rect = (margin_x, margin_y, max(1, w - 2 * margin_x), max(1, h - 2 * margin_y))
    cv2.grabCut(cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR), mask, rect, bg_model, fg_model, 5, cv2.GC_INIT_WITH_RECT)
    alpha = np.where((mask == cv2.GC_FGD) | (mask == cv2.GC_PR_FGD), 255, 0).astype(np.uint8)
    alpha = cv2.medianBlur(alpha, 5)
    alpha_image = Image.fromarray(alpha, "L").filter(ImageFilter.GaussianBlur(radius=max(1, round(max(w, h) / 500))))
    foreground = small.convert("RGBA")
    foreground.putalpha(alpha_image)
    background = Image.new("RGBA", small.size, (*color, 255))
    result = Image.alpha_composite(background, foreground).convert("RGB")
    if result.size != source.size:
        result = result.resize(source.size, Image.Resampling.LANCZOS)
    if small is not source:
        small.close()
    return result
