# bili-sync-self

这是一个给 [bili-sync](https://github.com/amtoaer/bili-sync) 用的增强版构建仓。

它由两部分组成：

- `bili-sync-rs`：bili-sync 主程序，基于上游 `c900777`（v2.11.1）打补丁编译。
- `steward`：同一个 Docker 容器里的管家服务，读同一个 `data.sqlite`，管理磁盘文件、下载队列、状态核对和磁盘闸门。

两个进程配套运行，但代码分开：主程序的改动在 `bili-sync-self.patch`，管家源码在 `src/`。对外只有这个仓库和这一套版本。

## 这版有什么

主程序补丁包括：

- 首页原生仪表盘：存储、监听、最近入库、下载任务、内存、CPU、网速、下载队列、视频总账。
- 仪表盘板块可以开关和排序，配置存在服务端。
- 视频列表按 bvid 折叠，支持立即下载、覆盖下载和状态编辑。
- 视频详情页重做，封面可点开看大图。
- `/api/dashboard-layout`、单条下载接口和网速采样。
- 下载候选按 bvid 去重，不改数据库表结构。

`steward` 提供独立页面和接口：

- 按分页文件判断视频是存在、部分存在、不存在、未下载还是无分页。
- 扫描 `/downloads`，查看和删除本地文件夹。
- 管理下载队列，清理库里已经丢失文件的视频状态。
- 新源默认只下载新视频。
- 磁盘空间低于阈值时停用下载源，腾出空间后可按快照恢复。
- 核对文件落盘状态并推送完成或丢失通知。

## 安装

release 按架构提供裸二进制和完整包。完整包里有两个二进制以及安装脚本；从完整包安装最省事：

```sh
tar xzf bili-sync-self-v1.0.2-x86_64.tar.gz
cd bili-sync-self-v1.0.2-x86_64
./install.sh
```

aarch64 机器把文件名里的 `x86_64` 换成 `aarch64`。脚本会同时更新：

- `/app/bili-sync-rs`
- `/app/.config/bili-sync/steward/steward`
- steward 的 `run.sh`、`stop.sh` 和配置文件

它们运行在同一个 `bili-sync-rs` Docker 容器里。数据库和配置位于 bind mount，不会因为替换二进制丢失。

也可以只拉仓库，脚本会按 `uname -m` 从同一个仓库的 release 取对应架构的两个二进制：

```sh
git clone https://github.com/molakesizhanfangguangmang/bili-sync-self.git
cd bili-sync-self
./install.sh
```

默认容器名是 `bili-sync-rs`，steward 端口是 `12346`，磁盘阈值是 10%。可以覆盖：

```sh
CONTAINER=bili-sync-rs PORT=12346 FLOOR=10 ./install.sh
TOKEN='一串随机值' ./install.sh
PUSH_URL=http://10.0.0.9:8000/hook ./install.sh
WATCH=0 ./install.sh
```

要让局域网访问 steward 页面，Docker compose 还需要映射端口：

```yaml
ports:
  - 12345:12345
  - 12346:12346
```

安装前建议先备份容器中的 `/app/bili-sync-rs`。回退时拷回原文件并重启容器即可。

## release 附件

每个架构有四种文件：

- `bili-sync-rs-aarch64` / `bili-sync-rs-x86_64`：bili-sync 主程序裸二进制。
- `steward-v1.0.2-aarch64` / `steward-v1.0.2-x86_64`：steward 裸二进制。
- `bili-sync-self-v1.0.2-aarch64.tar.gz` / `bili-sync-self-v1.0.2-x86_64.tar.gz`：完整安装包。
- `SHA256SUMS`：所有附件的校验和。

只支持 64 位 Linux `aarch64` 和 `x86_64`。主程序的前端资源已经嵌进二进制，不需要额外下载 web 文件；两份主程序都是 musl 构建，没有动态链接器依赖。`steward` 也是静态单文件。

## 自己编译

GitHub Actions 的 `release.yml` 在打 `v*` tag 时编译：

- bili-sync 主程序：aarch64、x86_64 两个 musl 目标。
- steward：aarch64、x86_64 两个静态目标。
- 完整包和 `SHA256SUMS`。

主程序基线由 `UPSTREAM_PIN` 固定，改动在 `bili-sync-self.patch`。`SELF_VERSION` 控制主程序显示的版本号。

本地编 steward：

```sh
./build.sh
```

本地测试：

```sh
python3 test/run_tests.py
```

## 许可

主程序补丁基于上游 bili-sync，遵循上游 MIT License，全文见 `LICENSE`。steward 的 SQLite amalgamation 为公有领域，具体说明见 `LICENSE.steward`。
