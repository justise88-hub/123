"""Start the opt-in PS GuardPRO LAN change-log service on the host PC."""
import argparse, os
from pathlib import Path
from app.network_sync import ChangeStore, SyncHTTPServer

def main():
    p=argparse.ArgumentParser(description="PS GuardPRO LAN sync service (TLS required)")
    p.add_argument("--host",default="0.0.0.0"); p.add_argument("--port",type=int,default=8765)
    p.add_argument("--db",default="ServerData/pspassmaster_sync.sqlite3")
    p.add_argument("--workspace",default="default"); p.add_argument("--cert",required=True); p.add_argument("--key",required=True)
    a=p.parse_args(); token=os.environ.get("PSPASSMASTER_SYNC_TOKEN","")
    if len(token)<24: p.error("Задайте переменную среды PSPASSMASTER_SYNC_TOKEN (не менее 24 символов).")
    for f in (a.cert,a.key):
        if not Path(f).is_file(): p.error(f"Файл не найден: {f}")
    store=ChangeStore(a.db,a.workspace)
    server=SyncHTTPServer((a.host,a.port),store,token,a.cert,a.key)
    print(f"PS GuardPRO LAN Sync: HTTPS {a.host}:{a.port}, workspace={a.workspace}")
    print("Остановить сервер: Ctrl+C")
    try: server.serve_forever(poll_interval=.5)
    except KeyboardInterrupt: print("\nОстановка службы…")
    finally: server.shutdown(); server.server_close()
if __name__=="__main__": main()
