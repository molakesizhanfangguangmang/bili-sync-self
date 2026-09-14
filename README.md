# bili-sync-self

[`amtoaer/bili-sync`](https://github.com/amtoaer/bili-sync) 的一组**非官方**补丁，外加一个能把它复现出来的编译脚手架。

不是上游的 fork，与上游作者没有隶属关系。补丁本体是 [`bili-sync-self.patch`](bili-sync-self.patch)，
基线是上游 `c900777e78796653fdfb00421879b6a802d1eedd`（`UPSTREAM_PIN`）。

## 补丁改了什么

- **首页仪表盘重做**：存储空间、当前监听、最近入库、下载任务状态、内存、CPU、网络速率、下载队列、视频总账。
  等宽栅格，页头「编辑板块」可开关与排序，配置落服务端。
- **视频列表**：行内「立即下载」（已存在会问是否覆盖）、「⋯」菜单里的「状态编辑」；
  默认视图下同一个 bvid 折成一条并标出来源数，切到单源筛选仍是逐条。
- **视频详情页**：「视频信息」换成三段栅格 —— 封面（点击看大图）、标题/UP 主/简介/时长/分辨率/bvid、右侧五个任务的状态与进度。
- **侧栏**：「内容管理」组加一项「存储」→ `/storage`。这个页面是给同机另一个服务（管家，12346）的 iframe，跟上游功能无关，用不上可以不管。
- **后端**：`GET/PUT /api/dashboard-layout`、`POST /api/videos/{id}/download`、sysinfo 加网速采样、
  下载候选按 bvid 去重（只看 bvid，不动 `data.sqlite` 表结构、无迁移）。

逐文件明细见 [`HARNESS.md`](HARNESS.md)。

## 怎么用

[Releases](../../releases) 里是**裸静态二进制**（musl，无动态依赖）：

| 架构 | 附件名（以最新 release 为准） |
| --- | --- |
| aarch64（ARM64） | `bili-sync-rs-aarch64-c900777-r9` |
| x86_64（AMD64） | `bili-sync-rs-x86_64-c900777-r9` |

以官方 docker 部署为例，替换容器里那份二进制即可：

```sh
# 下对应架构那份，核对 SHA256SUMS
mv bili-sync-rs-x86_64-c900777-r9 bili-sync-rs
chmod 755 bili-sync-rs
docker cp bili-sync-rs <容器名>:/app/bili-sync-rs
docker restart <容器名>
```

替换前先把容器里原来那份 `/app/bili-sync-rs` 备份出来；出问题拷回去、重启，就回到原样。
数据目录、配置、数据库都在挂载卷里，替换二进制不会动它们。

二进制对应的上游那次 pin 是 `c900777`。跟上游其它版本的数据库/前端资源混用没有保证 ——
真要升上游，看 [`HARNESS.md`](HARNESS.md) 的「上游更新时」那节重新打一次补丁。

## 自己编

[`.github/workflows/self-build.yaml`](.github/workflows/self-build.yaml) 就是全部流程：
拉上游 pin → `git apply` 补丁 → bun 编前端 → cross 编 aarch64 / x86_64 静态二进制 → 打包上传。
fork 这个仓、开 Actions（或手动 `workflow_dispatch`）即可，产物在 run 的 artifacts 里。

## 许可证

补丁与二进制衍生自 [`amtoaer/bili-sync`](https://github.com/amtoaer/bili-sync)，
MIT License，Copyright (c) 2024 ᴀᴍᴛᴏᴀᴇʀ，全文见 [`LICENSE`](LICENSE)。
本仓同样以 MIT 分发，**不提供任何担保**。
