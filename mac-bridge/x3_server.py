#!/usr/bin/env python3
"""
X3 Terminal Server

Runs the tmux-to-X3 bridge and exposes a web dashboard + control API.

Usage:
    python3 x3_server.py [--config path/to/config.json]
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import textwrap
import threading
import time
import unicodedata
import urllib.error
import urllib.request
from pathlib import Path

from flask import Flask, jsonify, redirect, render_template_string, request, url_for

# ============================================================================
# DEFAULTS
# ============================================================================

DEFAULT_CONFIG = {
    "x3_ip": "http://192.168.128.209",
    "tmux_session": "x3-terminal",
    "cols": 65,
    "rows": 32,
    "poll_interval": 0.25,
    "post_timeout": 1.0,
}

CONFIG_PATH = Path("~/.config/x3terminal/config.json").expanduser()

# ============================================================================
# GLYPH MAP (same as original bridge)
# ============================================================================

GLYPH_MAP = str.maketrans({
    "│": "|", "┃": "|", "║": "|",
    "─": "-", "━": "-", "═": "-",
    "┌": "+", "┐": "+", "└": "+", "┘": "+",
    "├": "+", "┤": "+", "┬": "+", "┴": "+", "┼": "+",
    "╭": "+", "╮": "+", "╰": "+", "╯": "+",
    "█": "#", "▓": "#", "▒": "#", "░": ".",
    "●": "*", "•": "*", "…": "...",
    "“": '"', "”": '"', "‘": "'", "’": "'",
})

# ============================================================================
# CONFIG
# ============================================================================

def load_config(path: Path) -> dict:
    if path.exists():
        try:
            return {**DEFAULT_CONFIG, **json.loads(path.read_text())}
        except Exception:
            pass
    return dict(DEFAULT_CONFIG)

def save_config(cfg: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cfg, indent=2))

# ============================================================================
# BRIDGE
# ============================================================================

class Bridge:
    def __init__(self, cfg: dict) -> None:
        self.cfg = cfg
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._last_payload: dict | None = None
        self.connected = False
        self.error: str | None = None

    @property
    def running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def start(self) -> None:
        if self.running:
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._clear_display()

    def _clear_display(self) -> None:
        payload = {"cols": self.cfg["cols"], "rows": [], "cursor": [0, 0]}
        _post_frame(self.cfg["x3_ip"], payload, self.cfg["post_timeout"])

    def _loop(self) -> None:
        target = self.cfg["tmux_session"] + ":"
        cols = self.cfg["cols"]
        rows = self.cfg["rows"]
        interval = self.cfg["poll_interval"]

        while not self._stop.is_set():
            try:
                _ensure_pane_size(target, cols, rows)
                payload = {
                    "cols": cols,
                    "rows": _capture(target, rows, cols),
                    "cursor": _cursor(target),
                }
            except (subprocess.CalledProcessError, FileNotFoundError) as e:
                self.error = str(e)
                time.sleep(1.0)
                continue

            if payload != self._last_payload:
                ok = _post_frame(self.cfg["x3_ip"], payload, self.cfg["post_timeout"])
                if ok:
                    self._last_payload = payload
                self.connected = ok
                self.error = None if ok else "X3 unreachable"

            self._stop.wait(interval)

# ============================================================================
# TMUX HELPERS
# ============================================================================

def _run_tmux(args: list[str]) -> str:
    r = subprocess.run(["tmux", *args], check=True, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.stdout

def _ensure_pane_size(target: str, cols: int, rows: int) -> None:
    subprocess.run(["tmux", "resize-pane", "-t", target,
                    "-x", str(cols), "-y", str(rows)], check=False)

def _clean_line(line: str, cols: int) -> str:
    line = line.translate(GLYPH_MAP)
    line = unicodedata.normalize("NFKD", line)
    line = line.encode("ascii", "replace").decode("ascii")
    line = "".join(ch if ch == "\t" or 32 <= ord(ch) <= 126 else " " for ch in line)
    line = line.replace("\t", "    ")
    return line.rstrip()

def _wrap(lines: list[str], cols: int, rows: int) -> list[str]:
    wrapped: list[str] = []
    for line in lines:
        clean = _clean_line(line, cols)
        if not clean:
            wrapped.append("")
            continue
        chunks = textwrap.wrap(clean, width=cols, break_long_words=True,
                               break_on_hyphens=False, replace_whitespace=False,
                               drop_whitespace=False)
        wrapped.extend(c[:cols] for c in chunks)
    if len(wrapped) < rows:
        wrapped = [""] * (rows - len(wrapped)) + wrapped
    return wrapped[-rows:]

def _capture(target: str, rows: int, cols: int) -> list[str]:
    out = _run_tmux(["capture-pane", "-p", "-t", target,
                     "-S", f"-{rows * 2}", "-E", "-"])
    return _wrap(out.splitlines(), cols, rows)

def _cursor(target: str) -> list[int]:
    out = _run_tmux(["display-message", "-p", "-t", target, "#{cursor_x} #{cursor_y}"])
    try:
        x, y = out.strip().split()
        return [int(x), int(y)]
    except ValueError:
        return [0, 0]

def _post_frame(base_url: str, payload: dict, timeout: float) -> bool:
    data = json.dumps(payload, separators=(",", ":")).encode()
    req = urllib.request.Request(
        base_url.rstrip("/") + "/frame", data=data,
        headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return 200 <= r.status < 300
    except Exception:
        return False

def _ensure_session(session: str) -> None:
    result = subprocess.run(["tmux", "has-session", "-t", session],
                            capture_output=True)
    if result.returncode != 0:
        subprocess.run(["tmux", "new-session", "-d", "-s", session], check=True)

# ============================================================================
# FLASK APP
# ============================================================================

DASHBOARD_HTML = """
<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>X3 Terminal</title>
<style>
  body { font-family: -apple-system, sans-serif; max-width: 600px; margin: 40px auto; padding: 0 16px; background: #f5f5f5; }
  h1 { font-size: 1.4em; }
  h2 { font-size: 1.1em; border-bottom: 1px solid #ccc; padding-bottom: 4px; margin-top: 28px; }
  .card { background: #fff; border-radius: 8px; padding: 16px; margin: 12px 0; box-shadow: 0 1px 3px rgba(0,0,0,.1); }
  .status { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 6px; }
  .on  { background: #34c759; }
  .off { background: #ff3b30; }
  label { display: block; margin: 8px 0 2px; font-size: .9em; color: #555; }
  input[type=text], select { width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 6px; font-size: 1em; box-sizing: border-box; }
  .btn { display: inline-block; padding: 8px 16px; border: none; border-radius: 6px; font-size: .95em; cursor: pointer; margin: 4px 4px 4px 0; }
  .btn-primary { background: #007aff; color: #fff; }
  .btn-danger  { background: #ff3b30; color: #fff; }
  .btn-gray    { background: #e5e5ea; color: #000; }
  .row { display: flex; gap: 8px; }
  .row input { flex: 1; }
  .msg { padding: 8px 12px; border-radius: 6px; margin: 8px 0; font-size: .9em; }
  .msg-ok  { background: #d4edda; color: #155724; }
  .msg-err { background: #f8d7da; color: #721c24; }
</style>
</head>
<body>
<h1>X3 Terminal</h1>

{% if msg %}
<div class="msg {{ 'msg-ok' if msg_ok else 'msg-err' }}">{{ msg }}</div>
{% endif %}

<div class="card">
  <h2>ブリッジ状態</h2>
  <p>
    <span class="status {{ 'on' if bridge.running else 'off' }}"></span>
    {{ 'Running' if bridge.running else 'Stopped' }}
    {% if bridge.error %} — <small style="color:#ff3b30">{{ bridge.error }}</small>{% endif %}
  </p>
  <p style="font-size:.85em;color:#666">
    X3: {{ cfg.x3_ip }} &nbsp;|&nbsp; セッション: {{ cfg.tmux_session }} &nbsp;|&nbsp; {{ cfg.cols }}×{{ cfg.rows }}
  </p>
  <form method="post" action="/mode/start" style="display:inline">
    <button class="btn btn-primary">開始</button>
  </form>
  <form method="post" action="/mode/stop" style="display:inline">
    <button class="btn btn-danger">停止</button>
  </form>
</div>

<div class="card">
  <h2>tmux セッション</h2>
  <p style="font-size:.85em;color:#666">tmコマンド相当のセッション一覧。X3からも操作できます。</p>
  <div id="sessions-list">読み込み中...</div>
</div>

<div class="card">
  <h2>サーバー設定</h2>
  <form method="post" action="/settings">
    <label>X3 IP アドレス</label>
    <input type="text" name="x3_ip" value="{{ cfg.x3_ip }}">
    <label>tmux セッション名</label>
    <input type="text" name="tmux_session" value="{{ cfg.tmux_session }}">
    <label>列数</label>
    <input type="text" name="cols" value="{{ cfg.cols }}">
    <label>行数</label>
    <input type="text" name="rows" value="{{ cfg.rows }}">
    <br>
    <button class="btn btn-primary" style="margin-top:12px">保存</button>
  </form>
</div>

<div class="card">
  <h2>X3 デバイス設定</h2>
  <p style="font-size:.85em;color:#666">X3 の HTTP API に直接アクセスして設定を変更します。</p>
  <a href="http://{{ cfg.x3_ip }}/" target="_blank">
    <button class="btn btn-gray" type="button">X3 設定を開く →</button>
  </a>
</div>

<script>
async function loadSessions() {
  const el = document.getElementById('sessions-list');
  try {
    const r = await fetch('/tm/sessions');
    const data = await r.json();
    if (!data.sessions || data.sessions.length === 0) {
      el.innerHTML = '<p style="color:#999">セッションなし</p>';
      return;
    }
    const activeSess = '{{ cfg.tmux_session }}';
    let html = '';
    for (const s of data.sessions) {
      const active = s.name === activeSess ? ' (X3)' : '';
      html += `<div style="display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid #eee">
        <span><strong>${s.name}</strong>${active} <span style="font-size:.8em;color:#999">${s.windows}W</span></span>
        ${s.name !== activeSess ? `<button class="btn btn-gray" style="padding:4px 10px;font-size:.85em" onclick="switchSession('${s.name}')">切替</button>` : ''}
      </div>`;
    }
    el.innerHTML = html;
  } catch(e) {
    el.innerHTML = '<p style="color:#f33">エラー</p>';
  }
}
async function switchSession(name) {
  await fetch('/tm/switch', {method: 'POST', body: new URLSearchParams({name})});
  loadSessions();
}
loadSessions();
setInterval(loadSessions, 5000);
</script>
</body>
</html>
"""

def get_tm_sessions() -> list[dict]:
    """tmux list-sessionsの結果をパース"""
    try:
        r = subprocess.run(["tmux", "list-sessions", "-F", "#S|#{session_windows}|#{?session_attached,active,inactive}"],
                           capture_output=True, text=True, check=True)
        sessions = []
        for line in r.stdout.strip().split("\n"):
            if not line:
                continue
            parts = line.split("|")
            if len(parts) >= 3:
                sessions.append({
                    "name": parts[0],
                    "windows": parts[1],
                    "state": parts[2],
                })
        return sessions
    except Exception:
        return []

def create_app(bridge: Bridge, cfg_path: Path) -> Flask:
    app = Flask(__name__)
    cfg = bridge.cfg

    def reload_cfg():
        nonlocal cfg
        cfg = load_config(cfg_path)
        bridge.cfg = cfg
        return cfg

    @app.route("/")
    def index():
        msg = request.args.get("msg", "")
        msg_ok = request.args.get("ok", "1") == "1"
        return render_template_string(DASHBOARD_HTML,
                                      bridge=bridge, cfg=cfg,
                                      msg=msg, msg_ok=msg_ok)

    @app.route("/status")
    def status():
        return jsonify({
            "bridge": bridge.running,
            "connected": bridge.connected,
            "error": bridge.error,
            "config": cfg,
        })

    @app.route("/mode/start", methods=["POST"])
    def mode_start():
        _ensure_session(cfg["tmux_session"])
        bridge.start()
        return redirect("/?msg=ブリッジを開始しました&ok=1")

    @app.route("/mode/stop", methods=["POST"])
    def mode_stop():
        bridge.stop()
        return redirect("/?msg=ブリッジを停止しました&ok=1")

    @app.route("/keypress", methods=["POST"])
    def keypress():
        data = request.get_json(silent=True) or {}
        key = data.get("key", "")
        if not key:
            return jsonify({"error": "key required"}), 400
        target = cfg["tmux_session"] + ":"
        subprocess.run(["tmux", "send-keys", "-t", target, key], check=False)
        return "", 204

    @app.route("/session/ssh", methods=["POST"])
    def session_ssh():
        host = (request.form.get("host") or
                (request.get_json(silent=True) or {}).get("host", ""))
        if not host:
            return redirect("/?msg=ホストを入力してください&ok=0")
        target = cfg["tmux_session"] + ":"
        _ensure_session(cfg["tmux_session"])
        subprocess.run(["tmux", "send-keys", "-t", target,
                        f"ssh {host}", "Enter"], check=False)
        if not bridge.running:
            bridge.start()
        return redirect("/?msg=SSH 接続を開始しました: " + host + "&ok=1")

    @app.route("/tm/sessions")
    def tm_sessions():
        return jsonify({"sessions": get_tm_sessions()})

    @app.route("/tm/switch", methods=["POST"])
    def tm_switch():
        name = request.form.get("name", "")
        if not name:
            return jsonify({"error": "name required"}), 400
        cfg["tmux_session"] = name
        save_config(cfg, cfg_path)
        bridge.cfg = cfg
        # Restart bridge to target new session
        bridge.stop()
        time.sleep(0.2)
        bridge.start()
        return jsonify({"ok": True})

    @app.route("/settings", methods=["GET", "POST"])
    def settings():
        if request.method == "POST":
            form = request.form
            cfg["x3_ip"]        = form.get("x3_ip", cfg["x3_ip"]).strip()
            cfg["tmux_session"] = form.get("tmux_session", cfg["tmux_session"]).strip()
            try: cfg["cols"] = int(form.get("cols", cfg["cols"]))
            except ValueError: pass
            try: cfg["rows"] = int(form.get("rows", cfg["rows"]))
            except ValueError: pass
            save_config(cfg, cfg_path)
            return redirect("/?msg=設定を保存しました&ok=1")
        return jsonify(cfg)

    return app

# ============================================================================
# MAIN
# ============================================================================

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=CONFIG_PATH)
    parser.add_argument("--port", type=int, default=3333)
    parser.add_argument("--host", default="0.0.0.0")
    return parser.parse_args()

def main() -> int:
    args = parse_args()
    cfg = load_config(args.config)

    bridge = Bridge(cfg)
    _ensure_session(cfg["tmux_session"])
    bridge.start()

    app = create_app(bridge, args.config)
    print(f"X3 Terminal Server running at http://0.0.0.0:{args.port}/", flush=True)
    app.run(host=args.host, port=args.port, debug=False)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
