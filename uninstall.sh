#!/bin/sh
# 从容器里把 steward 拆掉：停进程、删目录。
# 只动注入的那一份，bili-sync 本体、data.sqlite、下载目录都不碰。
# steward 写进库里的东西（默认规则触发器、源的 rule、should_download）留在库里 ——
# 要连触发器一起清掉，见 README 的「卸掉」一节。
set -e

CONTAINER=${CONTAINER:-bili-sync-rs}
DEST=${DEST:-/app/.config/bili-sync/steward}
LEGACY=${LEGACY:-/app/.config/bili-sync/fileview}

docker inspect "$CONTAINER" >/dev/null 2>&1 || { echo "找不到容器 $CONTAINER" >&2; exit 1; }

for d in "$DEST" "$LEGACY"; do
    if docker exec "$CONTAINER" sh -c "[ -d $d ] && echo yes" 2>/dev/null | grep -q yes; then
        docker exec "$CONTAINER" sh -c "[ -x $d/stop.sh ] && $d/stop.sh >/dev/null 2>&1; true"
        docker exec -u 0 "$CONTAINER" rm -rf "$d"
        echo "删了 $d"
    fi
done
echo "完事。库里的触发器和规则没动。"
