#!/usr/bin/env python3
"""对着真机上跑着的 steward 做点击级验证（走本机 fygo-browser 的 CDP，不需要额外的浏览器内核）。

用法：
    python3 test/browser_check.py [页面地址]

前置：
    · steward 已在容器里跑着，页面地址默认 http://127.0.0.1:12346/
    · 先 curl http://127.0.0.1:16002/json/version 把 fygo-browser 唤醒（空闲约 1 分钟会停运行时）
    · 本脚本会真的改库：把某一档的记录在「要下 / 不下」之间来回切一次（切完复原）。
      不做删除、不做重下，盘上文件一律不动。
"""
import asyncio
import json
import sys
import urllib.request

URL = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:12346/"
CDP = "http://127.0.0.1:16003"

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ok   {name}")
    else:
        failed += 1
        print(f"  FAIL {name} {detail}")


def api(path, payload=None):
    req = urllib.request.Request(URL.rstrip("/") + path,
                                 data=None if payload is None else json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"},
                                 method="GET" if payload is None else "POST")
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def wake():
    for _ in range(8):
        try:
            with urllib.request.urlopen("http://127.0.0.1:16002/json/version", timeout=4) as r:
                if r.status == 200:
                    return True
        except Exception:
            pass
        import time
        time.sleep(2)
    return False


async def main():
    if not wake():
        print("fygo-browser 没唤醒")
        return 1
    from playwright.async_api import async_playwright

    async with async_playwright() as p:
        browser = await p.chromium.connect_over_cdp(CDP)
        ctx = await browser.new_context(viewport={"width": 1280, "height": 900})
        page = await ctx.new_page()
        dialogs = []
        page.on("dialog", lambda d: (dialogs.append(d.message), asyncio.ensure_future(d.accept())))
        try:
            await page.goto(URL, wait_until="domcontentloaded")
            await page.wait_for_timeout(4000)

            print("— 首屏 —")
            check("标题是 bili-sync 管家", await page.title() == "bili-sync 管家", await page.title())
            tabs = await page.locator("nav.tabs button").all_text_contents()
            check("三个 tab", tabs == ["视频清单", "本地文件", "下载列表"], tabs)
            banner = (await page.locator("#banner-text").inner_text()).strip()
            check("横幅报出剩余空间", "剩余" in banner and "%" in banner, banner)
            banner_cls = await page.locator("#banner").get_attribute("class")
            check("没拉闸时横幅不报警", "warn" not in (banner_cls or ""), banner_cls)
            check("没拉闸时没有恢复按钮", await page.locator("#btn-release").is_hidden())
            summary = (await page.locator("#summary").inner_text()).strip()
            check("概况行有各档条数", "条记录" in summary and "存在" in summary, summary)
            rec_rows = await page.locator("#rec-list .row").count()
            check("视频清单渲染出行", rec_rows > 0, rec_rows)
            pills = await page.locator("#rec-list .pill").all_text_contents()
            check("清单里有「不下」标记",
                  await page.locator("#rec-list .tag").count() >= 0 and len(pills) == rec_rows)

            print("— 切到下载列表 —")
            await page.locator('nav.tabs button[data-tab="dl"]').click()
            await page.wait_for_timeout(500)
            chips = await page.locator("#q-bucket button").all_text_contents()
            check("三档筛选带条数", len(chips) == 3 and all(c.strip() for c in chips), chips)
            live = api("/api/records")
            q = live["queue"]
            dl_rows = await page.locator("#dl-list .row").count()
            check("默认停在待下载档", dl_rows == q["pending"], (dl_rows, q))
            check("页面条数与接口一致", f"待下载 {q['pending']}" in " ".join(chips).replace("\n", " ").
                  replace("  ", " ") or str(q["pending"]) in chips[0], chips)
            check("页脚出现队列按钮", await page.locator("#foot-dl").is_visible())
            check("页脚没有本地文件的删除按钮", await page.locator("#foot-loc").is_hidden())

            print("— 护栏（真接口，删不该删的）—")
            guard = await page.evaluate("""async () => {
                const out = [];
                for (const p of ['/downloads', '/', '/app/.config/bili-sync', '/downloads/不存在的目录']) {
                    const r = await fetch('/api/delete', {method:'POST',
                        headers:{'Content-Type':'application/json'}, body: JSON.stringify({paths:[p]})});
                    out.push([p, await r.json()]);
                }
                return out;
            }""")
            for path, r in guard:
                check(f"拒绝删除 {path}",
                      r.get("deletedCount") == 0 and r.get("failedCount") == 1, r)

            print("— 真点一次：全选 → 移出下载队列 —")
            if q["pending"] == 0:
                print("  （待下载档是空的，跳过这一组）")
            else:
                await page.locator("#q-all").check()
                await page.wait_for_timeout(300)
                picked = await page.locator("#dl-list .dpick:checked").count()
                check("全选勾上了", picked == dl_rows, (picked, dl_rows))
                foot = await page.locator("#foot-info").inner_text()
                check("页脚报出已选条数", str(dl_rows) in foot, foot)
                check("按钮亮起", await page.locator("#btn-q-unwant").is_enabled())
                await page.locator("#btn-q-unwant").click()
                for _ in range(40):
                    await page.wait_for_timeout(500)
                    now = api("/api/records")["queue"]
                    if now["pending"] == 0:
                        break
                check("接口侧：待下载清空", now["pending"] == 0, now)
                check("接口侧：都进了不下档", now["off"] == q["off"] + q["pending"], now)
                body = await page.locator("#dl-list").inner_text()
                check("页面跟着刷新（不再有待下载行）", "这一档是空的" in body or body.count("待下载") == 0,
                      body[:120])

                print("— 再点回来：不下 → 加入下载队列（只挑 2 条）—")
                await page.locator('#q-bucket button[data-b="off"]').click()
                await page.wait_for_timeout(400)
                off_rows = await page.locator("#dl-list .row").count()
                check("不下档现在有内容", off_rows == now["off"], (off_rows, now))
                boxes = page.locator("#dl-list .dpick")
                await boxes.nth(0).check()
                await boxes.nth(1).check()
                await page.locator("#btn-q-want").click()
                for _ in range(40):
                    await page.wait_for_timeout(500)
                    back = api("/api/records")["queue"]
                    if back["pending"] == 2:
                        break
                check("两条回到待下载", back["pending"] == 2, back)
                check("不下档少了两条", back["off"] == now["off"] - 2, back)

                print("— 把这两条再挪回不下（收尾）—")
                await page.locator('#q-bucket button[data-b="pending"]').click()
                await page.wait_for_timeout(400)
                await page.locator("#q-all").check()
                await page.locator("#btn-q-unwant").click()
                for _ in range(40):
                    await page.wait_for_timeout(500)
                    fin = api("/api/records")["queue"]
                    if fin["pending"] == 0:
                        break
                check("回到全不下", fin["pending"] == 0 and fin["off"] == q["off"] + q["pending"], fin)
                check("确认框弹出过", len(dialogs) >= 3, dialogs)

            print("— 视频清单里能看到「不下」标记 —")
            await page.locator('nav.tabs button[data-tab="rec"]').click()
            await page.wait_for_timeout(400)
            tags = await page.locator("#rec-list .tag").count()
            check("清单里有不下标记", tags > 0, tags)
            check("回到清单后页脚整条收起", await page.locator("#foot").is_hidden())

            print("— 本地文件（下载目录现在是空的）—")
            await page.locator('nav.tabs button[data-tab="loc"]').click()
            await page.wait_for_timeout(800)
            body = await page.locator("#loc-list").inner_text()
            info = await page.locator("#loc-info").inner_text()
            check("空目录有说明", "没扫到" in body or "项" in body, body[:80])
            check("信息行报出文件夹数与总体积", "个视频文件夹" in info, info)

            print("— 横幅报警态（拦下 /api/disk 的应答，只看 DOM）—")
            await page.evaluate("""() => {
                const real = window.fetch;
                window.fetch = async (u, o) => {
                    if (String(u).includes('/api/disk')) {
                        return new Response(JSON.stringify({ok:true, root:'/downloads', total:100,
                            free:5, pct:5, floor:10, human:'5 B', braked:true,
                            note:'已停用 6 个源（快照已记）', pushed:1, lastPush:1, checkEvery:60,
                            pushUrl:'', snapshot:'/x'}), {headers:{'Content-Type':'application/json'}});
                    }
                    return real(u, o);
                };
                if (typeof loadDisk === 'function') loadDisk();
            }""")
            await page.wait_for_timeout(800)
            cls = await page.locator("#banner").get_attribute("class")
            txt = await page.locator("#banner-text").inner_text()
            check("报警态加 warn 类", "warn" in (cls or ""), cls)
            check("横幅写明已拉闸", "已拉闸" in txt and "已停用 6 个源" in txt, txt)
            check("出现恢复按钮", await page.locator("#btn-release").is_visible())
        finally:
            await ctx.close()

    print(f"\n通过 {passed} 项，失败 {failed} 项")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
