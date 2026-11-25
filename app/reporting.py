"""Offline report generation and daily expiry-notification helpers."""
from __future__ import annotations

import csv
from collections import Counter
from datetime import date, datetime
from pathlib import Path

from employee_lifecycle import expiry_status, parse_date

STATUS_LABELS = {
    "expired": "Просрочен",
    "soon": "Истекает",
    "valid": "Действует",
    "none": "Без срока",
    "unknown": "Проверить дату",
}


def _event_date(value: str):
    value = (value or "").strip()
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).date()
    except ValueError:
        parsed = parse_date(value[:10])
        return parsed


def build_report(records, movements, start_date=None, end_date=None, today=None, warning_days=30):
    """Build a deterministic report model from active employee and movement rows."""
    today = today or date.today()
    start_date = start_date or date(today.year, today.month, 1)
    end_date = end_date or today
    if start_date > end_date:
        raise ValueError("Начальная дата отчёта позже конечной.")
    warning_days = int(warning_days)
    if not 1 <= warning_days <= 365:
        raise ValueError("Период предупреждения должен быть от 1 до 365 дней.")

    employees = [dict(row) for row in records]
    movement_rows = []
    for row in movements:
        item = dict(row)
        event_day = _event_date(item.get("created_at", ""))
        if event_day and start_date <= event_day <= end_date:
            item["event_date"] = event_day.isoformat()
            movement_rows.append(item)

    expiry_counts = Counter(expiry_status(row.get("valid_until", ""), today=today,
                                          warning_days=warning_days) for row in employees)
    department_counts = Counter((row.get("department") or "Не указано").strip() or "Не указано"
                                for row in employees)
    organization_counts = Counter((row.get("organization") or "Не указано").strip() or "Не указано"
                                  for row in employees)
    event_counts = Counter(row.get("event_type") or "Не указано" for row in movement_rows)
    expiry = []
    for row in employees:
        status = expiry_status(row.get("valid_until", ""), today=today, warning_days=warning_days)
        if status in ("expired", "soon", "unknown"):
            expiry.append({
                "full_name": row.get("full_name", ""),
                "organization": row.get("organization", ""),
                "department": row.get("department", ""),
                "position": row.get("position", ""),
                "badge_number": row.get("badge_number", ""),
                "valid_until": row.get("valid_until", ""),
                "status": status,
                "status_label": STATUS_LABELS[status],
            })
    priority = {"expired": 0, "soon": 1, "unknown": 2}
    expiry.sort(key=lambda r: (priority[r["status"]], r["valid_until"], r["full_name"].casefold()))
    return {
        "created_at": datetime.now().isoformat(timespec="seconds"),
        "period_start": start_date.isoformat(),
        "period_end": end_date.isoformat(),
        "warning_days": warning_days,
        "employee_count": len(employees),
        "expiry_counts": dict(expiry_counts),
        "department_counts": dict(sorted(department_counts.items(), key=lambda x: x[0].casefold())),
        "organization_counts": dict(sorted(organization_counts.items(), key=lambda x: x[0].casefold())),
        "event_counts": dict(sorted(event_counts.items(), key=lambda x: x[0].casefold())),
        "expiry_rows": expiry,
        "movements": movement_rows,
    }


def notification_digest(records, today=None, warning_days=30):
    """Return counts and actionable rows for a once-daily local startup reminder."""
    today = today or date.today()
    rows = []
    for row in records:
        status = expiry_status(row.get("valid_until", ""), today=today, warning_days=warning_days)
        if status in ("expired", "soon", "unknown"):
            rows.append({"full_name": row.get("full_name", ""),
                         "badge_number": row.get("badge_number", ""),
                         "valid_until": row.get("valid_until", ""),
                         "status": status, "status_label": STATUS_LABELS[status]})
    counts = Counter(row["status"] for row in rows)
    return {"total": len(rows), "expired": counts["expired"], "soon": counts["soon"],
            "unknown": counts["unknown"], "rows": rows}


def export_csv(path, report):
    """Write a UTF-8 BOM CSV workbook with summary, expiry and movement sections."""
    with open(path, "w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.writer(stream, delimiter=";")
        writer.writerow(["PS PassMaster PRO — Сводный отчёт"])
        writer.writerow(["Период", report["period_start"], report["period_end"]])
        writer.writerow(["Сотрудников", report["employee_count"]])
        writer.writerow(["Период предупреждения, дней", report["warning_days"]])
        writer.writerow([])
        writer.writerow(["Статусы сроков", "Количество"])
        for key, label in STATUS_LABELS.items():
            writer.writerow([label, report["expiry_counts"].get(key, 0)])
        writer.writerow([])
        writer.writerow(["По подразделениям", "Количество сотрудников"])
        for name, count in report["department_counts"].items():
            writer.writerow([name, count])
        writer.writerow([])
        writer.writerow(["По организациям", "Количество сотрудников"])
        for name, count in report["organization_counts"].items():
            writer.writerow([name, count])
        writer.writerow([])
        writer.writerow(["Операции за период", "Количество"])
        for name, count in report["event_counts"].items():
            writer.writerow([name, count])
        writer.writerow([])
        writer.writerow(["Очередь сроков", "Организация", "Подразделение", "Должность", "ФИО", "Номер", "Действует до", "Статус"])
        for row in report["expiry_rows"]:
            writer.writerow(["", row["organization"], row["department"], row["position"], row["full_name"], row["badge_number"], row["valid_until"], row["status_label"]])
        writer.writerow([])
        writer.writerow(["Движение пропусков", "Дата/время", "ФИО", "Номер", "Операция", "Причина/комментарий", "Оператор"])
        for row in report["movements"]:
            writer.writerow(["", row.get("created_at", ""), row.get("full_name", ""), row.get("badge_number", ""), row.get("event_type", ""), row.get("note", ""), row.get("operator", "")])
    return Path(path)


def export_xlsx(path, report):
    from openpyxl import Workbook
    from openpyxl.styles import Alignment, Font, PatternFill
    from openpyxl.utils import get_column_letter

    wb = Workbook()
    summary = wb.active
    summary.title = "Сводка"
    summary.append(["PS PassMaster PRO — Сводный отчёт"])
    summary.append(["Период", report["period_start"], report["period_end"]])
    summary.append(["Сотрудников", report["employee_count"]])
    summary.append(["Период предупреждения, дней", report["warning_days"]])
    summary.append([])
    summary.append(["Статус срока", "Количество"])
    for key, label in STATUS_LABELS.items():
        summary.append([label, report["expiry_counts"].get(key, 0)])
    summary.append([])
    summary.append(["Подразделение", "Сотрудников"])
    for name, count in report["department_counts"].items():
        summary.append([name, count])
    summary.append([])
    summary.append(["Организация", "Сотрудников"])
    for name, count in report["organization_counts"].items():
        summary.append([name, count])
    summary.append([])
    summary.append(["Операция за период", "Количество"])
    for name, count in report["event_counts"].items():
        summary.append([name, count])

    expiry = wb.create_sheet("Сроки пропусков")
    expiry.append(["ФИО", "Организация", "Подразделение", "Должность", "Номер пропуска", "Действует до", "Статус"])
    for row in report["expiry_rows"]:
        expiry.append([row["full_name"], row["organization"], row["department"], row["position"], row["badge_number"], row["valid_until"], row["status_label"]])

    movements = wb.create_sheet("Движение пропусков")
    movements.append(["Дата/время", "ФИО", "Номер пропуска", "Операция", "Комментарий", "Оператор"])
    for row in report["movements"]:
        movements.append([row.get("created_at", ""), row.get("full_name", ""), row.get("badge_number", ""), row.get("event_type", ""), row.get("note", ""), row.get("operator", "")])

    for ws in wb.worksheets:
        ws.freeze_panes = "A2"
        if ws.max_row > 1:
            ws.auto_filter.ref = ws.dimensions
        for cell in ws[1]:
            cell.font = Font(bold=True, color="252A30")
            cell.fill = PatternFill("solid", fgColor="F2C230")
            cell.alignment = Alignment(vertical="center", wrap_text=True)
        for col in ws.columns:
            letter = get_column_letter(col[0].column)
            width = min(42, max(14, max(len(str(c.value or "")) for c in col) + 2))
            ws.column_dimensions[letter].width = width
    wb.save(path)
    return Path(path)


def export_pdf(path, report, font_path):
    from reportlab.lib import colors
    from reportlab.lib.enums import TA_CENTER
    from reportlab.lib.pagesizes import A4, landscape
    from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
    from reportlab.lib.units import mm
    from reportlab.pdfbase import pdfmetrics
    from reportlab.pdfbase.ttfonts import TTFont
    from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, PageBreak

    font_path = Path(font_path)
    if not font_path.is_file():
        raise FileNotFoundError("Не найден шрифт DejaVuSans.ttf для PDF-отчёта.")
    if "PSDejaVu" not in pdfmetrics.getRegisteredFontNames():
        pdfmetrics.registerFont(TTFont("PSDejaVu", str(font_path)))
    styles = getSampleStyleSheet()
    styles.add(ParagraphStyle(name="PSBody", parent=styles["BodyText"], fontName="PSDejaVu", fontSize=8, leading=10))
    styles.add(ParagraphStyle(name="PSTitle", parent=styles["Title"], fontName="PSDejaVu", fontSize=16, leading=20, alignment=TA_CENTER))
    styles.add(ParagraphStyle(name="PSHeading", parent=styles["Heading2"], fontName="PSDejaVu", fontSize=11, leading=14))
    doc = SimpleDocTemplate(str(path), pagesize=landscape(A4), rightMargin=12*mm, leftMargin=12*mm, topMargin=12*mm, bottomMargin=12*mm)
    story = [Paragraph("PS PassMaster PRO — Сводный отчёт", styles["PSTitle"]), Spacer(1, 4*mm),
             Paragraph(f"Период: {report['period_start']} — {report['period_end']} · Сотрудников: {report['employee_count']} · Предупреждение: {report['warning_days']} дн.", styles["PSBody"]), Spacer(1, 4*mm),
             Paragraph("Сроки действия", styles["PSHeading"])]
    data = [["Статус", "Количество"]] + [[label, str(report["expiry_counts"].get(key, 0))] for key, label in STATUS_LABELS.items()]
    table = Table(data, colWidths=[75*mm, 30*mm])
    table.setStyle(TableStyle([("BACKGROUND", (0,0),(-1,0),colors.HexColor("#F2C230")),("FONTNAME",(0,0),(-1,-1),"PSDejaVu"),("FONTSIZE",(0,0),(-1,-1),8),("GRID",(0,0),(-1,-1),0.35,colors.grey),("PADDING",(0,0),(-1,-1),5)]))
    story += [table, Spacer(1, 4*mm), Paragraph("Сводка по организациям и подразделениям", styles["PSHeading"])]
    org_data = [["Организация", "Сотрудников"]] + [[name, str(count)] for name, count in report["organization_counts"].items()]
    if len(org_data) == 1: org_data.append(["Нет данных", "0"])
    dept_data = [["Подразделение", "Сотрудников"]] + [[name, str(count)] for name, count in report["department_counts"].items()]
    if len(dept_data) == 1: dept_data.append(["Нет данных", "0"])
    for data in (org_data, dept_data):
        small = Table(data, colWidths=[75*mm, 30*mm])
        small.setStyle(TableStyle([("BACKGROUND",(0,0),(-1,0),colors.HexColor("#F2C230")),("FONTNAME",(0,0),(-1,-1),"PSDejaVu"),("FONTSIZE",(0,0),(-1,-1),7),("GRID",(0,0),(-1,-1),0.3,colors.grey),("PADDING",(0,0),(-1,-1),4)]))
        story += [small, Spacer(1, 2*mm)]
    story += [Spacer(1, 2*mm), Paragraph("Очередь сроков: просроченные, истекающие и даты для проверки", styles["PSHeading"])]
    expiry_data = [["ФИО", "Организация", "Подразделение", "Номер", "Действует до", "Статус"]]
    expiry_data += [[r["full_name"], r["organization"], r["department"], r["badge_number"], r["valid_until"], r["status_label"]] for r in report["expiry_rows"]]
    if len(expiry_data) == 1: expiry_data.append(["Нет записей", "", "", "", "", ""])
    table = Table(expiry_data, repeatRows=1, colWidths=[52*mm, 42*mm, 38*mm, 25*mm, 28*mm, 35*mm])
    table.setStyle(TableStyle([("BACKGROUND",(0,0),(-1,0),colors.HexColor("#F2C230")),("FONTNAME",(0,0),(-1,-1),"PSDejaVu"),("FONTSIZE",(0,0),(-1,-1),7),("GRID",(0,0),(-1,-1),0.3,colors.grey),("VALIGN",(0,0),(-1,-1),"TOP"),("PADDING",(0,0),(-1,-1),4)]))
    story += [table, PageBreak(), Paragraph("Движение пропусков за выбранный период", styles["PSHeading"])]
    movement_data = [["Дата/время", "ФИО", "Номер", "Операция", "Комментарий", "Оператор"]]
    movement_data += [[r.get("created_at", ""), r.get("full_name", ""), r.get("badge_number", ""), r.get("event_type", ""), r.get("note", ""), r.get("operator", "")] for r in report["movements"]]
    if len(movement_data) == 1: movement_data.append(["Нет операций за период", "", "", "", "", ""])
    table = Table(movement_data, repeatRows=1, colWidths=[34*mm, 45*mm, 25*mm, 28*mm, 85*mm, 30*mm])
    table.setStyle(TableStyle([("BACKGROUND",(0,0),(-1,0),colors.HexColor("#F2C230")),("FONTNAME",(0,0),(-1,-1),"PSDejaVu"),("FONTSIZE",(0,0),(-1,-1),7),("GRID",(0,0),(-1,-1),0.3,colors.grey),("VALIGN",(0,0),(-1,-1),"TOP"),("PADDING",(0,0),(-1,-1),4)]))
    story.append(table)
    doc.build(story)
    return Path(path)
