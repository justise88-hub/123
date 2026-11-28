"""Conservative Magicard .card layout editor: edits only common visual properties."""
from __future__ import annotations
import copy, json
from pathlib import Path
from hub_integration import load_card, validate_card

EDITABLE = ("x", "y", "width", "height", "rotation", "visible", "opacity", "fill", "fontSize", "fontFamily", "fontStyle", "align", "text")
PORTRAIT_FILE_NAMES = {"images.jpg"}


def editable_elements(design: dict, side: str = "front") -> list[dict]:
    if side not in ("front", "back"):
        raise ValueError("Сторона карточки должна быть front или back.")
    section = design.get(side)
    if not isinstance(section, dict) or not isinstance(section.get("elements"), list):
        raise ValueError("В шаблоне отсутствует список элементов выбранной стороны.")
    return [e for e in section["elements"] if isinstance(e, dict)]


def apply_element_changes(design: dict, side: str, index: int, changes: dict) -> dict:
    result = copy.deepcopy(design)
    elements = editable_elements(result, side)
    if not isinstance(index, int) or index < 0 or index >= len(elements):
        raise IndexError("Элемент макета не найден.")
    element = elements[index]
    for key, value in changes.items():
        if key not in EDITABLE:
            raise ValueError(f"Свойство {key!r} нельзя изменять в редакторе.")
        if key in ("x", "y", "width", "height", "rotation", "opacity", "fontSize"):
            value = float(value)
            if key in ("width", "height") and value <= 0:
                raise ValueError("Ширина и высота должны быть больше нуля.")
            if key == "opacity" and not 0 <= value <= 1:
                raise ValueError("Прозрачность должна быть от 0 до 1.")
        if key == "visible":
            value = bool(value)
        if key in ("text", "fill", "fontFamily", "fontStyle", "align"):
            value = str(value)
        element[key] = value
    return result


def duplicate_element(design: dict, side: str, index: int) -> tuple[dict, int]:
    """Duplicate a visual element without mutating the source design."""
    result = copy.deepcopy(design)
    elements = editable_elements(result, side)
    if not isinstance(index, int) or index < 0 or index >= len(elements):
        raise IndexError("Элемент макета не найден.")
    clone = copy.deepcopy(elements[index])
    if str(clone.get("fileName", "")).lower() in PORTRAIT_FILE_NAMES:
        raise ValueError("Фото сотрудника нельзя дублировать как обычное изображение.")
    clone["x"] = float(clone.get("x", 0)) + 12
    clone["y"] = float(clone.get("y", 0)) + 12
    raw = result[side]["elements"]
    position = [i for i, item in enumerate(raw) if isinstance(item, dict)][index]
    raw.insert(position + 1, clone)
    return result, index + 1


def remove_element(design: dict, side: str, index: int) -> dict:
    """Remove a non-portrait visual element; required employee photo is protected."""
    result = copy.deepcopy(design)
    elements = editable_elements(result, side)
    if not isinstance(index, int) or index < 0 or index >= len(elements):
        raise IndexError("Элемент макета не найден.")
    if str(elements[index].get("fileName", "")).lower() in PORTRAIT_FILE_NAMES:
        raise ValueError("Обязательную фотографию сотрудника удалить нельзя.")
    raw = result[side]["elements"]
    position = [i for i, item in enumerate(raw) if isinstance(item, dict)][index]
    del raw[position]
    return result


def add_text_element(design: dict, side: str = "front", text: str = "Новый текст") -> tuple[dict, int]:
    """Add a text element by cloning the template's own text schema for HUB compatibility."""
    result = copy.deepcopy(design)
    elements = editable_elements(result, side)
    prototype = next((e for e in elements if str(e.get("name", "")).lower() == "text" and "text" in e), None)
    if prototype is None:
        raise ValueError("В выбранной стороне нет текстового элемента, по которому можно создать новый.")
    clone = copy.deepcopy(prototype)
    clone["text"] = str(text)[:500]
    clone["x"] = float(clone.get("x", 0)) + 20
    clone["y"] = float(clone.get("y", 0)) + 20
    result[side]["elements"].append(clone)
    return result, len(elements)


def save_template_copy(source: str | Path, destination: str | Path, design: dict) -> Path:
    source_path, target = Path(source).resolve(), Path(destination).resolve()
    if source_path == target:
        raise ValueError("Нельзя сохранять изменения поверх оригинального шаблона. Выберите новое имя файла.")
    validate_card(design)
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_name(target.name + ".tmp")
    try:
        temp.write_text(json.dumps(design, ensure_ascii=False, indent=2), encoding="utf-8")
        validate_card(json.loads(temp.read_text(encoding="utf-8")))
        temp.replace(target)
    finally:
        temp.unlink(missing_ok=True)
    return target

def replace_image_payload(design: dict, side: str, index: int, image_path: str | Path) -> dict:
    """Replace a non-portrait image element payload while preserving its HUB geometry."""
    import base64, io
    from PIL import Image, ImageOps
    result = copy.deepcopy(design)
    elements = editable_elements(result, side)
    if not isinstance(index, int) or index < 0 or index >= len(elements):
        raise IndexError("Элемент макета не найден.")
    element = elements[index]
    if str(element.get("fileName", "")).lower() == "images.jpg":
        raise ValueError("Фотографию сотрудника заменяйте через карточку сотрудника, а не через логотип.")
    if str(element.get("name", "")).lower() != "image" and "base64image" not in element:
        raise ValueError("Выбранный элемент не является изображением.")
    source = Path(image_path)
    if not source.is_file():
        raise ValueError("Файл изображения не найден.")
    with Image.open(source) as im:
        im = ImageOps.exif_transpose(im).convert("RGBA")
        if im.width < 16 or im.height < 16 or im.width > 10000 or im.height > 10000:
            raise ValueError("Недопустимые размеры изображения логотипа.")
        buf = io.BytesIO(); im.save(buf, format="PNG", optimize=True)
        uri = "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode("ascii")
        aspect = im.width / im.height
    element["base64image"] = uri
    element["originalBase64Image"] = uri
    element["aspectRatio"] = str(aspect)
    return result


# Optional GUI layer; geometry helpers remain importable in headless test environments.
try:
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QColor, QBrush, QPen, QFont
    from PySide6.QtWidgets import (QDialog, QVBoxLayout, QHBoxLayout, QFormLayout, QLabel,
        QPushButton, QComboBox, QGraphicsScene, QGraphicsView, QGraphicsRectItem,
        QGraphicsTextItem, QDoubleSpinBox, QLineEdit, QCheckBox, QFileDialog,
        QMessageBox, QSplitter, QWidget, QInputDialog, QGraphicsPixmapItem)
    _QT_AVAILABLE = True
except ImportError:
    _QT_AVAILABLE = False

if _QT_AVAILABLE:
    class CardDesignerDialog(QDialog):
        """Drag visual elements on a preview, edit text and save a separate .card copy."""
        SCALE = 0.62
        def __init__(self, template_path: str, parent=None):
            super().__init__(parent)
            self.source = Path(template_path).resolve()
            self.design = load_card(self.source)
            self.side = "front"
            self.graphics: dict[int, QGraphicsRectItem] = {}
            self.current_index = None
            self.setWindowTitle("Редактор макета Magicard — PS GuardPRO")
            self.resize(1120, 720)
            root = QVBoxLayout(self)
            top = QHBoxLayout(); top.addWidget(QLabel(f"Шаблон: {self.source.name}")); top.addStretch()
            self.side_box = QComboBox(); self.side_box.addItem("Лицевая сторона", "front"); self.side_box.addItem("Оборотная сторона", "back")
            self.side_box.currentIndexChanged.connect(self.rebuild_scene); top.addWidget(self.side_box)
            root.addLayout(top)
            split = QSplitter(Qt.Horizontal)
            self.scene = QGraphicsScene(self)
            self.view = QGraphicsView(self.scene); self.view.setMinimumWidth(600); self.view.setDragMode(QGraphicsView.RubberBandDrag)
            split.addWidget(self.view)
            panel = QWidget(); form = QFormLayout(panel)
            self.element_label = QLabel("Выберите элемент на макете")
            self.text_edit = QLineEdit()
            self.x = self._spin(-10000,10000); self.y = self._spin(-10000,10000)
            self.w = self._spin(1,10000); self.h = self._spin(1,10000)
            self.rotation = self._spin(-360,360)
            self.font_size = self._spin(1,500)
            self.fill = QLineEdit()
            self.visible = QCheckBox("Показывать элемент")
            for label, widget in (("Выбранный элемент",self.element_label),("Текст",self.text_edit),("X",self.x),("Y",self.y),("Ширина",self.w),("Высота",self.h),("Поворот",self.rotation),("Размер шрифта",self.font_size),("Цвет",self.fill),("Видимость",self.visible)):
                form.addRow(label,widget)
            apply_btn=QPushButton("Применить свойства"); apply_btn.clicked.connect(self.apply_controls); form.addRow(apply_btn)
            logo_btn=QPushButton("Заменить выбранное изображение / логотип…"); logo_btn.clicked.connect(self.replace_selected_image); form.addRow(logo_btn)
            add_text_btn=QPushButton("Добавить текстовое поле…"); add_text_btn.clicked.connect(self.add_text); form.addRow(add_text_btn)
            duplicate_btn=QPushButton("Дублировать выбранный элемент"); duplicate_btn.clicked.connect(self.duplicate_selected); form.addRow(duplicate_btn)
            delete_btn=QPushButton("Удалить выбранный элемент"); delete_btn.clicked.connect(self.delete_selected); form.addRow(delete_btn)
            form.addRow(QLabel("Перемещайте элементы мышью на макете. Текстовые поля поддерживают {{FULL_NAME}}, {{ORGANIZATION}}, {{POSITION}}, {{BADGE_NUMBER}}, {{ISSUE_DATE}}, {{VALID_UNTIL}}, {{DEPARTMENT}} и {{CARD_TYPE}}."))
            split.addWidget(panel); split.setSizes([760,360]); root.addWidget(split,1)
            bottom=QHBoxLayout(); save=QPushButton("Сохранить как новый шаблон .card"); save.clicked.connect(self.save_copy)
            close=QPushButton("Закрыть"); close.clicked.connect(self.reject)
            bottom.addWidget(save); bottom.addStretch(); bottom.addWidget(close); root.addLayout(bottom)
            self.scene.selectionChanged.connect(self.selection_changed)
            self.rebuild_scene()
    
        @staticmethod
        def _spin(minimum, maximum):
            box=QDoubleSpinBox(); box.setRange(minimum,maximum); box.setDecimals(1); box.setSingleStep(1); return box
    
        def rebuild_scene(self, *_):
            self.side=self.side_box.currentData(); self.scene.clear(); self.graphics.clear(); self.current_index=None
            elements=editable_elements(self.design,self.side)
            about = self.design.get("about", {}) if isinstance(self.design.get("about", {}), dict) else {}
            card_type = about.get("type", {}) if isinstance(about.get("type", {}), dict) else {}
            width = float(card_type.get("widthPixels", 1013) or 1013)
            height = float(card_type.get("heightPixels", 642) or 642)
            if not (100 <= width <= 10000 and 60 <= height <= 10000):
                width, height = 1013.0, 642.0
            self.scene.setSceneRect(0,0,width*self.SCALE,height*self.SCALE)
            card=QGraphicsRectItem(0,0,width*self.SCALE,height*self.SCALE); card.setBrush(QBrush(QColor('#f8f8f4'))); card.setPen(QPen(QColor('#444444'),2)); card.setZValue(-100); self.scene.addItem(card)
            preview = about.get("preview", "")
            if isinstance(preview, str) and preview.startswith("data:image/") and "," in preview:
                try:
                    import base64
                    pixmap = QPixmap()
                    if pixmap.loadFromData(base64.b64decode(preview.split(",", 1)[1], validate=True)):
                        pix_item = QGraphicsPixmapItem(pixmap)
                        pix_item.setTransformationMode(Qt.SmoothTransformation)
                        pix_item.setScale(min(width*self.SCALE/pixmap.width(), height*self.SCALE/pixmap.height()))
                        pix_item.setZValue(-99)
                        self.scene.addItem(pix_item)
                except Exception:
                    pass
            for i,e in enumerate(elements):
                x=float(e.get('x',0))*self.SCALE; y=float(e.get('y',0))*self.SCALE
                w=max(8,float(e.get('width',80))*self.SCALE); h=max(8,float(e.get('height',40))*self.SCALE)
                item=QGraphicsRectItem(0,0,w,h); item.setPos(x,y); item.setData(0,i); item.setFlag(QGraphicsRectItem.ItemIsMovable,True); item.setFlag(QGraphicsRectItem.ItemIsSelectable,True)
                item.setBrush(QBrush(QColor(255,230,100,45))); item.setPen(QPen(QColor('#d19b22'),1.5,Qt.DashLine)); self.scene.addItem(item); self.graphics[i]=item
                label=str(e.get('text') or e.get('displayName') or e.get('name') or ('Фото' if str(e.get('fileName','')).lower()=='images.jpg' else 'Изображение'))
                child=QGraphicsTextItem(label[:90],item); child.setDefaultTextColor(QColor(str(e.get('fill','#222222')) if str(e.get('fill','#222222')).startswith('#') else '#222222')); child.setFont(QFont(str(e.get('fontFamily','Arial')),max(7,int(float(e.get('fontSize',14) or 14)*self.SCALE))))
                child.setTextWidth(max(30,w-4)); child.setPos(2,2); child.setAcceptedMouseButtons(Qt.NoButton)
                if not e.get('visible',True): item.setOpacity(0.25)
                item.setRotation(float(e.get('rotation',0) or 0))
            self.view.fitInView(self.scene.sceneRect(),Qt.KeepAspectRatio)
    
        def selection_changed(self):
            selected=self.scene.selectedItems()
            if not selected: return
            index=selected[0].data(0)
            if index is None: return
            self.current_index=int(index); e=editable_elements(self.design,self.side)[self.current_index]
            self.element_label.setText(str(e.get('displayName') or e.get('name') or f"Элемент {index+1}"))
            self.text_edit.setText(str(e.get('text','')))
            item=self.graphics[self.current_index]; pos=item.pos()
            self.x.setValue(pos.x()/self.SCALE); self.y.setValue(pos.y()/self.SCALE)
            self.w.setValue(item.rect().width()/self.SCALE); self.h.setValue(item.rect().height()/self.SCALE)
            self.rotation.setValue(item.rotation()); self.font_size.setValue(float(e.get('fontSize',12) or 12)); self.fill.setText(str(e.get('fill','#000000'))); self.visible.setChecked(bool(e.get('visible',True)))
    
        def apply_controls(self):
            if self.current_index is None: return
            changes={'x':self.x.value(),'y':self.y.value(),'width':self.w.value(),'height':self.h.value(),'rotation':self.rotation.value(),'fontSize':self.font_size.value(),'fill':self.fill.text(),'visible':self.visible.isChecked()}
            if 'text' in editable_elements(self.design,self.side)[self.current_index]: changes['text']=self.text_edit.text()
            try:
                selected_index = self.current_index
                self.design=apply_element_changes(self.design,self.side,selected_index,changes)
                self.rebuild_scene(); self.graphics[selected_index].setSelected(True)
            except Exception as exc: QMessageBox.warning(self,"Редактор макета",str(exc))
    
        def add_text(self):
            value, ok = QInputDialog.getText(self, "Новое текстовое поле", "Текст или переменные сотрудника:")
            if not ok:
                return
            try:
                self.design, index = add_text_element(self.design, self.side, value or "Новый текст")
                self.rebuild_scene()
                self.graphics[index].setSelected(True)
            except Exception as exc:
                QMessageBox.warning(self, "Добавление текста", str(exc))

        def duplicate_selected(self):
            if self.current_index is None:
                QMessageBox.information(self, "Редактор макета", "Сначала выберите элемент.")
                return
            try:
                self.design, index = duplicate_element(self.design, self.side, self.current_index)
                self.rebuild_scene()
                self.graphics[index].setSelected(True)
            except Exception as exc:
                QMessageBox.warning(self, "Дублирование элемента", str(exc))

        def delete_selected(self):
            if self.current_index is None:
                QMessageBox.information(self, "Редактор макета", "Сначала выберите элемент.")
                return
            element = editable_elements(self.design, self.side)[self.current_index]
            if str(element.get("fileName", "")).lower() in PORTRAIT_FILE_NAMES:
                QMessageBox.warning(self, "Защита фотографии", "Обязательную фотографию сотрудника удалить нельзя.")
                return
            answer = QMessageBox.question(self, "Удалить элемент", "Удалить выбранный элемент из новой копии шаблона?")
            if answer != QMessageBox.Yes:
                return
            try:
                self.design = remove_element(self.design, self.side, self.current_index)
                self.current_index = None
                self.rebuild_scene()
            except Exception as exc:
                QMessageBox.warning(self, "Удаление элемента", str(exc))

        def replace_selected_image(self):
            if self.current_index is None:
                QMessageBox.information(self, "Логотип", "Сначала выберите изображение на макете."); return
            path, _ = QFileDialog.getOpenFileName(self, "Выберите логотип или изображение", "", "Изображения (*.png *.jpg *.jpeg *.webp *.bmp)")
            if not path: return
            try:
                self.design = replace_image_payload(self.design, self.side, self.current_index, path)
                QMessageBox.information(self, "Логотип", "Изображение заменено. Размер и положение элемента сохранены.")
                self.rebuild_scene()
            except Exception as exc:
                QMessageBox.warning(self, "Не удалось заменить изображение", str(exc))

        def save_copy(self):
            # Capture any mouse-dragged positions before serialization.
            for index,item in self.graphics.items():
                self.design=apply_element_changes(self.design,self.side,index,{'x':item.pos().x()/self.SCALE,'y':item.pos().y()/self.SCALE,'rotation':item.rotation()})
            default=self.source.with_name(self.source.stem+'_edited.card')
            path,_=QFileDialog.getSaveFileName(self,"Сохранить копию шаблона Magicard",str(default),"Шаблоны Magicard (*.card)")
            if not path: return
            if not path.lower().endswith('.card'): path += '.card'
            try:
                target=save_template_copy(self.source,path,self.design)
                QMessageBox.information(self,"Редактор макета",f"Новая копия шаблона сохранена:\n{target}\n\nОригинал не изменён. Перед печатью проверьте макет в Magicard HUB.")
                self.accept()
            except Exception as exc: QMessageBox.critical(self,"Не удалось сохранить шаблон",str(exc))
else:
    class CardDesignerDialog:
        def __init__(self, *args, **kwargs):
            raise RuntimeError("Для визуального редактора требуется PySide6.")
