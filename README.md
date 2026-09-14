# bili-sync-self

[bili-sync](https://github.com/amtoaer/bili-sync) 的补丁、构建脚本，以及配套的 steward 管家服务。

主程序和 steward 在同一个 Docker 容器中运行：

- `bili-sync-rs`：基于上游 `c900777`（v2.11.1）的构建，改动记录在 `bili-sync-self.patch`。
- `steward`：读取同一个 `data.sqlite` 和 `/downloads`，提供文件状态、下载队列、磁盘检查和磁盘闸门等功能。源码位于 `steward/`。

## 改动内容

### bili-sync

- 首页增加存储、监听、最近入库、下载任务、内存、CPU、网速、下载队列和视频总账等板块。
- 首页板块支持开关和排序，配置保存在服务端。
- 视频列表支持按 bvid 合并显示、立即下载、覆盖下载和状态编辑。
- 视频详情页调整为三栏布局，封面可以点开查看大图。
- 增加仪表盘布局接口、单条下载接口和网速采样。
- 下载候选按 bvid 去重，不涉及数据库表结构变更。

### steward

- 根据分页文件判断视频的本地状态。
- 扫描 `/downloads`，查看和删除本地视频文件夹。
- 管理下载队列，处理库中已找不到文件的视频记录。
- 为新添加的视频源写入默认过滤规则。
- 磁盘可用空间低于阈值时暂停下载源，空间恢复后可以按记录恢复。
- 记录文件状态变化，并按配置发送通知。

## 安装

Release 提供 aarch64 和 x86_64 两种架构的主程序、steward，以及包含安装脚本的压缩包。使用完整包时，选择对应架构的文件：

```sh
tar xzf bili-sync-self-v1.0.2-x86_64.tar.gz
cd bili-sync-self-v1.0.2-x86_64
./install.sh
```

aarch64 机器使用文件名中带 `aarch64` 的包。安装脚本会更新容器中的：

```text
/app/bili-sync-rs
/app/.config/bili-sync/steward/steward
/app/.config/bili-sync/steward/run.sh
/app/.config/bili-sync/steward/stop.sh
```

默认容器名为 `bili-sync-rs`，steward 使用端口 `12346`，磁盘阈值为 10%。可以通过环境变量调整：

```sh
CONTAINER=bili-sync-rs PORT=12346 FLOOR=10 ./install.sh
TOKEN='一串随机值' ./install.sh
PUSH_URL=http://10.0.0.9:8000/hook ./install.sh
WATCH=0 ./install.sh
```

也可以从仓库安装。脚本会根据 `uname -m` 从当前仓库的 release 下载对应架构的两个二进制：

```sh
git clone https://github.com/molakesizhanfangguangmang/bili-sync-self.git
cd bili-sync-self
./install.sh
```

安装脚本会先备份容器里的 `/app/bili-sync-rs`，然后更新主程序、重启容器并启动 steward。数据库和配置位于容器挂载目录，安装脚本不主动删除这些内容。回退时可以把备份的主程序拷回容器后重启。

要从局域网访问 steward 页面，Docker compose 需要映射端口：

```yaml
ports:
  - 12345:12345
  - 12346:12346
```

## Release 文件

以 `v1.0.2` 为例：

- `bili-sync-rs-aarch64`、`bili-sync-rs-x86_64`：bili-sync 主程序。
- `steward-v1.0.2-aarch64`、`steward-v1.0.2-x86_64`：steward 程序。
- `bili-sync-self-v1.0.2-aarch64.tar.gz`、`bili-sync-self-v1.0.2-x86_64.tar.gz`：对应架构的安装包。
- `SHA256SUMS`：附件校验和。

目前构建目标是 64 位 Linux `aarch64` 和 `x86_64`。bili-sync 的前端资源在编译时嵌入主程序；两个程序都使用静态链接，运行时不需要额外的动态库。

## 构建和测试

`.github/workflows/release.yml` 负责构建两个架构的 bili-sync 和 steward，并生成安装包。主程序的上游提交由 `UPSTREAM_PIN` 指定，`SELF_VERSION` 用于设置主程序显示的版本号。

在本地构建 steward：

```sh
./build.sh
```

运行 steward 测试：

```sh
python3 steward/test/run_tests.py
```

## 许可

主程序补丁基于上游 bili-sync，许可见 `LICENSE`。steward 使用的 SQLite amalgamation 为公有领域，相关说明见 `LICENSE.steward`。
