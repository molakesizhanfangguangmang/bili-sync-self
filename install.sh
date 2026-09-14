#!/bin/sh
# 从一个 release 取齐 bili-sync 主程序和 steward 管家，然后把 steward 注入同一个容器。
set -e

CONTAINER=${CONTAINER:-bili-sync-rs}
PORT=${PORT:-12346}
DB=${DB:-/app/.config/bili-sync/data.sqlite}
ROOT=${ROOT:-/downloads}
TOKEN=${TOKEN:-}
FLOOR=${FLOOR:-10}
PUSH_URL=${PUSH_URL:-}
PUSH_EVERY=${PUSH_EVERY:-900}
CHECK_EVERY=${CHECK_EVERY:-60}
WATCH=${WATCH:-1}
DEST=${DEST:-/app/.config/bili-sync/steward}
LEGACY=${LEGACY:-/app/.config/bili-sync/fileview}
OWNER=${OWNER:-1000:1001}
SRC=$(cd "$(dirname "$0")" && pwd)
TMP=${TMPDIR:-/tmp}/bili-sync-self.env.$$
REPO=${REPO:-molakesizhanfangguangmang/bili-sync-self}
VERSION=${VERSION:-1.0.2}
FETCH=${FETCH:-1}

case "$(uname -m)" in
    aarch64|arm64) ARCH=aarch64 ;;
    x86_64|amd64) ARCH=x86_64 ;;
    *) ARCH= ;;
esac
BASE="https://github.com/$REPO/releases/download/v$VERSION"

if [ "$FETCH" != "0" ]; then
    if [ -z "$ARCH" ]; then
        echo "本机架构 $(uname -m) 没有现成的二进制；自行编译或手动放入 dist/。" >&2
        exit 1
    fi
    if command -v curl >/dev/null 2>&1; then DL="curl -fsSL -o"
    elif command -v wget >/dev/null 2>&1; then DL="wget -qO"
    else echo "没有 curl 或 wget，不能下载 release。" >&2; exit 1
    fi
    mkdir -p "$SRC/dist"
    if [ ! -f "$SRC/dist/bili-sync-rs" ]; then
        echo "[0/6] 取 bili-sync-rs-$ARCH"
        $DL "$SRC/dist/bili-sync-rs" "$BASE/bili-sync-rs-$ARCH"
        chmod 755 "$SRC/dist/bili-sync-rs"
    fi
    if [ ! -f "$SRC/dist/steward" ]; then
        echo "[0/6] 取 steward-v$VERSION-$ARCH"
        $DL "$SRC/dist/steward" "$BASE/steward-v$VERSION-$ARCH"
        chmod 755 "$SRC/dist/steward"
    fi
    SUMFILE=${TMPDIR:-/tmp}/SHA256SUMS.$$
    $DL "$SUMFILE" "$BASE/SHA256SUMS"
    for pair in "bili-sync-rs-$ARCH:$SRC/dist/bili-sync-rs" "steward-v$VERSION-$ARCH:$SRC/dist/steward"; do
        name=${pair%%:*}
        file=${pair#*:}
        want=$(awk -v f="$name" '$2 == f {print $1}' "$SUMFILE")
        [ -n "$want" ] || { echo "校验清单缺少 $name" >&2; exit 1; }
        got=$(sha256sum "$file" | awk '{print $1}')
        [ "$want" = "$got" ] || { echo "SHA256 对不上：$file" >&2; exit 1; }
    done
    rm -f "$SUMFILE"
fi

[ -x "$SRC/dist/bili-sync-rs" ] || { echo "缺少 dist/bili-sync-rs。" >&2; exit 1; }
[ -x "$SRC/dist/steward" ] || { echo "缺少 dist/steward。" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || { echo "找不到 docker。" >&2; exit 1; }
docker inspect "$CONTAINER" >/dev/null 2>&1 || { echo "找不到容器 $CONTAINER。" >&2; exit 1; }

echo "[1/6] 更新 bili-sync 主程序"
# 先保留容器内正在用的主程序，防止更新失败后无文件可回退。
BACKUP="$SRC/bili-sync-rs.backup.$(date +%Y%m%d%H%M%S)"
docker cp "$CONTAINER:/app/bili-sync-rs" "$BACKUP"
echo "旧主程序已备份：$BACKUP"
docker cp "$SRC/dist/bili-sync-rs" "$CONTAINER:/app/bili-sync-rs"
echo "[2/6] 注入 steward"
docker exec -u 0 "$CONTAINER" mkdir -p "$DEST"
docker cp "$SRC/dist/steward" "$CONTAINER:$DEST/steward"
docker cp "$SRC/run.sh" "$CONTAINER:$DEST/run.sh"
docker cp "$SRC/stop.sh" "$CONTAINER:$DEST/stop.sh"
docker exec -u 0 "$CONTAINER" chown -R "$OWNER" "$DEST" 2>/dev/null || true
docker exec -u 0 "$CONTAINER" chmod 755 "$DEST" "$DEST/run.sh" "$DEST/stop.sh" "$DEST/steward"
{
    echo "STEWARD_PORT=$PORT"; echo "STEWARD_DB=$DB"; echo "STEWARD_ROOT=$ROOT"; echo "STEWARD_FLOOR=$FLOOR"
    echo "STEWARD_PUSH_URL=$PUSH_URL"; echo "STEWARD_PUSH_EVERY=$PUSH_EVERY"; echo "STEWARD_CHECK_EVERY=$CHECK_EVERY"; echo "STEWARD_WATCH=$WATCH"
    [ -n "$TOKEN" ] && echo "STEWARD_TOKEN=$TOKEN"
} > "$TMP"
docker cp "$TMP" "$CONTAINER:$DEST/steward.env"; rm -f "$TMP"
docker exec -u 0 "$CONTAINER" chown "$OWNER" "$DEST/steward.env" 2>/dev/null || true
docker exec -u 0 "$CONTAINER" chmod 600 "$DEST/steward.env"

echo "[3/6] 停掉旧 steward"
docker exec "$CONTAINER" sh -c "[ -x $DEST/stop.sh ] && $DEST/stop.sh >/dev/null 2>&1; true"
if docker exec "$CONTAINER" sh -c "[ -d $LEGACY ] && echo yes" 2>/dev/null | grep -q yes; then
    docker exec -u 0 "$CONTAINER" sh -c "[ -x $LEGACY/stop.sh ] && $LEGACY/stop.sh >/dev/null 2>&1; rm -rf $LEGACY"
fi
echo "[4/6] 重启容器并初始化 steward"
docker restart "$CONTAINER" >/dev/null
docker exec "$CONTAINER" "$DEST/steward" --db "$DB" --root "$ROOT" --init || echo "初始化失败，bili-sync 主程序仍可运行。" >&2
echo "[5/6] 启动 steward"
docker exec -d -e STEWARD_PORT="$PORT" -e STEWARD_DB="$DB" -e STEWARD_ROOT="$ROOT" -e STEWARD_TOKEN="$TOKEN" -e STEWARD_FLOOR="$FLOOR" -e STEWARD_PUSH_URL="$PUSH_URL" -e STEWARD_PUSH_EVERY="$PUSH_EVERY" -e STEWARD_CHECK_EVERY="$CHECK_EVERY" -e STEWARD_WATCH="$WATCH" "$CONTAINER" "$DEST/run.sh"
echo "[6/6] 完成：bili-sync + steward 已更新（版本 v$VERSION，架构 $ARCH）"
if ! docker port "$CONTAINER" 2>/dev/null | grep -q ":$PORT"; then echo "注意：容器没有映射 $PORT，局域网无法打开 steward 页面。"; fi
