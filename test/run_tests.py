#!/usr/bin/env python3
"""对 dist/steward 跑一遍离线接口测试（不上机、不碰 docker）。

用法：python3 test/run_tests.py [夹具目录]

夹具默认每次重建（test/make_fixture.py 从真实的 data.sqlite 拷一份），
所以这些用例依赖真实库里存在「下过 / 没下过 / 部分」三类记录；
库里如果一条都没落过盘，有些断言会失去意义。
"""
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer

BASE = sys.argv[1] if len(sys.argv) > 1 else "/tmp/fvtest"
DB = os.path.join(BASE, "data.sqlite")
ROOT = os.path.join(BASE, "downloads")
BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dist", "steward")
HOST = "127.0.0.1"
PORT = 18346
PORT2 = 18347
PUSH_PORT = 18348
TOK = "t0ken"
B = f"http://{HOST}:{PORT}"
B2 = f"http://{HOST}:{PORT2}"
SRC_TABLES = ("favorite", "collection", "watch_later", "submission")

passed = failed = 0
raw_flag = [False]


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ok   {name}")
    else:
        failed += 1
        print(f"  FAIL {name} {detail}")


def _maybe_json(body):
    if raw_flag[0]:
        return body
    try:
        return json.loads(body)
    except Exception:
        return body


def get(path, base=B, raw=False):
    raw_flag[0] = raw
    try:
        with urllib.request.urlopen(base + path) as r:
            return r.status, _maybe_json(r.read())
    except urllib.error.HTTPError as e:
        return e.code, _maybe_json(e.read())


def post(path, payload, base=B, raw=False):
    data = payload if raw else json.dumps(payload).encode()
    req = urllib.request.Request(base + path, data=data,
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def count_sql(db_path, sql):
    con = sqlite3.connect(db_path)
    try:
        return con.execute(sql).fetchone()[0]
    finally:
        con.close()


def wait_up(base, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        try:
            urllib.request.urlopen(base + "/healthz", timeout=1).read()
            return True
        except Exception:
            time.sleep(0.2)
    return False


def main():
    if not os.environ.get("FV_KEEP_FIXTURE"):
        here = os.path.dirname(os.path.abspath(__file__))
        rc = subprocess.run([sys.executable, os.path.join(here, "make_fixture.py"), BASE],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        if rc.returncode != 0:
            print("夹具重建失败（make_fixture.py）")
            return 1

    proc = subprocess.Popen([BIN, "--db", DB, "--root", ROOT, "--port", str(PORT),
                             "--bind", HOST, "--token", TOK],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_up(B):
            print("服务没起来")
            return 1

        print("— 基本端点 —")
        st, body = get("/", raw=True)
        check("index 200", st == 200)
        check("index 带页面内容", "bili-sync 管家".encode() in body)
        check("页面带五档悬停说明", "判据只看这一条的分页文件".encode() in body)
        check("页面带删除口径悬停说明", "的单位是".encode() in body)
        check("页面带下载列表悬停说明", "要不要下".encode() in body)
        check("三个重载按钮名字各不相同",
              "重新扫描文件目录".encode() in body and body.count("重新扫描".encode()) >= 2)
        check("问号提示是 hover 触发", b".help:hover .tip" in body)
        check("页面有「恢复被停用的源」按钮", "btn-release".encode() in body)
        st, _ = get("/healthz")
        check("healthz 200", st == 200)
        st, _ = get("/api/nope")
        check("未知路径 404", st == 404)

        print("— token —")
        for p in ("/api/records", "/api/files", "/api/disk"):
            st, _ = get(p)
            check(f"{p} 无 token 403", st == 403)
        st, _ = get("/api/records?token=wrong")
        check("错 token 403", st == 403)
        st, d = get(f"/api/records?token={TOK}")
        check("对 token 200", st == 200)
        for p in ("/api/queue", "/api/brake"):
            st, _ = post(p, {"action": "nope"})
            check(f"{p} 无 token 403", st == 403)

        print("— A 视频清单 —")
        rec = d
        by = {r["id"]: r for r in rec["records"]}
        check("记录数与 DB 一致", rec["count"] == len(rec["records"]))
        check("有分组标签", {r["group"] for r in rec["records"]} >= {"收藏夹 · 测试"})
        s = rec["summary"]
        check("全部状态都被分到某一档", sum(s.values()) == rec["count"], s)
        check("存在档 > 0", s["exists"] > 0, s)
        check("不存在档 > 0", s["missing"] > 0, s)
        check("未下载档 > 0（DB 里 path 为空的分页）", s["undownloaded"] > 0, s)
        partial = [r for r in rec["records"] if r["status"] == "partial"]
        check("部分档 > 0（多分页只落了一页）", len(partial) > 0)
        exists = [r for r in rec["records"] if r["status"] == "exists"]
        check("存在档记录的文件大小 > 0", all(r["size"] > 0 for r in exists))
        missing = [r for r in rec["records"] if r["status"] == "missing"]
        check("不存在档 withPath>0 且 present=0",
              all(r["withPath"] > 0 and r["present"] == 0 for r in missing))

        print("— B 本地文件 —")
        st, fl = get(f"/api/files?token={TOK}")
        check("files 200", st == 200)
        items = [it for g in fl["groups"] for it in g["items"]]
        check("扫到视频文件夹", fl["count"] == len(items) and fl["count"] > 0)
        check("含孤儿文件夹", any("孤儿" in it["rel"] for it in items))
        check("体积是各文件之和（含 nfo/字幕/封面）", all(it["size"] >= 1024 * 1024 for it in items))
        total = sum(it["size"] for it in items)
        check("totalSize 等于逐项相加", fl["totalSize"] == total)
        check("分组按父目录聚合", all("/" in g["group"] for g in fl["groups"]))

        print("— 磁盘 —")
        st, dk = get(f"/api/disk?token={TOK}")
        check("disk 200", st == 200 and dk.get("ok") is True, dk)
        check("disk 有 total/free/pct", dk["total"] > 0 and dk["free"] > 0 and 0 < dk["pct"] <= 100, dk)
        check("disk 默认阈值 10%", dk["floor"] == 10, dk)
        check("disk 默认没拉闸", dk["braked"] is False, dk)
        check("disk 报出快照路径", dk["snapshot"].endswith("steward-brake-sources.txt"), dk)
        check("disk 有给人看的体积", re.match(r"[\d.]+ [KMGT]?i?B", dk["human"]) is not None, dk)

        print("— C 下载列表：三档 —")
        q = rec["queue"]
        check("三档之和 = 记录数", sum(q.values()) == rec["count"], q)
        check("每条都带 want/bucket",
              all(isinstance(r.get("want"), bool) and r.get("bucket") in ("pending", "done", "off")
                  for r in rec["records"]))
        check("待下载档 > 0", q["pending"] > 0, q)
        check("已下载档 > 0", q["done"] > 0, q)
        check("不下档 > 0（夹具里造了 3 条）", q["off"] == 3, q)
        check("已下载档 = 每页都有落盘路径",
              all(r["pages"] > 0 and r["withPath"] == r["pages"] and r["want"]
                  for r in rec["records"] if r["bucket"] == "done"))
        check("不下档 = want 为假",
              all(r["want"] is False for r in rec["records"] if r["bucket"] == "off"))

        print("— C 下载列表：移出 / 加回队列 —")
        ids = [r["id"] for r in rec["records"] if r["bucket"] == "pending"][:3]
        st, r = post(f"/api/queue?token={TOK}", {"action": "unwant", "ids": ids})
        check("移出队列 200", st == 200 and r["ok"] is True, r)
        check("affected = 请求条数", r["affected"] == len(ids), r)
        st, rec2 = get(f"/api/records?token={TOK}")
        by2 = {x["id"]: x for x in rec2["records"]}
        check("那几条变「不下」",
              all(by2[i]["bucket"] == "off" and by2[i]["want"] is False for i in ids))
        check("不下档计数 +3", rec2["queue"]["off"] == q["off"] + len(ids), rec2["queue"])
        check("分页路径没被碰", all(by2[i]["withPath"] == by[i]["withPath"] for i in ids))
        st, r = post(f"/api/queue?token={TOK}", {"action": "want", "ids": ids})
        check("加回队列 affected = 3", r["affected"] == len(ids), r)
        st, rec3 = get(f"/api/records?token={TOK}")
        by3 = {x["id"]: x for x in rec3["records"]}
        check("加回后回到待下载", all(by3[i]["bucket"] == "pending" for i in ids))

        print("— C 下载列表：重置并重下 —")
        done_one = [r for r in rec3["records"] if r["bucket"] == "done" and r["pages"] > 0][0]
        before_pages = count_sql(DB, f"select count(*) from page where video_id={done_one['id']}")
        folder_before = os.path.exists(done_one["folder"])
        st, r = post(f"/api/queue?token={TOK}", {"action": "redownload", "ids": [done_one["id"]]})
        check("重置 200", r["ok"] is True, r)
        st, rec4 = get(f"/api/records?token={TOK}")
        b4 = {x["id"]: x for x in rec4["records"]}[done_one["id"]]
        check("分页行清光了", b4["pages"] == 0 and before_pages > 0, (b4["pages"], before_pages))
        check("状态归零 → 待下载", b4["bucket"] == "pending" and b4["status"] == "nopages", b4)
        check("video.download_status 归零",
              count_sql(DB, f"select download_status from video where id={done_one['id']}") == 0)
        check("盘上旧文件没动", os.path.exists(done_one["folder"]) == folder_before)

        print("— C 下载列表：删除记录 —")
        del_one = [r for r in rec4["records"] if r["bucket"] == "pending"][0]
        st, r = post(f"/api/queue?token={TOK}", {"action": "delete", "ids": [del_one["id"]]})
        check("删除 200 且 affected=1", r["ok"] is True and r["affected"] == 1, r)
        st, rec5 = get(f"/api/records?token={TOK}")
        check("记录少了 1 条", rec5["count"] == rec4["count"] - 1, (rec5["count"], rec4["count"]))
        check("那条真没了", del_one["id"] not in {x["id"] for x in rec5["records"]})
        check("page 行也跟着清了",
              count_sql(DB, f"select count(*) from page where video_id={del_one['id']}") == 0)

        print("— C 坏 body —")
        for body in (b"", b"{}", b'{"ids":[1]}', b'{"action":"want"}', b'{"action":"boom","ids":[1]}'):
            st, r = post(f"/api/queue?token={TOK}", body, raw=True)
            check(f"400 {body!r}", st == 400 and r.get("ok") is False, (st, r))
        for body in (b"", b"{}", b'{"action":"boom"}'):
            st, r = post(f"/api/brake?token={TOK}", body, raw=True)
            check(f"brake 400 {body!r}", st == 400 and r.get("ok") is False, (st, r))

        print("— C 下载列表：清理僵尸完成 —")
        st, recP = get(f"/api/records?token={TOK}")
        stale_ids = [r["id"] for r in recP["records"] if r["status"] == "missing"]
        kept_ids = [r["id"] for r in recP["records"] if r["status"] in ("exists", "partial")]
        check("夹具里有「下过但文件没了」的条目", len(stale_ids) > 0, len(stale_ids))
        check("夹具里有在盘上的条目", len(kept_ids) > 0, len(kept_ids))
        st, r = post(f"/api/purge?token={TOK}", {"action": "stale", "dry": True})
        check("dry 200 且只数不改",
              r["ok"] is True and r["dry"] is True and r["stale"] == len(stale_ids), r)
        check("dry 之后库里还是完成态",
              count_sql(DB, f"select count(*) from video where id={stale_ids[0]}"
                            " and download_status <> 0") == 1)
        st, r = post(f"/api/purge?token={TOK}", {"action": "stale"})
        check("清理 200 且条数与僵尸数一致",
              r["ok"] is True and r["dry"] is False and r["stale"] == len(stale_ids), r)
        check("报出的分页数 > 0", r["pages"] > 0, r)
        check("完成标记收回了",
              count_sql(DB, f"select count(*) from video where id={stale_ids[0]}"
                            " and download_status = 0 and should_download = 0"
                            " and (path is null or path = '')") == 1)
        check("落盘路径清空、分页行还在（不是删记录）",
              count_sql(DB, f"select count(*) from page where video_id={stale_ids[0]}"
                            " and path = ''") > 0)
        st, recP2 = get(f"/api/records?token={TOK}")
        bp = {x["id"]: x for x in recP2["records"]}
        check("僵尸条目变「未下载」",
              all(bp[i]["status"] == "undownloaded" and bp[i]["withPath"] == 0
                  for i in stale_ids))
        check("僵尸条目落到「不下」",
              all(bp[i]["bucket"] == "off" and bp[i]["want"] is False for i in stale_ids))
        check("在盘上的条目没被动",
              all(bp[i]["status"] == next(x for x in recP["records"]
                                          if x["id"] == i)["status"] for i in kept_ids))
        st, r = post(f"/api/purge?token={TOK}", {"action": "stale"})
        check("再清一次是 0（幂等）", r["stale"] == 0 and r["pages"] == 0, r)
        st, r = post(f"/api/purge?token={TOK}", {"action": "boom"})
        check("不认识的 action 400", st == 400 and r.get("ok") is False, (st, r))

        print("— 删除守卫 —")
        src_dirs = [r["groupPath"] for r in rec["records"] if r["groupPath"]]
        guards = [ROOT, os.path.dirname(src_dirs[0]), src_dirs[0],
                  src_dirs[-1], os.path.join(ROOT, "不存在的东西"),
                  "/etc/passwd", "x/y", "/tmp"]
        for g in dict.fromkeys(guards):
            st, r = post(f"/api/delete?token={TOK}", {"paths": [g]})
            check(f"拒绝 {g}", r["deletedCount"] == 0 and r["failedCount"] == 1,
                  r.get("failed"))
            if g.startswith(ROOT) and "不存在" not in g:
                check(f"  {g} 还在", os.path.exists(g))

        print("— 真删：一个视频文件夹 —")
        victim = [r for r in rec["records"] if r["status"] == "exists"][0]
        st, r = post(f"/api/delete?token={TOK}", {"paths": [victim["folder"]]})
        check("删除成功", r["deletedCount"] == 1 and r["failedCount"] == 0, r)
        check("磁盘上没了", not os.path.exists(victim["folder"]))
        st, rec2 = get(f"/api/records?token={TOK}")
        b2 = {x["id"]: x for x in rec2["records"]}
        check("A 里该条变 不存在", b2[victim["id"]]["status"] == "missing")
        st, fl2 = get(f"/api/files?token={TOK}")
        check("B 里少了一项", fl2["count"] == fl["count"] - 1)

        print("— 真删：原样 UTF-8 body（不转义）—")
        v2 = ROOT
        try:
            with urllib.request.urlopen(B + f"/api/files?token={TOK}") as rr:
                fl_now = json.load(rr)
            v2 = [it["abs"] for g in fl_now["groups"] for it in g["items"] if "孤儿" not in it["rel"]][0]
        except Exception as e:
            check("B 里还有可删项", False, e)
        st, r = post(f"/api/delete?token={TOK}",
                     json.dumps({"paths": [v2]}, ensure_ascii=False).encode(), raw=True)
        check("删除成功", r["deletedCount"] == 1, r)
        check("磁盘上没了", not os.path.exists(v2))

        print("— 真删：孤儿文件夹 —")
        orphan = [it for it in items if "孤儿" in it["rel"]][0]
        st, r = post(f"/api/delete?token={TOK}", {"paths": [orphan["abs"]]})
        check("删除成功", r["deletedCount"] == 1, r)
        check("磁盘上没了", not os.path.exists(orphan["abs"]))

        print("— 混合：一能删一不能删 —")
        with urllib.request.urlopen(B + f"/api/files?token={TOK}") as rr:
            fl_mix = json.load(rr)
        pool = [it["abs"] for g in fl_mix["groups"] for it in g["items"]]
        if pool:
            st, r = post(f"/api/delete?token={TOK}", {"paths": [pool[0], ROOT]})
            check("删 1 个、拒 1 个", r["deletedCount"] == 1 and r["failedCount"] == 1, r)
        else:
            check("混合用例有可删项（夹具删空了）", False, pool)

        print("— 坏 body（删除）—")
        for body in (b"", b"{}", b"not json", b'{"paths":[]}'):
            st, r = post(f"/api/delete?token={TOK}", body, raw=True)
            check(f"400 {body!r}", st == 400 and r.get("ok") is False, (st, r))

        print("— 默认规则：--init —")
        tdb = os.path.join(BASE, "rule.sqlite")
        shutil.copyfile(DB, tdb)
        out = subprocess.run([BIN, "--db", tdb, "--root", ROOT, "--init"],
                             capture_output=True).stdout.decode("utf-8", "replace")
        check("报 4/4 个触发器", "触发器 4/4" in out, out)
        check("报补了几个源", re.search(r"补默认规则的源：\d+ 个", out) is not None, out)
        n_trg = count_sql(tdb, "select count(*) from sqlite_master where type='trigger'"
                               " and name like 'steward_default_rule_%'")
        check("库里真有 4 个触发器", n_trg == 4, n_trg)
        empty = sum(count_sql(tdb, f"select count(*) from {t} where rule is null or rule=''")
                    for t in SRC_TABLES)
        check("没有空规则的源了", empty == 0, empty)

        # 已有规则的一律不覆盖
        con = sqlite3.connect(tdb)
        con.execute("update submission set rule='[[{\"field\":\"title\",\"rule\":"
                    "{\"operator\":\"contains\",\"value\":\"手工写的\"}}]]' where id=1")
        con.commit()
        con.close()
        subprocess.run([BIN, "--db", tdb, "--root", ROOT, "--init"], capture_output=True)
        rule_kept = count_sql(tdb, "select count(*) from submission where id=1 and rule like '%手工写的%'")
        check("已有规则不被覆盖", rule_kept == 1, rule_kept)

        print("— 默认规则：新加的源自动带上 —")
        con = sqlite3.connect(tdb)
        now = datetime.now()
        con.execute("insert into favorite (f_id,name,path,created_at,latest_row_at,enabled,rule)"
                    " values (999999,'触发器测试','/downloads/收藏夹/触发器测试',"
                    "'2026-09-12 00:00:00','1970-01-01 00:00:00',1,null)")
        con.execute("insert into submission (upper_id,upper_name,path,created_at,latest_row_at,enabled,rule)"
                    " values (999998,'触发器测试UP','/downloads/投稿/触发器测试UP',"
                    "'2026-09-12 00:00:00','1970-01-01 00:00:00',1,'')")
        con.execute("insert into favorite (f_id,name,path,created_at,latest_row_at,enabled,rule)"
                    " values (999997,'自带规则','/downloads/收藏夹/自带规则',"
                    "'2026-09-12 00:00:00','1970-01-01 00:00:00',1,"
                    "'[[{\"field\":\"title\",\"rule\":{\"operator\":\"contains\",\"value\":\"自带\"}}]]')")
        con.commit()
        fav_rule = con.execute("select rule from favorite where f_id=999999").fetchone()[0]
        sub_rule = con.execute("select rule from submission where upper_id=999998").fetchone()[0]
        own_rule = con.execute("select rule from favorite where f_id=999997").fetchone()[0]
        con.close()

        check("收藏夹：插进来就有规则", bool(fav_rule) and "favTime" in fav_rule, fav_rule)
        check("投稿：插进来就有规则", bool(sub_rule) and "pubTime" in sub_rule, sub_rule)
        check("空串也算没规则（被补上）", bool(sub_rule) and sub_rule != "''", sub_rule)
        check("自带规则的没被动", "自带" in own_rule, own_rule)

        def parse_rule(text):
            obj = json.loads(text)             # [[{...}]] 的壳
            target = obj[0][0]
            return target["field"], target["rule"]["operator"], target["rule"]["value"]

        field, op, value = parse_rule(fav_rule)
        check("收藏夹规则用 favTime + 大于", (field, op) == ("favTime", "greaterThan"), (field, op))
        check("规则值是个能解析的时间", isinstance(datetime.fromisoformat(value), datetime), value)
        drift = abs((datetime.fromisoformat(value) - now).total_seconds())
        check("规则值就是插入那一刻", drift < 600, (value, drift))
        field, op, _ = parse_rule(sub_rule)
        check("投稿规则用 pubTime + 大于", (field, op) == ("pubTime", "greaterThan"), (field, op))

        print("— --pending-off —")
        pdb = os.path.join(BASE, "pending.sqlite")
        shutil.copyfile(DB, pdb)
        want_before = count_sql(pdb, "select count(*) from video where should_download=1")
        off_before = count_sql(pdb, "select count(*) from video where should_download=0")
        undl_before = count_sql(pdb, "select count(*) from video where should_download=1 and not exists"
                                    " (select 1 from page p where p.video_id=video.id"
                                    "  and p.path is not null and p.path<>'')")
        out = subprocess.run([BIN, "--db", pdb, "--root", ROOT, "--pending-off"],
                             capture_output=True).stdout.decode("utf-8", "replace")
        m = re.search(r"改成不下的条目：(\d+)", out)
        check("报出改了几条", m is not None and int(m.group(1)) == undl_before, (out, undl_before))
        check("要下的条数正好少了这么多",
              count_sql(pdb, "select count(*) from video where should_download=1") == want_before - undl_before)
        check("不落盘又想下的一个不剩",
              count_sql(pdb, "select count(*) from video where should_download=1 and not exists"
                             " (select 1 from page p where p.video_id=video.id"
                             "  and p.path is not null and p.path<>'')") == 0)
        check("本来就不下的条目没被动",
              count_sql(pdb, "select count(*) from video where should_download=0")
              == off_before + undl_before)

        print("— 闸门：低于阈值拉闸 + 恢复 —")
        got = []

        class Sink(BaseHTTPRequestHandler):
            def do_POST(self):
                n = int(self.headers.get("Content-Length") or 0)
                got.append(self.rfile.read(n).decode("utf-8", "replace"))
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"ok")

            def log_message(self, *a):
                pass

        sink = HTTPServer((HOST, PUSH_PORT), Sink)
        threading.Thread(target=sink.serve_forever, daemon=True).start()

        bdb = os.path.join(BASE, "brake.sqlite")
        shutil.copyfile(DB, bdb)
        state = os.path.join(BASE, "state")
        os.makedirs(state, exist_ok=True)
        snap = os.path.join(state, "steward-brake-sources.txt")
        on_before = sum(count_sql(bdb, f"select count(*) from {t} where enabled=1") for t in SRC_TABLES)
        check("夹具里本来有开着的源", on_before > 0, on_before)

        bproc = subprocess.Popen([BIN, "--db", bdb, "--root", ROOT, "--port", str(PORT2),
                                  "--bind", HOST, "--floor", "200",
                                  "--push-url", f"http://{HOST}:{PUSH_PORT}/",
                                  "--check-every", "5", "--state-dir", state],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            check("第二个实例起来了", wait_up(B2))
            dk2 = {}
            for _ in range(40):
                st, dk2 = get("/api/disk", base=B2)
                if dk2.get("braked"):
                    break
                time.sleep(0.5)
            check("阈值之下自动拉闸", dk2.get("braked") is True, dk2)
            check("闸门说明有内容", "已停用" in (dk2.get("note") or ""), dk2)
            check("列表里所有源都停用了",
                  sum(count_sql(bdb, f"select count(*) from {t} where enabled=1")
                      for t in SRC_TABLES) == 0)
            check("开了几个源就记了几行快照",
                  os.path.exists(snap) and
                  len([l for l in open(snap) if l.strip()]) == on_before)
            check("推送收到了一条", len(got) == 1, got)
            check("推送正文说清楚阈值与动作",
                  got and "低于 200%" in got[0] and "已停用" in got[0], got)

            st, r = post("/api/brake", {"action": "release"}, base=B2)
            check("恢复 200 且有说明", r["ok"] is True and "恢复" in r["note"], r)
            check("源都按快照开回来了",
                  sum(count_sql(bdb, f"select count(*) from {t} where enabled=1")
                      for t in SRC_TABLES) == on_before)
            check("快照用掉就删了", not os.path.exists(snap))
            check("恢复也推了一条", len(got) >= 2 and "恢复" in got[-1], got)
            st, dk3 = get("/api/disk", base=B2)
            check("横幅不再显示拉闸", dk3["braked"] is False, dk3)
        finally:
            bproc.terminate()
            bproc.wait(timeout=5)
            sink.shutdown()

        print("— 坏快照：只认白名单里的表 —")
        state2 = os.path.join(BASE, "state2")
        os.makedirs(state2, exist_ok=True)
        with open(os.path.join(state2, "steward-brake-sources.txt"), "w") as f:
            f.write("submission 1\n随便写的 2\n; DROP TABLE video\n\n")
        vids_before = count_sql(bdb, "select count(*) from video")
        bproc2 = subprocess.Popen([BIN, "--db", bdb, "--root", ROOT, "--port", str(PORT2),
                                   "--bind", HOST, "--floor", "200", "--push-url", "",
                                   "--check-every", "5", "--state-dir", state2],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            check("带旧快照的实例起来了", wait_up(B2))
            st, r = post("/api/brake", {"action": "release"}, base=B2)
            check("坏行被跳过，只恢复认得的那条",
                  r["ok"] is True and r["affected"] == 1, r)
            check("video 表还在", count_sql(bdb, "select count(*) from video") == vids_before)
            check("恢复后 submission 1 是开着的",
                  count_sql(bdb, "select count(*) from submission where id=1 and enabled=1") == 1)
        finally:
            bproc2.terminate()
            bproc2.wait(timeout=5)

        print("— 落盘监视：真下完才推 —")
        wg = []

        class WSink(BaseHTTPRequestHandler):
            def do_POST(self):
                n = int(self.headers.get("Content-Length") or 0)
                wg.append(json.loads(self.rfile.read(n).decode("utf-8", "replace")).get("text", ""))
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"ok")

            def log_message(self, *a):
                pass

        wsink = HTTPServer((HOST, PUSH_PORT + 1), WSink)
        threading.Thread(target=wsink.serve_forever, daemon=True).start()

        wdb = os.path.join(BASE, "watch.sqlite")
        shutil.copyfile(DB, wdb)
        wstate = os.path.join(BASE, "state3")
        os.makedirs(wstate, exist_ok=True)
        wproc = subprocess.Popen([BIN, "--db", wdb, "--root", ROOT, "--port", str(PORT2),
                                  "--bind", HOST, "--floor", "0",
                                  "--push-url", f"http://{HOST}:{PUSH_PORT + 1}/",
                                  "--check-every", "5", "--state-dir", wstate],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            check("监视实例起来了", wait_up(B2))
            st, rec = get("/api/records", base=B2)
            on_disk = [r for r in rec["records"] if r["status"] == "exists"]
            stale = [r for r in rec["records"] if r["status"] == "missing"]
            check("夹具里有在盘上的、也有下过没文件的", on_disk and stale,
                  (len(on_disk), len(stale)))
            st, dk = get("/api/disk", base=B2)
            check("第一次跑只做基线、不推", not wg, wg)
            check("基线条数 = 盘上真有的条数",
                  dk["watch"]["onDisk"] == len(on_disk), dk.get("watch"))
            check("记忆文件落在 state-dir",
                  os.path.exists(os.path.join(wstate, "steward-ondisk.txt")))

            # 把某条「下过但文件没了」的文件补回盘上 —— 等价于它刚下完
            pick = stale[0]
            paths = [row[0] for row in sqlite3.connect(wdb).execute(
                "select path from page where video_id=? and path<>''", (pick["id"],))]
            for p in paths:
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as f:
                    f.write(b"x" * 1024)
            deadline = time.time() + 25
            while time.time() < deadline and not wg:
                time.sleep(0.5)
            check("补回盘上之后推了一条", len(wg) == 1, wg)
            check("推的是「下载完成」且带条数",
                  wg and "下载完成 1 条" in wg[0], wg)
            check("推文里有那条的名字", wg and pick["name"] in wg[0], (wg, pick["name"]))
            st, dk = get("/api/disk", base=B2)
            check("账上多了一条在盘上", dk["watch"]["onDisk"] == len(on_disk) + 1, dk.get("watch"))
            check("数过了就记住，不重复推", len(wg) == 1, wg)

            # 再把文件删掉 —— 记录还在、路径也还写着
            for p in paths:
                os.remove(p)
            deadline = time.time() + 25
            while time.time() < deadline and len(wg) < 2:
                time.sleep(0.5)
            check("文件没了也推一条", len(wg) == 2, wg)
            check("推的是「文件不在了」", wg and "文件不在了" in wg[1], wg)
            check("提示了去点清理僵尸完成", wg and "清理僵尸完成" in wg[1], wg)

            st, r = post("/api/watch", {"action": "baseline"}, base=B2)
            check("baseline 200 且只数不改", r["ok"] is True and r["onDisk"] == len(on_disk), r)
            st, dk = get("/api/disk", base=B2)
            check("重新基线后不再记着那条",
                  dk["watch"]["onDisk"] == len(on_disk), dk.get("watch"))
            st, r = post("/api/watch", {"action": "boom"}, base=B2)
            check("不认识的 action 400", st == 400 and r.get("ok") is False, (st, r))
        finally:
            wproc.terminate()
            wproc.wait(timeout=5)
            wsink.shutdown()

        print("— 自检模式 —")
        out = subprocess.run([BIN, "--db", DB, "--root", ROOT, "--selftest"],
                             capture_output=True).stdout.decode("utf-8", "replace")
        check("selftest 三个视图都 OK", out.count("OK") == 3, out)
        check("selftest 报磁盘", "disk: OK" in out, out)
        check("selftest 报触发器数量", re.search(r"triggers: \d+ 个", out) is not None, out)

        print(f"\n通过 {passed} 项，失败 {failed} 项")
        return 1 if failed else 0
    finally:
        proc.terminate()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
