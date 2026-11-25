"""Opt-in LAN sync transport and revisioned change journal.

The service stores an append-only change log on the host PC. It never exposes
SQLite files over SMB. TLS is mandatory and clients authenticate with a shared
workspace token. Application CRUD integration remains a separate migration step.
"""
from __future__ import annotations
import hashlib, hmac, json, os, socket, sqlite3, ssl, tempfile, threading, urllib.error, urllib.request
from dataclasses import asdict, dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Literal

Role = Literal["standalone", "server", "client"]
MAX_BODY = 2 * 1024 * 1024
MAX_FILE_SIZE = 25 * 1024 * 1024

@dataclass(frozen=True)
class NetworkProfile:
    role: Role = "standalone"
    server_host: str = ""
    server_port: int = 8765
    use_tls: bool = True
    workspace_id: str = "default"
    def validate(self):
        if self.role not in ("standalone", "server", "client"): raise ValueError("Неизвестный режим сети.")
        if not isinstance(self.server_port, int) or not 1 <= self.server_port <= 65535: raise ValueError("Порт должен быть в диапазоне 1–65535.")
        if self.role == "client" and not self.server_host.strip(): raise ValueError("Для рабочей станции укажите адрес главного компьютера.")
        if self.role != "standalone" and not self.use_tls: raise ValueError("Для сетевого режима требуется защищённое TLS-соединение.")
        if not self.workspace_id.strip() or len(self.workspace_id) > 64: raise ValueError("Идентификатор рабочей группы должен содержать 1–64 символа.")
        if len(self.server_host) > 253 or any(c.isspace() for c in self.server_host): raise ValueError("Некорректный адрес главного компьютера.")
        return self

def load_profile(path):
    target=Path(path)
    if not target.exists(): return NetworkProfile()
    raw=json.loads(target.read_text(encoding="utf-8"))
    if not isinstance(raw,dict): raise ValueError("Файл сетевого профиля повреждён.")
    allowed={"role","server_host","server_port","use_tls","workspace_id"}
    return NetworkProfile(**{k:v for k,v in raw.items() if k in allowed}).validate()

def save_profile(profile,path):
    profile.validate(); target=Path(path); target.parent.mkdir(parents=True,exist_ok=True)
    fd,tmp=tempfile.mkstemp(prefix=target.name+".",suffix=".tmp",dir=str(target.parent))
    try:
        with os.fdopen(fd,"w",encoding="utf-8",newline="\n") as f:
            json.dump(asdict(profile),f,ensure_ascii=False,indent=2); f.write("\n"); f.flush(); os.fsync(f.fileno())
        Path(tmp).replace(target)
    except Exception:
        Path(tmp).unlink(missing_ok=True); raise
    return target

def probe_server(host,port,timeout=2.0):
    if not host.strip() or not 1<=int(port)<=65535: return False,"Не задан корректный адрес или порт."
    try:
        with socket.create_connection((host.strip(),int(port)),timeout=max(.2,min(float(timeout),5.0))):
            return True,"TCP-порт доступен. Это не подтверждает TLS, авторизацию или готовность синхронизации."
    except (OSError,ValueError) as exc: return False,f"Сервер недоступен: {exc}"

class ChangeStore:
    """Append-only, idempotent change log with optimistic revision checks."""
    def __init__(self, db_path, workspace_id="default"):
        self.path=Path(db_path); self.path.parent.mkdir(parents=True,exist_ok=True); self.workspace_id=workspace_id
        with self.connect() as c:
            c.execute("CREATE TABLE IF NOT EXISTS sync_changes (seq INTEGER PRIMARY KEY AUTOINCREMENT, workspace TEXT NOT NULL, change_id TEXT NOT NULL, entity_type TEXT NOT NULL, entity_id TEXT NOT NULL, revision INTEGER NOT NULL, base_revision INTEGER NOT NULL, operation TEXT NOT NULL, payload TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, UNIQUE(workspace,change_id))")
            c.execute("CREATE INDEX IF NOT EXISTS ix_sync_entity ON sync_changes(workspace,entity_type,entity_id,revision)")
    def connect(self):
        c=sqlite3.connect(self.path,timeout=15); c.execute("PRAGMA journal_mode=WAL"); c.execute("PRAGMA busy_timeout=15000"); return c
    def latest_revision(self, entity_type, entity_id):
        with self.connect() as c:
            r=c.execute("SELECT MAX(revision) FROM sync_changes WHERE workspace=? AND entity_type=? AND entity_id=?",(self.workspace_id,entity_type,entity_id)).fetchone()[0]
        return int(r or 0)
    def append(self, item):
        required={"change_id","entity_type","entity_id","base_revision","operation","payload"}
        if not isinstance(item,dict) or not required.issubset(item): raise ValueError("Неполное изменение.")
        vals=[str(item[k]) for k in ("change_id","entity_type","entity_id","operation")]
        change_id,etype,eid,op=vals
        if not all(vals) or len(change_id)>128 or len(etype)>64 or len(eid)>256 or op not in ("upsert","delete"): raise ValueError("Некорректные поля изменения.")
        base=int(item["base_revision"])
        if base<0: raise ValueError("Некорректная базовая версия.")
        payload=item["payload"]
        if not isinstance(payload,dict): raise ValueError("Полезная нагрузка должна быть JSON-объектом.")
        encoded=json.dumps(payload,ensure_ascii=False,separators=(",",":"),sort_keys=True)
        with self.connect() as c:
            old=c.execute("SELECT seq,revision FROM sync_changes WHERE workspace=? AND change_id=?",(self.workspace_id,change_id)).fetchone()
            if old: return {"seq":old[0],"revision":old[1],"duplicate":True}
            current=c.execute("SELECT MAX(revision) FROM sync_changes WHERE workspace=? AND entity_type=? AND entity_id=?",(self.workspace_id,etype,eid)).fetchone()[0] or 0
            if base!=current: raise RevisionConflict(int(current))
            rev=int(current)+1
            cur=c.execute("INSERT INTO sync_changes(workspace,change_id,entity_type,entity_id,revision,base_revision,operation,payload) VALUES(?,?,?,?,?,?,?,?)",(self.workspace_id,change_id,etype,eid,rev,base,op,encoded))
            return {"seq":cur.lastrowid,"revision":rev,"duplicate":False}
    def since(self, cursor=0, limit=500):
        cursor=max(0,int(cursor)); limit=max(1,min(int(limit),1000))
        with self.connect() as c:
            rows=c.execute("SELECT seq,change_id,entity_type,entity_id,revision,base_revision,operation,payload,created_at FROM sync_changes WHERE workspace=? AND seq>? ORDER BY seq LIMIT ?",(self.workspace_id,cursor,limit)).fetchall()
        return [{"seq":r[0],"change_id":r[1],"entity_type":r[2],"entity_id":r[3],"revision":r[4],"base_revision":r[5],"operation":r[6],"payload":json.loads(r[7]),"created_at":r[8]} for r in rows]

    @property
    def files_root(self):
        root = self.path.parent / "sync_files"
        root.mkdir(parents=True, exist_ok=True)
        return root

    def put_file(self, digest: str, data: bytes) -> dict:
        if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest.lower()):
            raise ValueError("Некорректный SHA-256 идентификатор файла.")
        if not data or len(data) > MAX_FILE_SIZE:
            raise ValueError("Размер файла должен быть от 1 байта до 25 МиБ.")
        actual = hashlib.sha256(data).hexdigest()
        if not hmac.compare_digest(actual, digest.lower()):
            raise ValueError("Контрольная сумма файла не совпадает.")
        target = self.files_root / digest[:2] / digest.lower()
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            if hashlib.sha256(target.read_bytes()).hexdigest() != digest.lower():
                raise ValueError("На сервере обнаружен повреждённый файл с таким SHA-256.")
            return {"sha256": digest.lower(), "size": len(data), "duplicate": True}
        fd, tmp = tempfile.mkstemp(prefix=".upload-", dir=str(target.parent))
        try:
            with os.fdopen(fd, "wb") as stream:
                stream.write(data); stream.flush(); os.fsync(stream.fileno())
            Path(tmp).replace(target)
        except Exception:
            Path(tmp).unlink(missing_ok=True)
            raise
        return {"sha256": digest.lower(), "size": len(data), "duplicate": False}

    def get_file(self, digest: str) -> bytes:
        if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest.lower()):
            raise ValueError("Некорректный SHA-256 идентификатор файла.")
        target = self.files_root / digest[:2] / digest.lower()
        data = target.read_bytes()
        if len(data) > MAX_FILE_SIZE or hashlib.sha256(data).hexdigest() != digest.lower():
            raise ValueError("Файл на сервере повреждён или превышает допустимый размер.")
        return data

class RevisionConflict(Exception):
    def __init__(self,current_revision): self.current_revision=current_revision; super().__init__(f"Конфликт версии: актуальная версия {current_revision}.")

class SyncHTTPServer(ThreadingHTTPServer):
    daemon_threads=True; allow_reuse_address=True
    def __init__(self,address,store,token,certfile,keyfile):
        if not token or len(token)<24: raise ValueError("Токен должен содержать не менее 24 символов.")
        self.store=store; self.token=token
        super().__init__(address,self.handler_class())
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.minimum_version=ssl.TLSVersion.TLSv1_2; ctx.load_cert_chain(certfile,keyfile)
        self.socket=ctx.wrap_socket(self.socket,server_side=True)
    def handler_class(self):
        store=self.store; token=self.token
        class Handler(BaseHTTPRequestHandler):
            server_version="PSPassMasterSync/1.0"; sys_version=""
            def log_message(self,*args): pass
            def send_json(self,status,obj):
                data=json.dumps(obj,ensure_ascii=False).encode(); self.send_response(status); self.send_header("Content-Type","application/json; charset=utf-8"); self.send_header("Content-Length",str(len(data))); self.send_header("Cache-Control","no-store"); self.end_headers(); self.wfile.write(data)
            def authorized(self):
                supplied=self.headers.get("Authorization","")
                return hmac.compare_digest(supplied,"Bearer "+token)
            def do_GET(self):
                if not self.authorized(): return self.send_json(401,{"error":"unauthorized"})
                if self.path=="/health": return self.send_json(200,{"status":"ok","workspace":store.workspace_id,"protocol":1})
                if self.path.startswith("/v1/files/"):
                    digest = self.path.split("?", 1)[0].rsplit("/", 1)[-1]
                    try:
                        data = store.get_file(digest)
                        self.send_response(200); self.send_header("Content-Type", "application/octet-stream")
                        self.send_header("Content-Length", str(len(data))); self.send_header("Cache-Control", "no-store")
                        self.end_headers(); self.wfile.write(data)
                    except FileNotFoundError: self.send_json(404, {"error":"file_not_found"})
                    except ValueError as exc: self.send_json(400, {"error":str(exc)[:200]})
                    return
                if self.path.startswith("/v1/changes"):
                    from urllib.parse import urlparse,parse_qs
                    q=parse_qs(urlparse(self.path).query)
                    try: return self.send_json(200,{"changes":store.since(q.get("since",["0"])[0],q.get("limit",["500"])[0])})
                    except (ValueError,TypeError): return self.send_json(400,{"error":"invalid_cursor"})
                self.send_json(404,{"error":"not_found"})
            def do_POST(self):
                if not self.authorized(): return self.send_json(401,{"error":"unauthorized"})
                if self.path.startswith("/v1/files/"):
                    digest = self.path.split("?", 1)[0].rsplit("/", 1)[-1]
                    try:
                        size = int(self.headers.get("Content-Length", "0"))
                        if size <= 0 or size > MAX_FILE_SIZE: return self.send_json(413, {"error":"file_too_large"})
                        data = self.rfile.read(size)
                        if len(data) != size: return self.send_json(400, {"error":"incomplete_upload"})
                        return self.send_json(200, store.put_file(digest, data))
                    except (ValueError, OSError) as exc: return self.send_json(400, {"error":str(exc)[:200]})
                if self.path!="/v1/changes": return self.send_json(404,{"error":"not_found"})
                try:
                    size=int(self.headers.get("Content-Length","0"))
                    if size<=0 or size>MAX_BODY: return self.send_json(413,{"error":"payload_too_large"})
                    item=json.loads(self.rfile.read(size).decode("utf-8")); result=store.append(item); self.send_json(200,result)
                except RevisionConflict as e: self.send_json(409,{"error":"revision_conflict","current_revision":e.current_revision})
                except (ValueError,TypeError,KeyError,json.JSONDecodeError) as e: self.send_json(400,{"error":str(e)[:300]})
        return Handler

def _request(base_url,token,path,method="GET",payload=None,timeout=10,ca_file=None):
    if not base_url.startswith("https://"): raise ValueError("Синхронизация разрешена только по HTTPS.")
    data=None if payload is None else json.dumps(payload,ensure_ascii=False).encode()
    req=urllib.request.Request(base_url.rstrip("/")+path,data=data,method=method,headers={"Authorization":"Bearer "+token,"Accept":"application/json","Content-Type":"application/json"})
    ctx=ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(req,timeout=timeout,context=ctx) as r: return json.loads(r.read(MAX_BODY+1).decode())
    except urllib.error.HTTPError as e:
        body=e.read(4096).decode("utf-8","replace")
        if e.code==409: raise RevisionConflict(json.loads(body).get("current_revision",0))
        raise RuntimeError(f"Сервер синхронизации вернул HTTP {e.code}: {body}") from e

def client_health(base_url,token,ca_file=None): return _request(base_url,token,"/health",ca_file=ca_file)
def publish_change(base_url,token,item,ca_file=None): return _request(base_url,token,"/v1/changes","POST",item,ca_file=ca_file)
def fetch_changes(base_url,token,since=0,limit=500,ca_file=None): return _request(base_url,token,f"/v1/changes?since={int(since)}&limit={int(limit)}",ca_file=ca_file)["changes"]


def upload_file(base_url, token, data: bytes, ca_file=None):
    """Upload a content-addressed attachment; returns server SHA-256 metadata."""
    if not isinstance(data, (bytes, bytearray)) or not data or len(data) > MAX_FILE_SIZE:
        raise ValueError("Файл должен иметь размер от 1 байта до 25 МиБ.")
    digest = hashlib.sha256(data).hexdigest()
    if not base_url.startswith("https://"): raise ValueError("Синхронизация разрешена только по HTTPS.")
    req = urllib.request.Request(base_url.rstrip("/")+"/v1/files/"+digest, data=bytes(data), method="POST",
        headers={"Authorization":"Bearer "+token,"Content-Type":"application/octet-stream","Accept":"application/json"})
    ctx=ssl.create_default_context(cafile=ca_file)
    with urllib.request.urlopen(req, timeout=30, context=ctx) as response:
        result=json.loads(response.read(4096).decode("utf-8"))
    if result.get("sha256") != digest or int(result.get("size", -1)) != len(data):
        raise ValueError("Сервер вернул некорректное подтверждение файла.")
    return result


def download_file(base_url, token, digest, ca_file=None):
    """Download and verify a content-addressed attachment."""
    if len(str(digest)) != 64 or any(c not in "0123456789abcdef" for c in str(digest).lower()):
        raise ValueError("Некорректный SHA-256 идентификатор файла.")
    if not base_url.startswith("https://"): raise ValueError("Синхронизация разрешена только по HTTPS.")
    req=urllib.request.Request(base_url.rstrip("/")+"/v1/files/"+str(digest).lower(),headers={"Authorization":"Bearer "+token,"Accept":"application/octet-stream"})
    ctx=ssl.create_default_context(cafile=ca_file)
    with urllib.request.urlopen(req, timeout=30, context=ctx) as response:
        data=response.read(MAX_FILE_SIZE+1)
    if len(data)>MAX_FILE_SIZE or hashlib.sha256(data).hexdigest()!=str(digest).lower():
        raise ValueError("Полученный файл не прошёл проверку SHA-256 или превышает лимит.")
    return data
