# bili-sync-self

不是上游的 fork，是一个十几 KB 的**编译脚手架**：把上游 pin 住的提交拉到 runner 上，打好
`bili-sync-self.patch`，编出 aarch64（musl 静态）的 `bili-sync-rs`。

私有仓。给自己用的，不发布。

## 里面有什么

| 文件 | 作用 |
| --- | --- |
| `.github/workflows/self-build.yaml` | 全部流程：拉上游 → 打补丁 → bun 编前端 → cross 编 aarch64 → 打包上传 |
| `bili-sync-self.patch` | 对上游 `c900777` 的改动（见下表） |
| `HARNESS.md` | 本文件 |

上游基线：`amtoaer/bili-sync` @ `c900777e78796653fdfb00421879b6a802d1eedd`（`UPSTREAM_PIN`）。

## 补丁改了什么

前端：

- `web/src/lib/components/app-sidebar.svelte`：「内容管理」组加一项「存储」→ `/storage`
- `web/src/routes/storage/+page.svelte`：新增，iframe 指向 `location.hostname:12346`（steward 管家），
  并把当前明暗当 `?theme=dark|light` 带过去（那边认这个参数）
- `web/src/routes/+page.svelte`：首页仪表盘重做。上游原本写死的几块拆成可选板块，
  一律用上游自带的 Card / Badge / Progress / Chart 渲染：存储空间、当前监听、最近入库、
  下载任务状态、内存、CPU、网络速率、下载队列（待下载 / 已下载 / 不下）、视频总账（总数 / 有效 / 失效）。
  等宽栅格（md 两列、xl 三列），每块一样大——板块顺序可以随便调，固定大小才不会在行里留出空档。
  页头有「编辑板块」按钮，弹窗里开关 + 上下调顺序，保存后写服务端。
- `web/src/lib/api.ts`、`web/src/lib/types.ts`：加 `/dashboard-layout` 的读写方法与类型；
  `SysInfo` 加 `net_rx_speed` / `net_tx_speed`；`DashBoardResponse` 加队列与总账几个计数

后端（Rust）：

- `api/routes/dashboard/mod.rs`：仪表盘接口补队列与总账计数，直接复用视频列表那套
  `ValidationFilter` / `StatusFilter` 条件，口径跟「视频列表」页一致
- `api/routes/dashboard_layout/mod.rs`（新增）：`GET` / `PUT /api/dashboard-layout`，
  板块 id 走白名单（不认识返回 400），落 `$CONFIG_DIR/dashboard-layout.json`，读坏了退回默认
- `api/routes/ws/mod.rs`：sysinfo 采样里加网速——读 `/proc/net/dev` 的累计字节数，
  与上一次采样做差除以间隔（回环网卡不计），2 秒一个数
- `api/response.rs`：`SysInfo` 两个网速字段、`DashBoardResponse` 五个计数

Rust 侧只碰这四个文件加一个新文件，没动下载/同步/通知任何逻辑。

## 为什么不是真 fork

上游整段历史要进这个私有仓只能本地全量 clone 再推，而本机到 GitHub 的链路实测会超时；
GitHub 服务器侧导入的老接口（`PUT /repos/{owner}/{repo}/import`）已经下线（返回 404，
官方指到网页版 importer）。runner 上拉上游是秒级的，没必要过本机。

代价：不能 `git merge upstream/main`。上游更新时改 `UPSTREAM_PIN` 就行 —— 补丁打不上会
在 CI 里当场失败（`git apply -v` 那步），不会静默带病编译。

## 产物

- `bili-sync-rs-Linux-aarch64-musl.tar.gz` + `bili-sync-rs.sha256`（artifact 名 `bili-sync-rs-Linux-aarch64`）
- `web-build`（前端产物，便于离线核对菜单项/路由/板块有没有进去）

## 改前端时在本机先验一遍

本机有 node（`/var/apps/nodejs_v24/target/bin`，不在默认 PATH 里），没装 bun，用 npm 顶上：

```sh
export PATH=/var/apps/nodejs_v24/target/bin:$PATH
cp -r web /tmp/webcheck && cd /tmp/webcheck
npm install --legacy-peer-deps   # 上游锁的是 bun.lock，npm 会报 peer 冲突，忽略即可
npx vite build                   # 编得过说明 Svelte/TS 语法没问题
npx svelte-check                 # 类型与 a11y 警告
```

注意本机 npm 解析的是语义化版本，跟 CI 的 `bun install --frozen-lockfile` 不保证逐字一致，
只用来提前挡语法/类型错误。

## 换到机器上（要动容器，得先点头）

```sh
gh run download -n bili-sync-rs-Linux-aarch64
tar xzf bili-sync-rs-Linux-aarch64-musl.tar.gz
docker cp bili-sync-rs bili-sync-rs:/app/bili-sync-rs
docker restart bili-sync-rs
# 容器重启后 steward 要重新注入（12346）
```

回退：换前备一份原 `/app/bili-sync-rs`，拷回去再 restart。

## 上游更新时

1. 看 `web/src/lib/components/app-sidebar.svelte` 的 `navMain`、`routes/+page.svelte` 的结构、
   `api/routes/` 的模块清单有没有挪窝。
2. 改 `UPSTREAM_PIN` 与补丁，推上去，看这次构建绿不绿。

补丁面仍然很小（前端两个已有文件 + 一个新增页面 + 三个 lib/Rust 文件 + 一个新路由），
上游撞上的概率不高；真撞上了，改的是补丁，不是上游代码。
