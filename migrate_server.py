"""Create, verify or restore a PS PassMaster PRO server migration bundle."""
import argparse
from pathlib import Path
from app.server_migration import create_migration_bundle, verify_migration_bundle, restore_migration_bundle


def main():
    parser = argparse.ArgumentParser(description="PS PassMaster PRO — перенос серверных данных")
    sub = parser.add_subparsers(dest="command", required=True)
    exp = sub.add_parser("export", help="Создать пакет переноса")
    exp.add_argument("--sync-db", required=True, type=Path, help="SQLite-журнал LAN-сервиса")
    exp.add_argument("--output", required=True, type=Path, help="Путь к ZIP-пакету")
    exp.add_argument("--workspace", default="default")
    exp.add_argument("--employee-db", type=Path, help="Локальная employees.sqlite3 (необязательно)")
    exp.add_argument("--employee-files", type=Path, help="Папка Employees с фотографиями/файлами (необязательно)")
    exp.add_argument("--sync-files", type=Path, help="Каталог sync_files сервера с загруженными вложениями (необязательно)")
    ver = sub.add_parser("verify", help="Проверить пакет без восстановления")
    ver.add_argument("bundle", type=Path)
    imp = sub.add_parser("restore", help="Восстановить пакет в новую папку сервера")
    imp.add_argument("bundle", type=Path)
    imp.add_argument("--destination", required=True, type=Path)
    args = parser.parse_args()
    if args.command == "export":
        result = create_migration_bundle(sync_db=args.sync_db, output_zip=args.output,
            employee_db=args.employee_db, employee_files=args.employee_files,
            sync_files=args.sync_files, workspace=args.workspace)
        print(f"Пакет создан: {args.output}\nФайлов: {len(result['files'])}\nРабочая группа: {result['workspace']}")
    elif args.command == "verify":
        result = verify_migration_bundle(args.bundle)
        print(f"Проверка пройдена. Файлов: {len(result['files'])}; рабочая группа: {result['workspace']}")
    else:
        result = restore_migration_bundle(args.bundle, args.destination)
        print("Восстановление завершено:")
        for key, value in result.items(): print(f"{key}: {value}")
        print("Создайте новый TLS-сертификат и задайте PSPASSMASTER_SYNC_TOKEN отдельно на новом сервере.")

if __name__ == "__main__":
    main()
