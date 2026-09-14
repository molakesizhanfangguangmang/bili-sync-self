#!/bin/sh
# 容器内常驻包装：起 steward，进程没了就重起；日志写在同目录 steward.log
# 由 install.sh 塞进容器里，用 docker exec -d 启动。
# 设置写在同目录的 steward.env 里（KEY=VALUE），每次起来先读它 —— 这样容器重建后
# 手敲一条 docker exec 也能把原来那套参数带上。
DIR=$(cd "$(dirname "$0")" && pwd)
LOG=$DIR/steward.log

[ -f "$DIR/steward.env" ] && . "$DIR/steward.env"

PORT=${STEWARD_PORT:-12346}
DB=${STEWARD_DB:-/app/.config/bili-sync/data.sqlite}
ROOT=${STEWARD_ROOT:-/downloads}
TOKEN=${STEWARD_TOKEN:-}
FLOOR=${STEWARD_FLOOR:-10}
PUSH_URL=${STEWARD_PUSH_URL:-}
PUSH_EVERY=${STEWARD_PUSH_EVERY:-900}
CHECK_EVERY=${STEWARD_CHECK_EVERY:-60}
WATCH=${STEWARD_WATCH:-1}

rm -f "$DIR/stop"
echo "[run.sh] start $(date '+%F %T') port=$PORT db=$DB root=$ROOT floor=$FLOOR push=$PUSH_URL token=$([ -n "$TOKEN" ] && echo on || echo off) watch=$WATCH" >>"$LOG"

while [ ! -f "$DIR/stop" ]; do
    set -- --port "$PORT" --db "$DB" --root "$ROOT" --floor "$FLOOR" \
           --push-every "$PUSH_EVERY" --check-every "$CHECK_EVERY"
    [ "$WATCH" = "0" ] && set -- "$@" --no-watch
    [ -n "$PUSH_URL" ] && set -- "$@" --push-url "$PUSH_URL"
    [ -n "$TOKEN" ] && set -- "$@" --token "$TOKEN"

    "$DIR/steward" "$@" >>"$LOG" 2>&1 &
    echo $! >"$DIR/steward.pid"
    wait $!
    rc=$?

    [ -f "$DIR/stop" ] && break
    echo "[run.sh] steward 退出 rc=$rc，5 秒后重起" >>"$LOG"
    sleep 5
done

echo "[run.sh] stop $(date '+%F %T')" >>"$LOG"
