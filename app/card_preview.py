"""Offline visual proof and layout checks for Magicard HUB .card files."""
from __future__ import annotations
import base64, io, math, textwrap
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageOps


def _number(value, default=0.0):
    try:
        v=float(value)
        return v if math.isfinite(v) else default
    except (TypeError, ValueError):
        return default


def _font(size, bold=False):
    candidates = (["DejaVuSans-Bold.ttf", "arialbd.ttf"] if bold else ["DejaVuSans.ttf", "arial.ttf"])
    for name in candidates:
        try: return ImageFont.truetype(name, max(6, int(size)))
        except OSError: pass
    return ImageFont.load_default()


def _decode_image(value):
    if not isinstance(value, str) or not value.startswith("data:image/") or "," not in value:
        return None
    try:
        raw=base64.b64decode(value.split(",",1)[1], validate=True)
        im=Image.open(io.BytesIO(raw)); im.load(); return im.convert("RGBA")
    except Exception:
        return None


def render_card_preview(design: dict, side: str="front", size=(1013,642), photo_path: str|Path|None=None):
    """Render common text/image elements for review; this is not a HUB print renderer."""
    if side not in ("front","back"): raise ValueError("Неизвестная сторона карточки")
    width,height=map(int,size)
    if width<100 or height<60 or width>4000 or height>4000: raise ValueError("Недопустимый размер предпросмотра")
    canvas=Image.new("RGBA",(width,height),(248,248,245,255))
    section=design.get(side,{})
    bg=section.get("background") if isinstance(section,dict) else None
    if isinstance(bg,str) and bg.startswith("#"):
        try: canvas.paste(Image.new("RGBA",canvas.size,bg))
        except Exception: pass
    elif isinstance(bg,dict):
        color=bg.get("fill") or bg.get("color")
        if isinstance(color,str) and color.startswith("#"):
            try: canvas.paste(Image.new("RGBA",canvas.size,color))
            except Exception: pass
    draw=ImageDraw.Draw(canvas)
    issues=[]
    elements=section.get("elements",[]) if isinstance(section,dict) else []
    # Coordinates are stored in HUB's 1013x642 design coordinate system in supplied CR80 templates.
    sx,sy=width/1013.0,height/642.0
    for idx,el in enumerate(elements):
        if not isinstance(el,dict) or el.get("visible",True) is False: continue
        x,y=_number(el.get("x"))*sx,_number(el.get("y"))*sy
        w,h=_number(el.get("width",80))*sx,_number(el.get("height",40))*sy
        if w<=0 or h<=0: issues.append(f"Элемент {idx+1}: нулевая или отрицательная область"); continue
        x2,y2=x+w,y+h
        if x<0 or y<0 or x2>width or y2>height:
            issues.append(f"Элемент {idx+1} ({el.get('displayName') or el.get('name') or 'без имени'}): выходит за границы карточки")
        box=(int(max(0,x)),int(max(0,y)),int(min(width,x2)),int(min(height,y2)))
        if box[2]<=box[0] or box[3]<=box[1]: continue
        name=str(el.get("name","")).lower()
        image_value=el.get("base64image")
        if str(el.get("fileName","")).lower()=="images.jpg" and photo_path:
            try:
                with Image.open(photo_path) as p: obj=ImageOps.exif_transpose(p).convert("RGBA")
                image_value=None
            except Exception:
                obj=None
                issues.append("Фотография сотрудника не читается")
        else:
            obj=_decode_image(image_value)
        if obj is not None:
            try:
                obj=ImageOps.fit(obj,(max(1,box[2]-box[0]),max(1,box[3]-box[1])),method=Image.Resampling.LANCZOS)
                canvas.alpha_composite(obj,(box[0],box[1]))
            finally: obj.close()
        elif name=="text" or isinstance(el.get("text"),str):
            content=str(el.get("text", ""))
            color=str(el.get("fill", "#202020"))
            if not (len(color) in (4,7) and color.startswith("#")): color="#202020"
            font=_font(_number(el.get("fontSize",18))*sy, "bold" in str(el.get("fontStyle", "")).lower())
            align=str(el.get("align","left")).lower()
            # PIL anchors and per-line clipping provide a conservative, readable proof.
            lines=content.splitlines() or [""]
            line_h=max(8,int(_number(el.get("fontSize",18))*sy*1.25))
            for line_no,line in enumerate(lines):
                yy=box[1]+line_no*line_h
                if yy>=box[3]:
                    issues.append(f"Элемент {idx+1}: текст не помещается по высоте")
                    break
                bbox=draw.textbbox((0,0),line,font=font)
                text_w=bbox[2]-bbox[0]
                if text_w>box[2]-box[0]:
                    issues.append(f"Элемент {idx+1}: строка текста шире поля")
                xx=box[0]
                if align=="center": xx=box[0]+max(0,(box[2]-box[0]-text_w)//2)
                elif align=="right": xx=box[2]-text_w
                draw.text((xx,yy),line,font=font,fill=color)
        if _number(el.get("rotation",0)):
            issues.append(f"Элемент {idx+1}: поворот отображён приблизительно; сверьте в HUB")
    # Draw a subtle proof boundary.
    draw=ImageDraw.Draw(canvas)
    draw.rectangle((0,0,width-1,height-1),outline=(90,90,90,255),width=max(1,width//1000))
    return canvas.convert("RGB"), issues


def inspect_card_layout(design: dict, side: str="front") -> list[str]:
    """Return geometry/text warnings without modifying the source design."""
    if side not in ("front","back"): raise ValueError("Неизвестная сторона карточки")
    issues=[]
    section=design.get(side,{})
    for i,e in enumerate(section.get("elements",[]) if isinstance(section,dict) else []):
        if not isinstance(e,dict): continue
        x,y=_number(e.get("x")),_number(e.get("y")); w,h=_number(e.get("width")),_number(e.get("height"))
        label=str(e.get("displayName") or e.get("name") or f"Элемент {i+1}")
        if w<=0 or h<=0: issues.append(f"{label}: неверные размеры")
        if x<0 or y<0 or x+w>1013 or y+h>642: issues.append(f"{label}: элемент выходит за пределы CR80 (1013×642)")
        if isinstance(e.get("text"),str) and e["text"].strip() and _number(e.get("fontSize",12))>h*1.8:
            issues.append(f"{label}: размер шрифта может не помещаться по высоте")
    return issues


try:
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QPixmap, QImage
    from PySide6.QtWidgets import QDialog,QVBoxLayout,QHBoxLayout,QLabel,QComboBox,QTextEdit,QPushButton,QScrollArea,QWidget
    _QT=True
except ImportError: _QT=False

if _QT:
    class CardPreviewDialog(QDialog):
        def __init__(self, design:dict, employee:dict, photo_path=None, parent=None):
            super().__init__(parent)
            self.design=design; self.employee=employee; self.photo_path=photo_path
            self.setWindowTitle("Предпросмотр пропуска — PS PassMaster PRO")
            self.resize(1050,760)
            root=QVBoxLayout(self)
            top=QHBoxLayout(); top.addWidget(QLabel(f"Сотрудник: {employee.get('full_name','—')} · № {employee.get('badge_number','—')}")); top.addStretch()
            self.side=QComboBox(); self.side.addItem("Лицевая сторона","front"); self.side.addItem("Оборотная сторона","back"); self.side.currentIndexChanged.connect(self.refresh)
            top.addWidget(self.side); root.addLayout(top)
            self.image=QLabel(alignment=Qt.AlignCenter); self.image.setMinimumSize(700,440); self.image.setStyleSheet("background:#dedede;")
            scroll=QScrollArea(); scroll.setWidgetResizable(True); scroll.setWidget(self.image); root.addWidget(scroll,1)
            self.report=QTextEdit(); self.report.setReadOnly(True); self.report.setMaximumHeight(125); root.addWidget(self.report)
            buttons=QHBoxLayout(); buttons.addStretch(); close=QPushButton("Закрыть"); close.clicked.connect(self.accept); buttons.addWidget(close); root.addLayout(buttons)
            self.refresh()
        def refresh(self,*_):
            side=self.side.currentData()
            im,issues=render_card_preview(self.design,side,photo_path=self.photo_path)
            raw=im.tobytes(); q=QImage(raw,im.width,im.height,im.width*3,QImage.Format_RGB888).copy()
            self.image.setPixmap(QPixmap.fromImage(q).scaled(940,600,Qt.KeepAspectRatio,Qt.SmoothTransformation))
            layout=inspect_card_layout(self.design,side)
            lines=["Проверка макета (предварительная):"]
            lines.extend("• "+x for x in (layout+issues))
            if not layout and not issues: lines.append("Критических геометрических замечаний не обнаружено. Проверьте итоговый файл в Magicard HUB перед печатью.")
            self.report.setPlainText("\n".join(lines))
else:
    class CardPreviewDialog:
        def __init__(self,*args,**kwargs): raise RuntimeError("Для графического предпросмотра требуется PySide6")
