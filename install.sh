#!/bin/sh
# 把 steward 注入 bili-sync 容器并起进程。需要 docker 权限（在能跑 docker 的账号下跑）。
#
#   ./install.sh
#   PORT=12346 TOKEN=随便一串 ./install.sh
#   FLOOR=10 PUSH_URL=http://10.0.0.9:8310/ ./install.sh
#   WATCH=0 ./install.sh          # 关掉核盘推送（默认开）
#   CONTAINER=bili-sync-rs DB=/app/.config/bili-sync/data.sqlite ROOT=/downloads ./install.sh
#   FETCH=0 ./install.sh          # 不取现成二进制，只用 dist/steward（自己编的或手工放的）
#
# 装进的是容器里那个 bind mount 目录（/app/.config/bili-sync），所以：
#   · 文件在宿主上，容器重建不丢，不用重新拷；
#   · 容器重建后把进程重新拉起来就行：
#       docker exec -d bili-sync-rs /app/.config/bili-sync/steward/run.sh
#     参数记在同目录的 steward.env 里，run.sh 每次起来会先读它。
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
TMP=${TMPDIR:-/tmp}/steward.env.$$
REPO=${REPO:-molakesizhanfangguangmang/bili-sync-steward}
VERSION=${VERSION:-1.0.2}
FETCH=${FETCH:-1}

# 仓里没有二进制时按本机架构去 release 取一份。取的是静态单文件，校验过 SHA256 才用。
if [ ! -f "$SRC/dist/steward" ] && [ "$FETCH" != "0" ]; then
    case "$(uname -m)" in
        aarch64|arm64) ARCH=aarch64 ;;
        x86_64|amd64)  ARCH=x86_64 ;;
        *)             ARCH= ;;
    esac
    FILE="steward-v$VERSION-$ARCH"
    BASE="https://github.com/$REPO/releases/download/v$VERSION"

    if [ -z "$ARCH" ]; then
        cat >&2 <<EOF
本机架构 $(uname -m) 没有现成的二进制。自行编译，或去
    $BASE
下一份对应架构的，改名放进 $SRC/dist/steward（或显式给 ARCH=架构名）。
EOF
        exit 1
    fi
    if command -v curl >/dev/null 2>&1; then
        DL="curl -fsSL -o"
    elif command -v wget >/dev/null 2>&1; then
        DL="wget -qO"
    else
        echo "没有 curl 也没有 wget，取不了 $FILE。去 $BASE 手动下载后放进 $SRC/dist/steward。" >&2
        exit 1
    fi

    echo "[0/5] 本地没有 dist/steward，取 $FILE"
    mkdir -p "$SRC/dist"
    # shellcheck disable=SC2086
    $DL "$SRC/dist/steward" "$BASE/$FILE"
    chmod 755 "$SRC/dist/steward"

    if SUMFILE=${TMPDIR:-/tmp}/SHA256SUMS.$$; $DL "$SUMFILE" "$BASE/SHA256SUMS" 2>/dev/null; then
        want=$(awk -v f="$FILE" '$2 == f { print $1 }' "$SUMFILE")
        got=$(sha256sum "$SRC/dist/steward" 2>/dev/null | awk '{ print $1 }')
        rm -f "$SUMFILE"
        if [ -n "$want" ] && [ "$want" != "$got" ]; then
            echo "SHA256 对不上（清单 $want，实际 $got），已删掉取回来的文件。" >&2
            rm -f "$SRC/dist/steward"
            exit 1
        fi
    fi
fi

if [ ! -f "$SRC/dist/steward" ]; then
    echo "没有 $SRC/dist/steward。跑 ./build.sh 自己编，或去掉 FETCH=0 让脚本去取。" >&2
    exit 1
fi

command -v docker >/dev/null 2>&1 || { echo "找不到 docker" >&2; exit 1; }
docker inspect "$CONTAINER" >/dev/null 2>&1 || { echo "找不到容器 $CONTAINER" >&2; exit 1; }

echo "[1/5] 拷贝到容器 $CONTAINER:$DEST"
docker exec -u 0 "$CONTAINER" mkdir -p "$DEST"
docker cp "$SRC/dist/steward" "$CONTAINER:$DEST/steward"
docker cp "$SRC/run.sh"       "$CONTAINER:$DEST/run.sh"
docker cp "$SRC/stop.sh"      "$CONTAINER:$DEST/stop.sh"
# 归属和权限：容器本体以非 root 跑（compose 里的 user:），得让它写日志、
# 也得让它能写 data.sqlite（库的属主就是那个用户，写权限靠这个）
docker exec -u 0 "$CONTAINER" chown -R "$OWNER" "$DEST" 2>/dev/null || \
    echo "  注意：chown 没成功，日志和建触发器可能写不进去" >&2
docker exec -u 0 "$CONTAINER" chmod 755 "$DEST" "$DEST/steward" "$DEST/run.sh" "$DEST/stop.sh"

# 参数落成文件，容器重建后手起也能带上
{
    echo "STEWARD_PORT=$PORT"
    echo "STEWARD_DB=$DB"
    echo "STEWARD_ROOT=$ROOT"
    echo "STEWARD_FLOOR=$FLOOR"
    echo "STEWARD_PUSH_URL=$PUSH_URL"
    echo "STEWARD_PUSH_EVERY=$PUSH_EVERY"
    echo "STEWARD_CHECK_EVERY=$CHECK_EVERY"
    echo "STEWARD_WATCH=$WATCH"
    [ -n "$TOKEN" ] && echo "STEWARD_TOKEN=$TOKEN"
} > "$TMP"
docker cp "$TMP" "$CONTAINER:$DEST/steward.env"
rm -f "$TMP"
docker exec -u 0 "$CONTAINER" chown "$OWNER" "$DEST/steward.env" 2>/dev/null || true
docker exec -u 0 "$CONTAINER" chmod 600 "$DEST/steward.env"

echo "[2/5] 停掉上一份"
docker exec "$CONTAINER" sh -c "[ -x $DEST/stop.sh ] && $DEST/stop.sh >/dev/null 2>&1; true"
# 老名字（fileview）那一份如果还在，一起收拾掉，免得两个进程抢同一个端口
if docker exec "$CONTAINER" sh -c "[ -d $LEGACY ] && echo yes" 2>/dev/null | grep -q yes; then
    docker exec -u 0 "$CONTAINER" sh -c "[ -x $LEGACY/stop.sh ] && $LEGACY/stop.sh >/dev/null 2>&1; rm -rf $LEGACY"
    echo "  清掉了旧目录 $LEGACY"
fi

echo "[3/5] 建默认规则触发器 + 补空规则（幂等）"
docker exec "$CONTAINER" "$DEST/steward" --db "$DB" --root "$ROOT" --init || \
    echo "  这一步没成 —— 库写不进去？bili-sync 会照常下，只是新源没有默认规则" >&2

echo "[4/5] 起进程（docker exec -d，不动 bili-sync 本体）"
docker exec -d \
    -e STEWARD_PORT="$PORT" -e STEWARD_DB="$DB" -e STEWARD_ROOT="$ROOT" \
    -e STEWARD_TOKEN="$TOKEN" -e STEWARD_FLOOR="$FLOOR" -e STEWARD_PUSH_URL="$PUSH_URL" \
    -e STEWARD_PUSH_EVERY="$PUSH_EVERY" -e STEWARD_CHECK_EVERY="$CHECK_EVERY" \
    -e STEWARD_WATCH="$WATCH" \
    "$CONTAINER" "$DEST/run.sh"

echo "[5/5] 自检"
sleep 2
if docker exec "$CONTAINER" sh -c "wget -qO- http://127.0.0.1:$PORT/healthz" 2>/dev/null; then
    echo "  steward 活着：容器内 http://127.0.0.1:$PORT/"
else
    echo "  自检没过，看容器里的日志：docker exec $CONTAINER cat $DEST/steward.log" >&2
fi

if ! docker port "$CONTAINER" 2>/dev/null | grep -q ":$PORT"; then
    cat <<EOF

注意：容器没把 $PORT 映射到宿主机，所以只在容器内部活着，局域网打不开页面。
在 docker-compose.yml 的 ports: 下加一行，然后 docker compose up -d：

      - $PORT:$PORT

（重建容器不丢 $DEST —— 它在 bind mount 里。重建后重新起进程即可：
  docker exec -d $CONTAINER $DEST/run.sh）
EOF
else
    echo "端口 $PORT 已映射，局域网直接开 http://<这台机器的地址>:$PORT/"
fi

if [ -n "$TOKEN" ]; then
    echo "已开 token 校验，访问地址要带上：http://<地址>:$PORT/?token=$TOKEN"
fi
