# bili-sync-self

不是上游的 fork，是一个十几 KB 的**编译脚手架**：把上游 pin 住的提交拉到 runner 上，打好
`bili-sync-self.patch`，编出 aarch64 / x86_64（都是 musl 静态）的 `bili-sync-rs`。

补丁与二进制衍生自 [`amtoaer/bili-sync`](https://github.com/amtoaer/bili-sync)（MIT），
与上游作者没有隶属关系，属非官方构建，无任何担保。许可证全文见 `LICENSE`。

## 里面有什么

| 文件 | 作用 |
| --- | --- |
| `.github/workflows/self-build.yaml` | 全部流程：拉上游 → 打补丁 → bun 编前端 → cross 编 aarch64/x86_64 → 打包上传 |
| `bili-sync-self.patch` | 对上游 `c900777` 的改动（见下表） |
| `SELF_VERSION` | 编进二进制的版本号（当前 `c900777-self-r9`），见下面「产物」 |
| `README.md` | 用法 |
| `LICENSE` | 上游 MIT 许可证全文 |
| `HARNESS.md` | 本文件（改动明细与复现方式） |

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
- `web/src/routes/videos/+page.svelte`、`web/src/lib/components/video-card.svelte`：视频列表行加两个入口 ——
  行内「立即下载」按钮（点到已经存在的会弹「重新下载并覆盖」确认框），「⋯」菜单里加「状态编辑」
  （复用详情页那套 Sheet 编辑器；列表行没有分页信息，打开前多取一次 `GET /api/videos/{id}`）。
  默认视图下同一个 bvid 折成一条，卡片上多印一行 bvid + 来源数
- `web/src/lib/api.ts`、`web/src/lib/types.ts`：加 `/dashboard-layout` 的读写方法与类型、
  `/videos/{id}/download` 的方法与类型；`SysInfo` 加 `net_rx_speed` / `net_tx_speed`；
  `DashBoardResponse` 加队列与总账几个计数；`VideoListItem`（`VideoInfo` + `source_count`）
- `web/src/lib/components/video-detail-panel.svelte`（新增）+ `web/src/routes/video/[id]/+page.svelte`：
  详情页的「视频信息」换成这个三段栅格组件 —— 左封面（`aspect-video` 放大，**点一下看大图**：纯 Svelte 状态 + 一层
  `fixed inset-0` 自绘遮罩，Esc/点背景关闭，没引新依赖）｜中标题/UP 主（头像 + 名字 + 「打开 B 站空间」按钮，头像不做点击目标）/发布与收藏时间/简介
  （默认 4 行、「展开/收起」）/时长（各分页相加）/分辨率（取首个有宽高的分页）/bvid/联合投稿（有才显示）｜右状态 Badge + `x/5` + 五段进度条
  （缩在 260px 栏内，不再横贯）并逐行列出五个任务名。原挂在该页 `VideoCard` 上的重置/清空重置确认框跟着挪进详情页。
  格式：简介走 `formatIntro`（`\r\n` 归一、`\xa0` 归一、**只解一轮** HTML 实体、空或 `-` 当没有、空行保留），联合投稿 `parseStaff`
  容错解析（后端给的就是数组，兜一层 JSON 字符串的情况），输出一律走 Svelte 文本插值（默认转义，不用 `{@html}`）；标题 name 不做实体解码。
- `web/src/lib/types.ts`：加 `VideoDetailInfo`（`VideoInfo` + 详情专属字段）与 `StaffMember`；`PageInfo` 补 `duration/width/height`；
  `VideoResponse.video` 改成 `VideoDetailInfo`（状态类接口仍回 `VideoInfo`，详情页把两边的字段合并后再存本地状态）。

后端（Rust）：

- `api/routes/dashboard/mod.rs`：仪表盘接口补队列与总账计数，直接复用视频列表那套
  `ValidationFilter` / `StatusFilter` 条件，口径跟「视频列表」页一致
- `api/routes/dashboard_layout/mod.rs`（新增）：`GET` / `PUT /api/dashboard-layout`，
  板块 id 走白名单（不认识返回 400），落 `$CONFIG_DIR/dashboard-layout.json`，读坏了退回默认
- `api/routes/ws/mod.rs`：sysinfo 采样里加网速——读 `/proc/net/dev` 的累计字节数，
  与上一次采样做差除以间隔（回环网卡不计），2 秒一个数
- `api/routes/videos/mod.rs`：`GET /api/videos` 在默认视图（没按来源筛选）按 bvid 折叠并附 `source_count`，
  代表行优先取「下载完成的那条」，没有完成份时取最早入库（id 最小）那条；新增
  `POST /api/videos/{id}/download`（`overwrite` 为真时先清空重置「有文件的那条」再下这一条）
- `utils/model.rs`：`find_duplicated_video_ids()` 算出本轮要跳过的重复记录。判据只看 bvid：
  已有完成份则其余都不下；否则只留最早入库那条「要下」的记录（有效、标记要下、来源仍启用）
- `task/video_downloader.rs`：`download_video_once()` 把单条视频塞进下载队列 —— 不插队、不绕并发上限，
  等正在跑的那轮结束后按顺序下
- `workflow.rs`：`download_single_video()` 手动单条下载（缺分页时先补一次详情）；
  `process_video_source()` / `download_unprocessed_videos()` 多带一个 `duplicated_video_ids` 过滤候选
- `api/request.rs`、`api/response.rs`：`DownloadVideoRequest`、`VideoListItem`、`DownloadVideoResponse`
- `api/response.rs`：新增 `VideoDetailInfo`（`VideoInfo` flatten + `upper_id / upper_face / intro / pubtime / ctime / favtime / staff / path`），
  `get_video` 改用它；`PageInfo` 补 `duration / width / height`。列表接口 `get_videos` 仍只回 `VideoInfo`。
- `api/routes/videos/mod.rs`：`to_video_detail_info()`（从完整 `video::Model` 手工构造，跟 `to_video_info()` 并列），
  `get_video` 取完整 Model 而不是 `into_partial_model::<VideoInfo>()`。

构建脚本（只为对外构建，跑起来跟上游没区别）：

- `crates/bili_sync/build.rs` + `src/config/args.rs`：上游这里靠 `built` 抓工作区 `.git` 写进
  `built.rs`，而 `version()` 优先用 `GIT_VERSION` —— 在这个脚手架仓里编时，工作区的 `.git` 是脚手架
  自己的，抓出来的是它的 commit，对外没意义。补丁改成：build.rs 另写一个 `self_version.rs`
  （内容取自仓根的 `SELF_VERSION`），`version()` 先看它，没有再走上游那套。这样不用去猜
  built crate 生成的常量长什么样（第一版就是在猜，CI 上 assert 直接挂了）。
  **没有 `SELF_VERSION` 文件就完全保持上游行为**（比如把补丁直接打到上游树上编）。

去重只作用在**下载候选**与**列表展示**两处：不改 `data.sqlite` 表结构、不加列、无迁移，
也不动管道/通知逻辑。折叠只在默认视图生效，切到单源筛选仍逐条显示。

## 为什么不是真 fork

上游整段历史要进这个仓只能本地全量 clone 再推，而本机到 GitHub 的链路实测会超时；
GitHub 服务器侧导入的老接口（`PUT /repos/{owner}/{repo}/import`）已经下线（返回 404，
官方指到网页版 importer）。runner 上拉上游是秒级的，没必要过本机。

代价：不能 `git merge upstream/main`。上游更新时改 `UPSTREAM_PIN` 就行 —— 补丁打不上会
在 CI 里当场失败（`git apply -v` 那步），不会静默带病编译。

## 产物

矩阵两格，各出一份：

- `bili-sync-rs-Linux-aarch64-musl.tar.gz` + `bili-sync-rs-aarch64.sha256`（artifact 名 `bili-sync-rs-Linux-aarch64`）
- `bili-sync-rs-Linux-x86_64-musl.tar.gz` + `bili-sync-rs-x86_64.sha256`（artifact 名 `bili-sync-rs-Linux-x86_64`）
- `web-build`（前端产物，只跟着 aarch64 那格传，便于离线核对菜单项/路由/板块有没有进去）

Release 里存的是解出来的**裸二进制**（`bili-sync-rs-aarch64-c900777-rN` / `bili-sync-rs-x86_64-c900777-rN`），
外加 `SHA256SUMS` 与 `UPSTREAM-License`。

版本号：仓里 `SELF_VERSION` 的内容（现在 `c900777-self-r9`）会被编进二进制，启动日志与 `--version`
显示的就是它。改轮次时同步改这个文件，否则和 release 里的二进制名对不上。

x86_64 那格在 CI 里多跑一步 `--version`，输出对不上就直接失败 —— 免得发出去的二进制连启动都不过。

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

## 换到机器上（要动容器）

那台机器现在走**自建镜像**：`官方 v2.11.1 + 补丁二进制 + /app/start.sh（启动时拉起 steward）`，
打 `bili-sync-self:rN`，再把 compose 的 `image:` 指过去 `docker compose up -d`。配方见机器上
`/vol1/@appdata/bili-sync/SELF-IMAGE.md`（工作区里也有一份 `build/bili-sync-self/`）。

也不用镜像时的老办法：

```sh
gh run download -n bili-sync-rs-Linux-aarch64
tar xzf bili-sync-rs-Linux-aarch64-musl.tar.gz
docker cp bili-sync-rs bili-sync-rs:/app/bili-sync-rs   # 源文件先 chmod 755，不然容器里不可执行
docker restart bili-sync-rs
# 容器重启后 steward 要重新注入（12346）
```

回退：换前备一份原 `/app/bili-sync-rs`（镜像路线就把 compose 的 tag 改回去），拷回去再 restart。

## 上游更新时

1. 看 `web/src/lib/components/app-sidebar.svelte` 的 `navMain`、`routes/+page.svelte` 的结构、
   `api/routes/` 的模块清单有没有挪窝。
2. 改 `UPSTREAM_PIN` 与补丁，推上去，看这次构建绿不绿。

补丁面仍然很小（前端几个已有文件 + 两个新增页面/组件 + 几个 lib/Rust 文件 + 一个新路由），
上游撞上的概率不高；真撞上了，改的是补丁，不是上游代码。
