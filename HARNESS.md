# bili-sync-self

不是上游的 fork，是一个十几 KB 的**编译脚手架**：把上游 pin 住的提交拉到 runner 上，打好
`bili-sync-self.patch`，编出 aarch64（musl 静态）的 `bili-sync-rs`。

私有仓。给自己用的，不发布。

## 里面有什么

| 文件 | 作用 |
| --- | --- |
| `.github/workflows/self-build.yaml` | 全部流程：拉上游 → 打补丁 → bun 编前端 → cross 编 aarch64 → 打包上传 |
| `bili-sync-self.patch` | 对上游 `web/` 的两处改动（见下）。只碰这两个文件 |
| `HARNESS.md` | 本文件 |

补丁内容：

- `web/src/lib/components/app-sidebar.svelte`：「内容管理」组加一项「存储」→ `/storage`
- `web/src/routes/storage/+page.svelte`：新增，iframe 指向 `location.hostname:12346`（steward）

## 为什么不是真 fork

上游整段历史要进这个私有仓只能本地全量 clone 再推，而本机到 GitHub 的链路实测会超时；
GitHub 服务器侧导入的老接口（`PUT /repos/{owner}/{repo}/import`）已经下线（返回 404，
官方指到网页版 importer）。runner 上拉上游是秒级的，没必要过本机。

代价：不能 `git merge upstream/main`。上游更新时改 `UPSTREAM_PIN` 就行 —— 补丁打不上会
在 CI 里当场失败（`git apply -v` 那步），不会静默带病编译。

## 产物

- `bili-sync-rs-Linux-aarch64-musl.tar.gz` + `bili-sync-rs.sha256`（artifact 名 `bili-sync-rs-Linux-aarch64`）
- `web-build`（前端产物，便于离线核对菜单项/路由有没有进去）

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

1. 看 `web/src/lib/components/app-sidebar.svelte` 的 `navMain` 有没有挪窝、`routes/` 有没有新约定。
2. 改 `UPSTREAM_PIN` 与补丁，推上去，看这次构建绿不绿。

前端那几行的补丁面很小，上游撞上的概率不高；真撞上了，改的是补丁，不是上游代码。
