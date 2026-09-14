# bili-sync-self

给 [bili-sync](https://github.com/amtoaer/bili-sync) 打的一组补丁，外加把补丁编成二进制的那套脚手架。

基线是上游 `c900777`（v2.11.1）。不是 fork，跟上游作者没关系。

## 改了什么

界面动得多，后端跟着配了几个接口。

首页仪表盘重做了一遍：存储、监听、最近入库、下载任务、内存、CPU、网速、下载队列、视频总账，等宽栅格摆开，页头「编辑板块」能开关和排序，配置存在服务端。

视频列表加了行内「立即下载」，已经有的会问要不要覆盖；「⋯」里多了「状态编辑」。默认视图把同一个 bvid 合成一条，切到单源筛选还是逐条。

详情页的「视频信息」换成三段：左边封面（点开看大图），中间标题、UP 主、简介、时长、分辨率、bvid，右边五个任务的状态和进度。

侧栏「内容管理」多一项「存储」，指向 `/storage`。那是另一个服务的 iframe，跟 bili-sync 本身没关系，用不上就删掉 `web/src/routes/storage/` 加侧栏那一项。

后端加了 `/api/dashboard-layout` 的读写、`POST /api/videos/{id}/download`、网速采样，下载候选按 bvid 去重。没动数据库表结构，不需要迁移。

完整 diff 在 `bili-sync-self.patch`，逐文件说明在 `HARNESS.md`。

## 怎么用

Releases 里有两份 musl 静态二进制，aarch64 和 x86_64 各一。

上游官方镜像里就一个 `/app/bili-sync-rs` 是要换的东西：

```sh
mv bili-sync-rs-x86_64-c900777-r9 bili-sync-rs
chmod 755 bili-sync-rs
docker cp bili-sync-rs bili-sync-rs:/app/bili-sync-rs
docker restart bili-sync-rs
```

动手前把容器里原来那份拷出来放着，不对就拷回去重启。数据和配置都在挂载卷里，不受影响。

`--version` 打的是 `c900777-self-r9`，跟原版分得开。

## 自己编

`.github/workflows/self-build.yaml`：拉上游 pin、`git apply` 补丁、bun 编前端、cross 编两个架构的 musl 静态二进制。fork 出去开 Actions 就行，产物在 run 的 artifacts 里。

## 许可

从 [bili-sync](https://github.com/amtoaer/bili-sync) 衍生，MIT License，`Copyright (c) 2024 ᴀᴍᴛᴏᴀᴇʀ`，全文见 `LICENSE`。
