#!/bin/sh
# 编译 steward：aarch64 静态单文件二进制
# 用法：./build.sh        产物在 dist/steward
# 注：sqlite3.c 单独编，-O1（本机 RK3566 上 -O2 要跑好几分钟），且带缓存，改 steward.c 后秒级重编。
set -e
cd "$(dirname "$0")"

# 1) 把网页嵌成 C 字符串
python3 - <<'PY'
data = open('web/index.html', 'rb').read()
out = ['/* 自动生成，勿手改：源文件 web/index.html */',
       'static const char INDEX_HTML[] =']
for i in range(0, len(data), 16):
    out.append('  "' + ''.join('\\x%02x' % b for b in data[i:i+16]) + '"')
out[-1] += ';'
open('src/web_assets.h', 'w').write('\n'.join(out) + '\n')
PY

# 2) 静态编译（glibc 静态 + SQLite amalgamation）
CC=${CC:-gcc}
CFLAGS_SQLITE="-O1 -pthread -Ivendor -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED"
CFLAGS_MAIN="-O2 -static -Wall -pthread -Isrc -Ivendor"

mkdir -p .obj dist

if [ ! -f .obj/sqlite3.o ] || [ vendor/sqlite3.c -nt .obj/sqlite3.o ]; then
    echo "[build] sqlite3.c -> .obj/sqlite3.o（慢，约两分钟）"
    $CC $CFLAGS_SQLITE -c -o .obj/sqlite3.o vendor/sqlite3.c
fi

echo "[build] steward.c -> dist/steward"
$CC $CFLAGS_MAIN -static -o dist/steward src/steward.c .obj/sqlite3.o -lm -ldl

ls -l dist/steward
file dist/steward 2>/dev/null || true
