"""
GitInspect — Python backend
Endpoints:
  POST /files   { url, token? }              → { files: [...] }
  POST /debug   { url, session_id, token?, files?: [...] } → { queued: N }
  GET  /stream?session_id=<id>               → SSE stream
  GET  /health                               → "healthy"
"""

import json
import os
import queue
import shutil
import subprocess
import tempfile
import threading
import time
from collections import defaultdict
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor

import boto3
from flask import Flask, Response, jsonify, request, stream_with_context

# ── Config ────────────────────────────────────────────────────────────────────

MODEL_ID      = "us.anthropic.claude-3-5-haiku-20241022-v1:0"
POOL_WORKERS  = 10
RATE_LIMITS   = {
    "/debug":  (5,  60),
    "/stream": (30, 60),
    "/health": (60, 60),
}
SKIP_EXTS = {
    ".pack", ".idx", ".rev", ".bin", ".exe",
    ".so", ".dylib", ".o", ".a", ".png", ".jpg",
    ".jpeg", ".gif", ".ico", ".svg", ".woff", ".woff2",
    ".ttf", ".eot", ".mp4", ".mp3", ".zip", ".tar", ".gz",
}

app      = Flask(__name__)
executor = ThreadPoolExecutor(max_workers=POOL_WORKERS)

# ── Prompt ────────────────────────────────────────────────────────────────────

def load_prompt() -> str:
    candidates = [
        Path(__file__).parent.parent / "AWS_Bedrock_Prompt" / "Prompt.txt",
        Path(__file__).parent / "AWS_Bedrock_Prompt" / "Prompt.txt",
    ]
    for p in candidates:
        if p.exists():
            return p.read_text(encoding="utf-8")
    print("[WARN] Prompt.txt not found — using empty prompt")
    return ""

SYSTEM_PROMPT = load_prompt()

# ── SSE sink registry ─────────────────────────────────────────────────────────

_sse_queues: dict[str, queue.Queue] = {}
_sse_lock   = threading.Lock()

def _get_queue(session_id: str) -> queue.Queue:
    with _sse_lock:
        if session_id not in _sse_queues:
            _sse_queues[session_id] = queue.Queue()
        return _sse_queues[session_id]

def _send_sse(session_id: str, data: str):
    with _sse_lock:
        q = _sse_queues.get(session_id)
    if q:
        q.put(data)

def _close_sse(session_id: str):
    with _sse_lock:
        q = _sse_queues.pop(session_id, None)
    if q:
        q.put(None)  # sentinel

# ── Rate limiter ──────────────────────────────────────────────────────────────

_rate_lock   = threading.Lock()
_rate_table: dict[str, list[float]] = defaultdict(list)

def _rate_check(ip: str, path: str) -> tuple[bool, int]:
    if path not in RATE_LIMITS:
        return True, 0
    max_req, window = RATE_LIMITS[path]
    now = time.monotonic()
    key = f"{ip}|{path}"
    with _rate_lock:
        timestamps = _rate_table[key]
        _rate_table[key] = [t for t in timestamps if now - t < window]
        if len(_rate_table[key]) >= max_req:
            retry_after = int(window - (now - _rate_table[key][0])) + 1
            return False, retry_after
        _rate_table[key].append(now)
    return True, 0

# ── CORS ──────────────────────────────────────────────────────────────────────

_allowed_origins: list[str] = []
_wildcard = True

def _init_cors():
    global _allowed_origins, _wildcard
    raw = os.environ.get("ALLOWED_ORIGINS", "")
    origins = [o.strip() for o in raw.split(",") if o.strip()]
    if origins:
        _allowed_origins = origins
        _wildcard = False
        print(f"[CORS] Allowed origins: {origins}")
    else:
        _wildcard = True
        print("[CORS] Wildcard mode (ALLOWED_ORIGINS not set)")

_init_cors()

def _cors_origin(request_origin: str) -> str | None:
    """Returns the value for Access-Control-Allow-Origin, or None if blocked."""
    if not request_origin:
        return None  # no header needed, not blocked
    if _wildcard:
        return "*"
    if request_origin in _allowed_origins:
        return request_origin
    return None  # blocked

@app.after_request
def _add_cors(response: Response) -> Response:
    origin = request.headers.get("Origin", "")
    value = _cors_origin(origin)
    if value:
        response.headers["Access-Control-Allow-Origin"] = value
        response.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
        response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    return response

@app.before_request
def _preflight():
    if request.method == "OPTIONS":
        origin = request.headers.get("Origin", "")
        value = _cors_origin(origin)
        if not value and origin:
            return Response("Forbidden", status=403)
        resp = Response(status=204)
        if value:
            resp.headers["Access-Control-Allow-Origin"] = value
            resp.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
            resp.headers["Access-Control-Allow-Headers"] = "Content-Type"
        return resp

    # Rate limit (skip OPTIONS)
    allowed, retry_after = _rate_check(request.remote_addr, request.path)
    if not allowed:
        resp = Response("Rate limit exceeded", status=429)
        resp.headers["Retry-After"] = str(retry_after)
        return resp

# ── Helpers ───────────────────────────────────────────────────────────────────

def _clone(url: str, token: str, dest: str) -> bool:
    clone_url = url
    if token:
        clone_url = url.replace("https://", f"https://{token}@")
    result = subprocess.run(
        ["git", "clone", "--depth=1", clone_url, dest],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"[ERROR] Clone failed: {result.stderr}")
    return result.returncode == 0

def _collect_files(root: str) -> list[str]:
    files = []
    root_path = Path(root)
    for p in root_path.rglob("*"):
        if not p.is_file():
            continue
        parts = p.relative_to(root_path).parts
        if parts and parts[0] == ".git":
            continue
        if p.suffix.lower() in SKIP_EXTS:
            continue
        files.append(str(p.relative_to(root_path)))
    return files

def _call_bedrock(filename: str, content: str) -> str:
    client = boto3.client("bedrock-runtime")
    body = {
        "anthropic_version": "bedrock-2023-05-31",
        "max_tokens": 2048,
        "system": SYSTEM_PROMPT,
        "messages": [{"role": "user", "content": f"File: {filename}\n\n{content}"}],
    }
    try:
        response = client.invoke_model(
            modelId=MODEL_ID,
            contentType="application/json",
            body=json.dumps(body),
        )
        result = json.loads(response["body"].read())
        return result["content"][0]["text"]
    except Exception as e:
        print(f"[ERROR] Bedrock call failed for {filename}: {e}")
        return json.dumps({"filePath": filename, "status": "error", "issues": []})

def _analyse_file(session_id: str, clone_dir: str, rel_path: str, pending: list, lock: threading.Lock):
    full_path = Path(clone_dir) / rel_path
    try:
        content = full_path.read_text(encoding="utf-8", errors="replace")
    except Exception as e:
        print(f"[WARN] Could not read {rel_path}: {e}")
        content = ""

    print(f"[DEBUG] Analysing {rel_path}")
    raw = _call_bedrock(rel_path, content)

    # Attach filePath so the frontend can display it
    try:
        parsed = json.loads(raw)
        parsed.setdefault("filePath", rel_path)
        parsed.setdefault("status", "complete")
        _send_sse(session_id, json.dumps(parsed))
    except Exception:
        _send_sse(session_id, json.dumps({"filePath": rel_path, "status": "error", "issues": []}))

    with lock:
        pending[0] -= 1
        if pending[0] == 0:
            _send_sse(session_id, json.dumps({"event": "done"}))
            _close_sse(session_id)
            shutil.rmtree(clone_dir, ignore_errors=True)
            print(f"[DEBUG] Cleaned up {clone_dir}")

# ── Routes ────────────────────────────────────────────────────────────────────

@app.post("/files")
def route_files():
    body = request.get_json(silent=True) or {}
    url   = body.get("url", "").strip()
    token = body.get("token", "").strip()

    if not url:
        return Response("Missing url", status=400)

    clone_dir = tempfile.mkdtemp(prefix="gitinspect_")
    if not _clone(url, token, clone_dir):
        shutil.rmtree(clone_dir, ignore_errors=True)
        return Response("Clone failed", status=500)

    files = _collect_files(clone_dir)
    shutil.rmtree(clone_dir, ignore_errors=True)
    return jsonify({"files": files})


@app.post("/debug")
def route_debug():
    body       = request.get_json(silent=True) or {}
    url        = body.get("url", "").strip()
    session_id = body.get("session_id", "").strip()
    token      = body.get("token", "").strip()
    selected   = body.get("files")  # optional list from file selector

    if not url or not session_id:
        return Response("Missing url or session_id", status=400)

    clone_dir = tempfile.mkdtemp(prefix="gitinspect_")
    if not _clone(url, token, clone_dir):
        shutil.rmtree(clone_dir, ignore_errors=True)
        return Response("Clone failed", status=500)

    all_files = _collect_files(clone_dir)
    files = [f for f in all_files if f in selected] if selected else all_files

    if not files:
        shutil.rmtree(clone_dir, ignore_errors=True)
        return jsonify({"queued": 0})

    pending = [len(files)]
    lock    = threading.Lock()
    _get_queue(session_id)  # pre-register queue before tasks start

    for rel_path in files:
        executor.submit(_analyse_file, session_id, clone_dir, rel_path, pending, lock)

    return jsonify({"queued": len(files)})


@app.get("/stream")
def route_stream():
    session_id = request.args.get("session_id", "")
    if not session_id:
        return Response("Missing session_id", status=400)

    q = _get_queue(session_id)

    def generate():
        while True:
            item = q.get()
            if item is None:
                break
            yield f"data: {item}\n\n"

    return Response(
        stream_with_context(generate()),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


@app.get("/health")
def route_health():
    return Response("healthy", mimetype="text/plain")


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080, threaded=True)
