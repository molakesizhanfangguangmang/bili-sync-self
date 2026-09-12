#!/bin/sh
# 容器内：停掉 steward（配合 run.sh 的 stop 标记，避免它把进程重起）
DIR=$(cd "$(dirname "$0")" && pwd)
touch "$DIR/stop"
[ -f "$DIR/steward.pid" ] && kill "$(cat "$DIR/steward.pid")" 2>/dev/null
sleep 1
rm -f "$DIR/steward.pid" "$DIR/stop"
echo "[stop.sh] steward 已停"
