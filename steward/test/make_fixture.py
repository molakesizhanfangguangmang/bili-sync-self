#!/usr/bin/env python3
"""离线测试夹具：把真实 data.sqlite 拷一份，把 /downloads 前缀改到夹具目录，
按几种典型情况造文件（全在 / 部分在 / 一个没有 / 从没下过），另放一个「盘上有、库里没有」的孤儿文件夹。

用法：python3 test/make_fixture.py [夹具目录，默认 /tmp/fvtest]
"""
import os
import shutil
import sqlite3
import sys

REAL_DB = os.environ.get("BILI_SYNC_DB", "/app/.config/bili-sync/data.sqlite")  # 拿一份真实库当底子造夹具
BASE = sys.argv[1] if len(sys.argv) > 1 else "/tmp/fvtest"
ROOT = os.path.join(BASE, "downloads")
DB = os.path.join(BASE, "data.sqlite")


def main():
    shutil.rmtree(BASE, ignore_errors=True)
    os.makedirs(ROOT, exist_ok=True)
    shutil.copyfile(REAL_DB, DB)

    con = sqlite3.connect(DB)
    cur = con.cursor()
    cur.execute("UPDATE video SET path = replace(path, '/downloads', ?)", (ROOT,))
    cur.execute("UPDATE page  SET path = replace(path, '/downloads', ?)", (ROOT,))

    # 下载列表那三档都得有料：全部先标「要下」，再挑 3 条标成「不下」
    cur.execute("UPDATE video SET should_download = 1")
    off = [r[0] for r in cur.execute("SELECT id FROM video ORDER BY id LIMIT 3").fetchall()]
    if off:
        cur.execute("UPDATE video SET should_download = 0 WHERE id IN (%s)"
                    % ",".join("?" * len(off)), off)
    con.commit()

    pages = cur.execute(
        "SELECT video_id, path FROM page WHERE path IS NOT NULL AND path <> '' ORDER BY video_id, id"
    ).fetchall()

    # 单页视频：1 全在、2 一个都不造、其余不管；多分页视频：只造第一页（部分）
    single = [p for p in pages if cur.execute(
        "SELECT count(*) FROM page WHERE video_id=?", (p[0],)).fetchone()[0] == 1]
    plan = {}
    for vid, _ in single[:6]:
        plan[vid] = "all"
    for vid, _ in single[6:9]:
        plan[vid] = "none"

    multi = [r for r in cur.execute(
        "SELECT video_id FROM page WHERE path IS NOT NULL AND path <> '' "
        "GROUP BY video_id HAVING count(*) > 1 ORDER BY video_id").fetchall()]
    for row in multi[:2]:
        plan[row[0]] = "first"

    seen = {}
    for vid, path in pages:
        mode = plan.get(vid)
        if mode is None:
            continue
        seen[vid] = seen.get(vid, 0) + 1
        if mode == "all" or (mode == "first" and seen[vid] == 1):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(b"\0" * (1024 * 1024))
            for extra, data in (("info.nfo", b"<nfo/>"), ("danmaku.ass", b"x" * 100), ("cover.jpg", b"\xff" * 50)):
                with open(os.path.join(os.path.dirname(path), extra), "wb") as f:
                    f.write(data)

    # 孤儿：盘上有、DB 里没有
    orphans = []
    for i, vid in enumerate(sorted(plan)[:2]):
        row = cur.execute("SELECT path FROM video WHERE id=?", (vid,)).fetchone()
        if not row or not row[0]:
            continue
        d = os.path.join(os.path.dirname(row[0]), f"孤儿视频{i}__BV1orphan{i}")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, f"BV1orphan{i}.mp4"), "wb") as f:
            f.write(b"\0" * (3 * 1024 * 1024))
        orphans.append(d)

    con.close()
    print(f"夹具就绪: {BASE}")
    print(f"  计划: {plan}")
    print(f"  孤儿: {orphans}")
    print("  库内记录数:", end=" ")
    con = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)
    print(con.execute("select count(*) from video").fetchone()[0])
    con.close()


if __name__ == "__main__":
    main()
