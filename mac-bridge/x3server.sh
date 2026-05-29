#!/bin/sh
set -e

SCRIPT_DIR="/Users/taku/xteink/xteink-terminal/mac-bridge"
LOG="/tmp/x3server.log"
PID_FILE="/tmp/x3server.pid"

case "${1}" in
  start)
    if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
      echo "すでに起動しています (PID: $(cat "$PID_FILE"))"
      exit 0
    fi
    python3 "$SCRIPT_DIR/x3_server.py" > "$LOG" 2>&1 &
    echo $! > "$PID_FILE"
    echo "起動しました (PID: $!)"
    echo "ログ: tail -f $LOG"
    ;;
  stop)
    if [ -f "$PID_FILE" ]; then
      kill "$(cat "$PID_FILE")" 2>/dev/null && echo "停止しました" || echo "すでに停止しています"
      rm -f "$PID_FILE"
    else
      echo "PIDファイルがありません"
    fi
    ;;
  restart)
    $0 stop
    sleep 1
    $0 start
    ;;
  log)
    tail -f "$LOG"
    ;;
  *)
    echo "Usage: $0 {start|stop|restart|log}"
    exit 1
    ;;
esac
