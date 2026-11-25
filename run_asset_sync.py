"""One-shot asset manifest sync for PS PassMaster PRO. Token is read from environment."""
import argparse
import os
import sys
from pathlib import Path

from app.asset_manifest_sync import scan_assets, sync_assets


def main() -> int:
    parser = argparse.ArgumentParser(description="PS PassMaster PRO — синхронизация фотографий и шаблонов Magicard")
    parser.add_argument("--db", required=True, help="Путь к локальной базе SQLite приложения")
    parser.add_argument("--assets", required=True, help="Корневой каталог фотографий и .card шаблонов")
    parser.add_argument("--url", required=True, help="HTTPS-адрес главного компьютера/сервера")
    parser.add_argument("--ca", required=True, help="CA-сертификат для проверки TLS")
    parser.add_argument("--scan", action="store_true", help="Просканировать каталог и поставить новые файлы в очередь")
    args = parser.parse_args()
    token = os.environ.get("PSPASSMASTER_SYNC_TOKEN", "")
    if len(token) < 24:
        parser.error("Задайте PSPASSMASTER_SYNC_TOKEN (не менее 24 символов)")
    if not Path(args.db).is_file():
        parser.error(f"База не найдена: {args.db}")
    if not Path(args.assets).is_dir():
        parser.error(f"Каталог вложений не найден: {args.assets}")
    if not Path(args.ca).is_file():
        parser.error(f"TLS-сертификат не найден: {args.ca}")
    try:
        if args.scan:
            print(f"Добавлено в очередь: {scan_assets(args.db, args.assets)}")
        result = sync_assets(args.db, args.assets, args.url, token, args.ca)
        print("Отправлено: {pushed} | Получено: {pulled} | В очереди: {pending} | Конфликтов: {conflicts}".format(
            **{**result, "conflicts": len(result["conflicts"])}))
        for item in result["conflicts"]:
            print("КОНФЛИКТ:", item)
        return 0 if not result["conflicts"] else 3
    except Exception as exc:
        print(f"Ошибка синхронизации: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
