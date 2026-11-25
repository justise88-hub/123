"""One-shot employee LAN sync. Set token in PSPASSMASTER_SYNC_TOKEN; never put it in config files."""
import argparse, os, sys
from pathlib import Path
from app.employee_sync import sync_once, queue_existing_employees

def main():
    p=argparse.ArgumentParser(description="PS PassMaster PRO — синхронизация сотрудников по локальной сети")
    p.add_argument("--db", required=True, help="Путь к Employees/employees.sqlite3")
    p.add_argument("--url", required=True, help="HTTPS URL сервера, например https://192.168.1.10:8765")
    p.add_argument("--ca", required=True, help="Сертификат CA/сервера для проверки TLS")
    p.add_argument("--bootstrap", action="store_true", help="Поставить существующие карточки в очередь первичной отправки")
    a=p.parse_args(); token=os.environ.get("PSPASSMASTER_SYNC_TOKEN","")
    if len(token)<24: p.error("Задайте PSPASSMASTER_SYNC_TOKEN (не менее 24 символов)")
    db=Path(a.db); ca=Path(a.ca)
    if not db.is_file(): p.error(f"База не найдена: {db}")
    if not ca.is_file(): p.error(f"Файл TLS-сертификата не найден: {ca}")
    if a.bootstrap:
        import sqlite3
        with sqlite3.connect(db) as con: queued=queue_existing_employees(con)
        print(f"Добавлено карточек в очередь: {queued}")
    try:
        result=sync_once(db,a.url,token,str(ca))
    except Exception as exc:
        print(f"Ошибка синхронизации: {exc}",file=sys.stderr); return 2
    print(f"Отправлено: {result['pushed']} | Получено: {result['pulled']} | В очереди: {result['pending']} | Конфликтов: {len(result['conflicts'])}")
    for c in result['conflicts']: print(f"КОНФЛИКТ {c['entity_id']}: версия сервера {c['current_revision']}")
    return 0 if not result['conflicts'] else 3
if __name__=="__main__": raise SystemExit(main())
