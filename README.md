# steward —— bili-sync 管家

`bili-sync` 的「视频」列表是**记录视图**：它读自己的 `data.sqlite`，从不看磁盘。所以在文件管理器里把某个视频删了，列表里那条还在，点进去还写着已下载。反过来，「哪条记录本地其实已经没了」「盘上有什么、占多大、能不能就地删掉」「这条到底还要不要下」这些事，它不管。这些就是 steward 管的。

顺手还管两件更要紧的：新加的源默认只下新视频，不把整个历史投稿拖下来；下载卷快满的时候自己停机，把源全停了、推一条通知，腾出空间再一键恢复。

它不改 bili-sync 源码，也不发布镜像。一个静态单文件小程序，塞进 `bili-sync` 容器里跑，自己开一个端口，读同一个库、看同一个 `/downloads`。

名字取「管家」：仓里有多少货、货还在不在、下一批要不要进、库房还剩几成。这些事原来得自己盯着。

```
容器 bili-sync-rs
├── /app/bili-sync-rs                     上游本体（不动）
└── /app/.config/bili-sync/               ← bind mount，重建容器也不丢
    ├── data.sqlite                       上游的库（这里读写）
    ├── steward/                          注入的这一件
    │   ├── steward                       静态二进制（无依赖）
    │   ├── run.sh                        常驻包装：挂了重起，日志进 steward.log
    │   ├── steward.env                   参数（端口、阈值、推送地址……）
    │   └── stop.sh
    ├── steward-brake-sources.txt         拉闸时的快照（只有在闸门合上时存在）
    └── steward-ondisk.txt                「上次看到哪些条目在盘上」的记忆（核盘用）
```

## 三个视图

**视频清单**（bili-sync「视频」那一栏的数据）按源列出 `video` 表里的每一条，每条给一个本地状态：

| 状态 | 判定 |
| --- | --- |
| 存在 | 该条所有分页（`page.path` 指向的 mp4）都在 |
| 部分 | 有的在有的不在，或者有分页还没落盘 |
| 不存在 | 下过、有落盘路径，但文件一个都不在了 |
| 未下载 | 有分页行，但都没有落盘路径（从没下过） |
| 无分页 | `page` 表里没有它的行 |

判据是分页文件，不是文件夹。多 P 视频会出现「文件夹在、视频没了」这种误判，所以聚合到分页这一层。状态每次打开页面现算（逐个 `stat`），不落库。页面里每个问号鼠标停上去就是同一张表。

**本地文件**扫 `/downloads`，列出每个视频文件夹（含字幕、nfo、封面）的体积与文件数，按来源目录分组，多选可删。盘上有、记录里没有的孤儿文件夹也会列出来，比如对某个源点了「全量更新」但没勾「同时删除本地视频文件夹」。删除是整目录删，记录不动，所以删完回到清单视图，那条自己变成「不存在」。

**下载列表**是库里那两个开关的账：要不要下（`should_download`）和下完没有（分页有没有落盘路径）。三档：待下载 / 已下载 / 不下。勾上几条，底部可以：

| 动作 | 干的事 |
| --- | --- |
| 加入下载队列 | `should_download=1`，下一轮 bili-sync 去下 |
| 移出下载队列 | `should_download=0`，记录留着、文件不动，随时加回来 |
| 重置并重下 | 删掉这条的 `page` 行、`video.download_status` 归零，下一轮重新抓分页再下；盘上旧文件不动 |
| 删除记录 | 删掉 `video` / `page` 行，磁盘上的文件不动 |
| 清理僵尸完成 | 收回「下过但文件已经不在盘上」那些条目的完成标记，清掉落盘路径，并标成「不下」；只动库 |

**清理僵尸完成**解决的是这么一件事：bili-sync 自己从不回头核对盘上的文件。你下过一批、后来又手工把文件删了（或者在「本地文件」栏删了文件夹），库里仍旧记着「已完成」，于是清单里一片「不存在」、下载列表里一片「已下载」。这个按钮把这批条目打回「未下载」，并顺手标成不下。因为只要保留「要下」而状态归零，bili-sync 下一轮就会把它们整份重下一遍。想重下就在「下载列表」里勾上「加入下载队列」，再点「重置并重下」。

它是幂等的：清完再点一次，报 0。只改 `page.path` / `page.download_status` / `video.path` / `video.download_status` / `video.should_download`，不删任何行、不碰任何文件。

「不下」是个纯记账状态，bili-sync 认这个字段，所以它只是不再下这些条目，其余照常。

页面跟随系统明暗：浅色是毛玻璃，深色换成深底 + 低饱和的状态色。被别的界面嵌进 iframe 时，对方可以在地址上带 `?theme=dark|light` 指定（iframe 是独立的源，读不到父页面的主题，只能这么传）；不带这个参数就按系统偏好，跟着系统变。

## 首页概览

打开页面最上面几块，不点进任何视图就能看：

| 板块 | 里面是什么 |
| --- | --- |
| 磁盘 | 下载卷可用空间、剩余百分比与阈值，低于阈值就标「已拉闸」 |
| 网速 | 容器网口的实时收/发速率、网口名、累计收了多少 |
| 视频总账 | 记录条数，以及存在 / 部分 / 不存在 / 未下载 / 无分页各多少 |
| 下载队列 | 待下载 / 已下载 / 不下 三档的条数 |
| 闸门与源 | 闸门合着没有、阈值多少、核盘线程在不在、拉闸推过几次 |

右上「板块」按钮勾选要显示哪几块（「全都要」一次全勾）。选择存在 `steward-prefs.json`，跟 `data.sqlite` 同目录（bind mount），容器重建之后还在。

被别的页面套进 iframe 时地址上带 `?embed=1` 就只渲染这几块（页头、标签、页脚都收起来）；`?theme=dark|light` 也由父页面传。

## 库里另外两件事

**新加的源默认带上过滤规则。** 四个 `AFTER INSERT` 触发器（`steward_default_rule_*`），源入库那一刻把「收藏时间/发布时间 > 此刻」写进 `rule`。触发器跑在 bili-sync 自己的写事务里，所以不存在「源已经入库、规则还没写」的窗口；不然新源的头一轮会把整个历史投稿拖下来，几分钟就能填满盘。自己写过规则的源一律不覆盖。规则的具体形状（`[[{"field":...,"rule":{"operator":...,"value":...}}]]`）是上游自己那套 serde 格式，在 bili-sync 里点开源的详情能看到它渲染出来的中文描述。

**磁盘闸门。** 每 60 秒看一次 `/downloads` 所在卷，可用空间低于阈值（默认 10%）时：把当时开着的源记进快照、全部停用（`enabled=0`），然后每 15 分钟往推送地址发一条通知。不停容器、也不掐正在跑的下载。那一轮让它跑完，下一轮开始就没有源可下了。腾出空间后，页顶横幅上会出现「恢复被停用的源」，按快照把当时那些源开回来（不是无脑全开：快照里没有的源不动）。恢复不会自动发生，得你点。

停用的是四个源表里的 `enabled` 字段，bili-sync 每轮从库里读，最多一轮生效。

**推什么、不推什么。** 核盘和闸门是一起走的：每轮先数一遍盘，某条从「一个文件都没有」变成「这一条的分页文件全在盘上」，推「下载完成 N 条」（带标题，超过 10 条只报数）；反过来从「全在」变成「记录还在、落盘路径也还写着、文件却一个都不剩」，推「文件不在了」，后半句就是叫你去点「清理僵尸完成」。已经推过的记在 `steward-ondisk.txt`，同一条不重复推；装上去的头一轮只记基线、不推，免得把现有库存全报一遍。

之所以不直接转发 bili-sync 自己的通知：它只发两类，「N 条新视频已入库」和下载失败告警，而「入库」说的是五道子任务的状态位都变绿了，跟文件在不在盘上是两件事。状态位被重放的时候，几年前的旧条目也会重新算成「入库」。所以更省事的做法是在**转发那一层**（你自己的 webhook 中转脚本里）加一条关键词过滤：正文里含「已入库」的直接吞掉，失败告警照旧放行。

想关掉核盘推送：`--no-watch`，或者装的时候 `WATCH=0 ./install.sh`。

## 轻到什么程度

- 产物是静态单文件，约 1.8 MB：SQLite amalgamation 链进去，运行时零依赖。目标容器里没有 python / node / curl，也不需要。
- 没有 Dockerfile、没有自己的镜像、没有常驻依赖。装进去的东西就是三个文件加一个二进制，全在 bind mount 里，容器重建不用重装。
- 前端是一张内嵌的 HTML（约 45 KB，无外部资源、无框架），页面每次请求现算。
- 除了 GNU libc 和 SQLite（公有领域），没有第三方代码。

## 编译

需要 gcc 与 python3，无其它依赖：

```sh
./build.sh          # 产出 dist/steward
```

`build.sh` 把 `web/index.html` 嵌成 C 字符串，再和 `vendor/sqlite3.c` 一起静态链接。`sqlite3.c` 单独编且有缓存（RK3566 上 `-O1` 约两分钟），之后改前端或改 `steward.c` 是秒级重编。脚本认 `CC`，跨架构写 `CC=aarch64-linux-gnu-gcc ./build.sh`。

`aarch64` 与 `x86_64` 两套成品在打 tag 时由 GitHub Actions 各编一份（`.github/workflows/release.yml`），随 release 一起发布，不必自己动手。

## 安装

需要 docker 权限（在能跑 docker 的账号下执行）。两条路等价，任选一条：

```sh
# 一、从仓库装。仓里没有二进制时，脚本按 uname -m 去 Releases 取对应架构的那份
git clone https://github.com/molakesizhanfangguangmang/bili-sync-steward.git
cd bili-sync-steward && ./install.sh

# 二、从 Releases 下完整包
tar xzf steward-v1.0.2-x86_64.tar.gz
cd steward-v1.0.2-x86_64 && ./install.sh
```

认得的架构是 `aarch64`（`uname -m` 报 `aarch64` 或 `arm64`）与 `x86_64`（报 `x86_64` 或 `amd64`）；按的是 bili-sync 容器所在机器。取回来的文件在写入前照 `SHA256SUMS` 校验一次。两条路都可以绕开取货：自己 `./build.sh` 编好，或手工把二进制放到 `dist/steward`，再跑 `FETCH=0 ./install.sh`。

参数：

```sh
./install.sh                                  # 默认容器 bili-sync-rs、端口 12346、阈值 10%
PORT=12346 FLOOR=10 ./install.sh              # 显式给
TOKEN=随便一串 ./install.sh                    # 开 token 校验
PUSH_URL=http://10.0.0.9:8000/hook ./install.sh  # 拉闸时往这儿推
FETCH=0 ./install.sh                          # 只用本地 dist/steward，不联网取
VERSION=1.0.1 ./install.sh                    # 取指定版本（默认 1.0.2）
```

它做五件事：把二进制和三个 sh 拷进容器、停掉上一份、建触发器并给空规则的源补默认规则（`--init`，幂等）、`docker exec -d` 起进程、容器内自检 `/healthz`。参数写进 `steward.env`（0600），所以容器重建后手敲一条命令也能按原参数起来：

```sh
docker exec -d bili-sync-rs /app/.config/bili-sync/steward/run.sh
```

**端口**：容器默认只映射 bili-sync 自己的 12345。要让局域网打开这个页面，在 `docker-compose.yml` 的 `ports:` 下加一行，然后 `docker compose up -d`：

```yaml
    ports:
      - 12345:12345
      - 12346:12346
```

`install.sh` 会检查这件事，没映射就提示。`run.sh` 挂了会自己重起（5 秒后），日志在 `steward/steward.log`。

## 卸掉

```sh
./uninstall.sh      # 停进程 + 删容器里那份目录
```

下载文件、`data.sqlite` 都不动。想连触发器一起清掉（比如打算换回原样）：

```sh
docker exec bili-sync-rs /app/.config/bili-sync/steward/steward --db /app/.config/bili-sync/data.sqlite --help
# 卸完之后在 sqlite 里执行：
#   DROP TRIGGER IF EXISTS steward_default_rule_favorite;   （四张源表各一个）
```

已经写进各源的 `rule` 由你处置：留着就是「只下新视频」，清空就是回到上游默认的「全都下」。

## 安全

- 默认没有鉴权，同一局域网内谁都能打开，也就意味着能删 `/downloads` 里的文件夹、能改下载队列、能停用源。要么加 `TOKEN=`，要么别把端口露出去。
- 开了 `TOKEN` 后所有 `/api/*` 都要带 `?token=<值>` 或 `X-Token:` 请求头；首页不带也能打开，但页面里的请求会被拒。
- 会写 `data.sqlite`：`video.should_download`、`video.download_status`、`video.path`、`page.download_status`、`page.path`，四个源表的 `enabled`，以及建那四个触发器。用的是和 bili-sync 同一个连接方式（WAL + `busy_timeout=5000`），不主动改 journal 模式，也不碰别的表。`/api/records` 那条读路径仍是只读连接。
- 删文件只做文件系统删除，`rm -rf` 语义，不进回收站。

## 局限

- 依赖 `data.sqlite` 的表结构：`video`、`page` 和四张源表。上游改表结构就得跟着改（数据库迁移后重启一下 steward 即可把触发器补回来）。
- 页面是 bili-sync 原生界面外头独立一站；上游自己的「视频」页不会有角标。
- 状态每次请求现算（一千多条记录约 1–2 秒），没做缓存。
- 目录结构按「一个视频一个文件夹、分页 mp4 在文件夹里」这一种布局判定。保存路径模板换花样（比如把视频直接摊在源目录下）会扫不到。
- 闸门只在 steward 活着的时候工作。容器停着的时候它也不在，但那种时候也没人在下载。

## 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 页面 |
| GET | `/healthz` | `ok` |
| GET | `/api/records` | 清单 + 本地状态 + 三档计数（JSON） |
| GET | `/api/files` | `/downloads` 扫描结果（JSON） |
| GET | `/api/disk` | 卷用量、阈值、闸门状态（JSON） |
| GET | `/api/net` | 容器网口的收发速率、累计字节、网口名（JSON） |
| GET | `/api/prefs` | 首页板块配置；一次都没存过返回 `null` |
| POST | `/api/prefs` | `{"blocks":["disk","net","videos","queue","gate"]}`，不认识的块名 400 |
| POST | `/api/delete` | `{"paths":["/downloads/.../某个视频"]}` |
| POST | `/api/queue` | `{"action":"want\|unwant\|delete\|redownload","ids":[12,34]}` |
| POST | `/api/purge` | `{"action":"stale"}` 清理僵尸完成标记；`{"action":"stale","dry":true}` 只数不改 |
| POST | `/api/brake` | `{"action":"release"}` 照快照恢复源；`{"action":"on"}` 手动拉一次闸 |
| POST | `/api/watch` | `{"action":"baseline"}` 把此刻在盘上的这批记成新基线（不推） |

设了 `TOKEN` 时都要带 `?token=` 或 `X-Token:`。

命令行（都在容器里跑，`--db` 指向上游那个库）：

```sh
steward --selftest      # 三个视图 + 磁盘 + 触发器数量，各打一行
steward --init          # 建触发器 + 给空规则的源补默认规则（幂等，install.sh 会调）
steward --pending-off   # 把「要下、但一页都没落过盘」的条目改成不下
```

## 离线自测

不需要 docker，也不需要真的 bili-sync 数据（夹具从真实 `data.sqlite` 拷一份出来改）：

```sh
python3 test/run_tests.py        # 起进程打接口，176 项断言（会先自动重建夹具）
python3 test/browser_check.py    # 点真页面，走本机 CDP，验三档筛选与队列动作
```

`run_tests.py` 每次先重建夹具——它会真删夹具里的目录、真改夹具里的库。想接着上一次的跑，`FV_KEEP_FIXTURE=1 python3 test/run_tests.py`。

覆盖到：五档状态、孤儿文件夹、删除护栏（根目录/源目录/上级/`..`/不存在）、token、坏 body、原样 UTF-8 请求体、三档计数与四条队列动作、`--init` 与 `--pending-off`、触发器的字段与时刻、闸门（低于阈值自动拉闸 → 快照 → 推送 → 恢复）、坏快照只认白名单、核盘推送（基线不推 → 补回文件推「下载完成」→ 删掉文件推「文件不在了」→ 重新基线）、网速采样与首页板块配置（落盘、去重、白名单、空数组）。

夹具要从真实库里拷一份底子，所以给个路径：`BILI_SYNC_DB=/vol1/@appdata/bili-sync/data.sqlite python3 test/run_tests.py`（默认按容器里的 `/app/.config/bili-sync/data.sqlite` 找）。

拿当前这个真实库做底子时，176 项里 175 过：挂的是「删完文件夹后该条变不存在」那条。旧版（`0e65111`）用同一个底子跑也是同样一条挂，跟这次改动没关系，是夹具底子挑中的那条记录的事。

`test/browser_check.py` 会真点页面上的按钮（含把某一档在「要下 / 不下」之间来回切一次），跑完记录复原；不删文件、不重下。

## 许可

MIT。`vendor/sqlite3.c` 是 SQLite 3.50.4 amalgamation，公有领域。
