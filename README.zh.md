# fly.board

![fly.board logo](img/logo.png)

> 为数不多的简单博客引擎之一，在连接规模扩大时内存几乎保持平稳：空闲时即使仅使用单个 worker，RSS 也仅为 **~108 MB**（4 个 worker 时为 **~102 MB**），在 C10k、C100k 乃至 C1m 下仍保持在 **~110–146 MB** 左右。
> 基于 C 语言 CWIST Web 框架的轻量级论坛兼博客引擎，支持 HTTPS/3、Argon2id、PQC 签名与 NATS 消息。

## 特性

- **内存高效且连接可扩展** – 栈+堆 C 实现。空闲时 **~102–108 MB RSS**（1–4 个 worker）；从 C10k 到 C1m 并发连接，RSS 始终保持在 **~110–146 MB** 左右。
- **现代传输层** – 默认 TLS 1.3 + HTTP/3（QUIC）。可选 ECH（Encrypted Client Hello）。
- **安全认证** – 客户端 SHA-512 预哈希 + 服务端 **Argon2id**（OpenSSL 3 KDF）。JWT 会话 Cookie。
- **论坛 / 博客混合** – 基于 Slug 的 Markdown 文章 + 多板块 + 嵌套评论。
- **实时预览** – Markdown 编辑器即时生成服务端预览。
- **PQC 签名** – 为文章附加/验证后量子密码学（PQC）签名。
- **文件存储** – ≤1 MB 存于 SQLite，更大文件存放于卷。图片/视频/音频自动嵌入。
- **NATS 集成** – 通过 `NATS_URL` 环境变量接入分布式消息网关。
- **深色模式** – 基于 Cookie 的主题切换与动态 CSS 变量。

## 构建

```sh
make
./keygen.sh
```

依赖：
- [CWIST](https://github.com/religiya-serdtsa/cwist) — TLS 1.3 / HTTP/3（QUIC）由 CWIST 内置的 BoringSSL 处理，无需额外配置。
- OpenSSL 3.x（Argon2id KDF）
- ngtcp2 / nghttp3（HTTP/3）
- cJSON、SQLite3

`Makefile` 会克隆并构建 `third_party/md4c` 为静态库。

## 运行

```sh
./fly_board
```

默认端口遵循 `blog.settings` 中的 `port` 值（默认 9443）。

```text
https://localhost:9443
```

HTTP/3 在同一端口的 UDP 上监听。

### 启用 ECH（可选）

```sh
BLOG_ECH_KEY=ech/server.ech ./fly_board
# 或
BLOG_ECH_DIR=ech ./fly_board
```

如果 OpenSSL 构建不支持 ECH，将记录警告并继续使用常规 HTTPS/3。

### NATS 集成（可选）

```sh
NATS_URL=nats://localhost:4222 ./fly_board
```

## 主要功能

| 功能 | 路径 | 说明 |
|------|------|------|
| 首页 | `/` | 最新文章列表 |
| 板块 | `/boards` | 多板块管理（admin-only 支持） |
| 文章 | `/post/:slug` | md4c Markdown 渲染 + 评论 + 附件 |
| 登录/注册 | `/login`、 `/register` | Argon2id + JWT Cookie |
| 个人资料 | `/profile` | 昵称、简介、头像、加入日期 |
| 账户设置 | `/account/settings` | 编辑个人资料 |
| 修改密码 | `/account/password` | 验证当前密码后用 Argon2id 重新哈希 |
| 管理员 | `/admin/users` | 更改用户角色、删除用户 |
| 文件存储 | `/files` | 上传/下载/删除 |

## 配置

配置来自三个文件（首次运行时自动以默认值创建），以及用于运维开关的环境变量。

### `admin.settings`

两行原始内容：第 1 行为管理员用户名，第 2 行为管理员密码。

### `blog.settings`

简单的 `key=value` 行。未知键会被忽略；无效值回退到默认值。

| 键 | 默认值 | 取值 / 作用范围 |
|-----|---------|----------------|
| `title` | `CWIST Docker Blog` | 顶栏显示的网站标题 |
| `subtitle` | `Explore boards and read stories.` | 主视觉区副标题 |
| `brand_footer` | `Built with CWIST C Framework` | 页脚文本 |
| `root_url` | `https://localhost:8888/` | 站点规范 URL（以 `/` 结尾）。用于 RSS 链接、验证邮件和证书续期——生产环境中请设置为公网 URL |
| `port` | `8443` | TCP/UDP 监听端口（HTTP/3 在同一端口上走 UDP） |
| `accent` | `#3b82f6` | 强调色（十六进制） |
| `use_tls` | `true` | `true`/`false` — 开启/关闭 HTTPS（先运行 `./keygen.sh`） |
| `use_http2` | `true` | 基于 TLS 的 HTTP/2 |
| `use_http3` | `true` | 基于 UDP 的 HTTP/3（QUIC） |
| `use_tasfa` | `true` | TASFA 媒体管线（通过 ffmpeg 生成视频缩略图/预览） |
| `use_rss` | `false` | 暴露 `/rss.xml` |
| `roundness` | `0.0` | UI 圆角程度，`0.0`–`1.0` |
| `max_upload_size` | `1G` | 单文件上传上限。支持后缀 `K/M/G/T`（如 `500M`） |
| `max_total_parallel_uploads` | `8` | 全局并发上传数（1–512） |
| `max_upload_parallel_chunks` | `32` | 每次上传的并行分块数（1–64） |
| `max_concurrent_downloads` | `128` | 并发下载数（1–512） |
| `vote_only` | *(空 = `all`)* | 谁可以对文章投票：`all`（任何人，含匿名）、`authorized`（仅登录用户）、`admin`（仅管理员） |
| `use_special_modes` | *(空)* | 替换浅色/深色主题：`lightTheme,darkTheme`（或单个主题）。可用主题：`light`、`dark`、`ocean`、`forest`、`sepia`。例如 `ocean,forest` |
| `home_img`、`boards_img`、`files_img` | *(空)* | 各页面的主视觉/背景图；文件位于 `public/img/` 内 |
| `*_dark`（`home_img_dark`、`boards_img_dark`、`files_img_dark`） | *(空)* | 上述图片的深色模式变体 |
| `blog_logo`、`blog_logo_dark` | *(空)* | `public/img/` 中的 Logo 图片 |
| `invert_logo` | `false` | 为没有图片的模式自动反色 Logo |
| `favicon` | *(空)* | `public/img/` 中的 Favicon 文件 |
| `bg_full_light`、`bg_full_dark` | *(空)* | 整页背景图 |
| `bg_invert_color` | *(空)* | 逗号分隔的目标列表，其缺失的模式变体将通过反色另一模式自动生成：`home`、`boards`、`files`、`toplevel`、`logo` |
| `bg_invert_algo` | `luminv` | 反色算法：`luminv` 或 `oklch` |

### `fonts.settings`

字体排版覆盖项：`font_body`、`font_heading`、`font_ui`、`font_code`、`font_blockquote`、`font_display`、`font_import_url`、`font_face_family`、`font_face_src`，以及按元素的 `letter_spacing_*` 和 `font_weight_*` 值。首次运行时会写出默认值，打开生成的文件即可查看所有键。

### 环境变量

**核心**

| 变量 | 默认值 | 说明 |
|----------|---------|-------------|
| `BLOG_ROOT` | *(未设置)* | 项目根目录；当二进制文件在项目根之外启动时使用。否则会自动检测包含 `public/` 的目录 |
| `DEBUG` | *(关闭)* | `1`/`true`/`yes` 启用 DEBUG/INFO 日志；否则仅输出警告/错误 |
| `NATS_URL` | *(未设置)* | 例如 `nats://localhost:4222` — 启用 NATS 消息网关 |
| `BLOG_ECH_KEY` / `BLOG_ECH_DIR` | *(未设置)* | ECH（Encrypted Client Hello）密钥文件 / 密钥目录 |
| `CWIST_C1M_MODE` | `1` | 事件驱动的 C1M reactor。设为 `0` 可强制使用传统的线程池路径 |

**性能 / 缓存**

| 变量 | 默认值 | 说明 |
|----------|---------|-------------|
| `FLYBOARD_CACHE_MAX_MB` | `64` | 页面缓存大小（MB，1–1024） |
| `FLYBOARD_ADVERTISE_H3` | `true` | 发送通告 HTTP/3 的 `Alt-Svc` 响应头 |
| `FLYBOARD_ALT_SVC_MAX_AGE` | `300` | `Alt-Svc` 的 `ma` 值（秒，0–86400） |
| `FLYBOARD_INLINE_IMAGES` | *(关闭)* | 将图片以 base64 data URI 内联到 HTML 中 |
| `FLYBOARD_INLINE_ALL_ASSETS` | *(关闭)* | 同时内联脚本/样式 |
| `FLYBOARD_INLINE_BG_IMAGES` | *(关闭)* | 同时内联背景图（即使开启 `ALL_ASSETS` 也需显式启用） |
| `FLYBOARD_INLINE_MAX_IMAGE_SIZE` | `49152` | 每张内联图片的最大字节数 |
| `FLYBOARD_INLINE_MAX_ASSET_SIZE` | `65536` | 每个内联脚本/样式块的最大字节数 |
| `FLY_MEDIA_MAX_CONCURRENT` | `2` | 媒体预览的并发 ffmpeg 转换数 |
| `FLYBOARD_MEDIA_BACKFILL_ON_START` | *(关闭)* | 启动时重新生成所有旧版媒体预览（仅限维护时运行） |

**TLS 证书自动续期**（使用本地 ACME 客户端；会检测到 `keygen.sh` 生成的临时自签名证书且绝不改动）

| 变量 | 默认值 | 说明 |
|----------|---------|-------------|
| `FLY_CERT_RENEWAL` | *(关闭)* | `true` 启用每日到期看门狗。当证书剩余天数 ≤ `FLY_CERT_DAYS` 时续期，并热加载而无需重启 |
| `FLY_CERT_DAYS` | `30` | 续期阈值（天） |
| `FLY_CERT_EMAIL` | `admin@<host>` | ACME 账户邮箱 |
| `FLY_CERT_LEGO_BIN` | `lego` | lego 二进制名称/路径（可指向用于 DNS 挑战等的包装脚本） |

看门狗从 `root_url` 推导域名，并以 HTTP-01 挑战运行 lego，因此 80 端口必须可达。状态保存在 `.lego/` 下；续期后的证书会覆盖安装到 `server.crt`/`server.key`。

**邮箱验证注册**（默认关闭 = 开放注册）

| 变量 | 默认值 | 说明 |
|----------|---------|-------------|
| `FLY_EMAIL_CERT` | *(关闭)* | `true` 要求新注册用户验证邮箱后才能登录。系统会通过 SMTP 发送 24 小时有效的令牌链接 |
| `FLY_SMTP_HOST` | *(启用时必填)* | SMTP 中继主机 |
| `FLY_SMTP_PORT` | `25`（隐式 TLS 时为 `465`） | SMTP 端口 |
| `FLY_SMTP_TLS` | *(关闭)* | `starttls` 或 `implicit` |
| `FLY_SMTP_USER` / `FLY_SMTP_PASS` | *(未设置)* | AUTH LOGIN 凭据（可选） |
| `FLY_SMTP_FROM` | `FLY_SMTP_USER` | 信封/邮件头发件人 |

示例 — 生产环境启用邮箱验证注册与证书自动续期：

```sh
FLY_CERT_RENEWAL=true FLY_CERT_EMAIL=admin@example.com \
FLY_EMAIL_CERT=true FLY_SMTP_HOST=smtp.example.com FLY_SMTP_PORT=587 \
FLY_SMTP_TLS=starttls FLY_SMTP_USER=noreply@example.com FLY_SMTP_PASS=secret \
./fly_board
```

## 数据库

SQLite3（`data/blog.db`）。模式在应用启动时自动迁移。

```
users       – 账户、Argon2id 哈希、角色、个人资料
boards      – 板块名称/slug/描述/admin_only
posts       – Markdown 正文、PQC 签名、摘要
files       – 附件路径/大小/MIME
comments    – 嵌套评论（target_type, parent_id）
board_permissions – 私有板块访问权限
```

## 架构

```
CWIST (HTTP/3, TLS 1.3)
  ├── src/auth/     – Argon2id、JWT、会话
  ├── src/db/       – SQLite3 CRUD
  ├── src/handlers/ – 路由/业务逻辑
  ├── src/render/   – cwist_html_element SSR + md4c
  ├── src/crypto/   – PQC 签名/验证
  └── src/nats/     – 消息 Pub/Sub
```

## 许可证

MIT License

---

## 可扩展性基准测试

### 该基准测试衡量什么

这些测试使用 `h2load` **并带有 `-r`（速率限制）选项**。它们有意**不是**最大吞吐量测试，而是衡量服务器在受控的每个进程请求速率下，是否能够**维持海量并发 HTTP/2 连接**。

由于负载是速率受限的：

- 报告的 **RPS 反映的是配置的请求速率**，而不是服务器的绝对吞吐上限。
- 关键指标是 **常驻内存集（RSS）稳定性**，即连接从 10,000 增长到 1,000,000 时的内存占用变化。

Worker 数量会随负载扩展，以保持每次测试都贴近现实：C10k 使用 **4 个 worker**，C100k 使用 **12 个 worker**，C1m 使用 **12 个 worker**。这也解释了三次运行中 CPU 使用率的差异。

### 主机环境

| 项目 | 值 |
|------|-------|
| OS | Linux 7.1.0-mountain-rc6+ |
| 架构 | x86_64 |
| CPU | 12 logical cores |
| 内存 | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| OpenSSL | 3.5.6 |
| 基准工具 | h2load nghttp2/1.64.0 |
| CWIST | 来自同级 cwist 检出的 `libcwist.a`（2026-08-29，arena bump 分配器、共享 req/res arena、256KB worker 栈、HTTP/3 强化、sharded TLS handshake shepherd） |

### 系统调优

| 参数 | 值 |
|-----------|-------|
| ulimit -n | 1,050,000 |
| fs.file-max | 2,097,152 |
| fs.nr_open | 1,050,000 |
| net.core.somaxconn | 1,050,000 |
| net.ipv4.tcp_max_syn_backlog | 1,050,000 |
| net.ipv4.ip_local_port_range | 1024 65535 |
| vm.max_map_count | 1,048,576 |
| kernel.pid_max | 4,194,304 |
| CPU governor | ecodemand |

### 内存使用量

| 状态 | RSS | 较上次变化 | 备注 |
|-------|-----|-----------------|-------|
| 空闲（1 个 worker） | **~108 MB** (110,196 KB) | — | 1 个 worker，无连接 |
| 空闲（4 个 worker） | **~102 MB** (104,940 KB) | — | 4 个 worker，无连接 |
| C10k | **~110 MB** (112,436 KB) | +7,496 KB（对比 4 worker 空闲） | 10,000 并发连接 |
| C100k | **~146 MB** (148,848 KB) | +36,412 KB | 100,000 并发连接 |
| C1m churn | **~146 MB** (149,816 KB) | +968 KB | 100k 保持的 TLS 连接，1M 请求 churn |

从 **C100k 到 C1m churn 运行**，总 RSS 变化为 **+968 KB** —— 基本属于测量噪声。这是本次基准测试最重要的结果。

RSS 值为服务器进程 `/usr/bin/time -v` 报告的 **Maximum resident set size (kbytes)**。

### 内存成本

| 阶段 | Δ RSS | Δ 连接数 | 每个新增连接的大约成本 |
|---|---|---|---|
| Idle → C10k | +7,496 KB | 10,000 | ~0.75 KB / 连接 |
| C10k → C1m churn | +37,380 KB | — | 每个新增保持连接约 ~0.4 KB；C100k → C1m 为 +968 KB（噪声） |

从 Idle 到 C10k 的初始跃升预先支付了 TLS 状态、连接缓冲区和 worker 开销。从 C10k 到 C100k，每个新增保持连接的成本约为 ~0.4 KB，而 C100k 到 C1m 的 RSS 变化（+968 KB）处于测量噪声范围内 —— 每个连接的内存成本实际上是平稳的。

### C10k 并发连接测试

使用 `h2load` 维持 10,000 个并发连接测得。

| 项目 | 值 |
|------|-------|
| Workers | 4 |
| 并发连接数 | 10,000 |
| 持续时间 | 12.05 s |
| 最大 RSS | **~110 MB** (112,436 KB) |
| CPU 使用率 | ~365% |
| 用户时间 | 41.05 s |
| 系统时间 | 3.04 s |
| 主缺页中断 | 2 |
| 次缺页中断 | 16,948 |
| 主动上下文切换 | 58,050 |
| 被动上下文切换 | 14,828 |
| 文件系统输出 | 256 |
| 总请求数 | 20000 |
| 总成功数 | 20000 |
| 总失败数 | 0 |
| 近似总 RPS | **2285.22** |
| 成功率 | **100.00%** |
| 退出状态 | **0** |

### C100k 并发连接测试

使用 `h2load` 维持 100,000 个并发连接测得。

| 项目 | 值 |
|------|-------|
| Workers | 12 |
| 并发连接数 | 100,000 |
| 持续时间 | 1:23.49 |
| 最大 RSS | **~146 MB** (148,848 KB) |
| CPU 使用率 | ~815% |
| 用户时间 | 653.83 s |
| 系统时间 | 26.78 s |
| 主缺页中断 | 0 |
| 次缺页中断 | 76,332 |
| 主动上下文切换 | 446,557 |
| 被动上下文切换 | 617,777 |
| 文件系统输出 | 336 |
| 总请求数 | 200000 |
| 总成功数 | 200000 |
| 总失败数 | 0 |
| 近似总 RPS | **2785.16** |
| 成功率 | **100.00%** |
| 退出状态 | **0** |

### C1m Churn 测试（2026-08-23 重新设计，2026-08-24 修复）

旧的“1,000,000 个并发 TLS 连接”目标已被废弃：HTTPS 路径中每个活动连接都会占用一个 worker 线程，因此可保持的并发连接数受限于 workers x 线程数，远低于 1M。（cwist 的**明文** HTTP/1.x 路径是事件驱动的，确实达到了 1,000,000/1,000,000 个保持连接 —— 见 cwist README。）C1m 测试衡量的是 churn：在 100,000 个同时保持的 TLS 连接上，由 20 个 h2load 进程各发送 50,000 个请求，并受 watchdog 约束。

| 项目 | 值 |
|------|-------|
| Workers | 12 |
| 负载形态 | 20 x (-c 5000 -n 50000 -r 1000 -T 30) |
| 总量 | 100,000 个保持连接上的 1,000,000 个请求 |
| 结果 | **完成 —— 无停滞** |
| 总成功数 | **1,000,000 / 1,000,000 (100.0%)** |
| 出错数 | 0 |
| 耗时 | ~1:36（每个进程 h2load "finished in" 63.7-89.3 s） |
| 幻影连接 | 0（客户端/服务器 ESTABLISHED 计数一致） |
| 服务器关闭 | 干净退出，exit 0 |

历史：2026-08-23 对相同负载的运行在约 85k 连接时死锁。根因（已在 cwist `perf(https): non-blocking TLS handshake shepherd` 中修复）：TLS 握手在 worker 线程内同步执行，并带有 30 秒的 poll 等待，因此数百个缓慢的客户端会占满整个线程池，accept 队列溢出，溢出的握手被静默丢弃，导致客户端处于 ESTABLISHED 状态而服务器端没有对应 socket。现在握手在非阻塞的 shepherd 线程上运行；只有已建立的会话才占用线程池 worker。

> 注意：数值是在 HTTP/2（TLS 1.3）上维持真实客户端连接时测得。每次测试的 Worker 数量不同；详见“该基准测试衡量什么”。

**关键结论**

- **连接可扩展性**：从 10,000 到 1,000,000 并发连接，RSS 始终维持在 **~110–146 MB** 左右。每个连接的内存成本实际上是平稳的。
- **在真实负载下保持稳定**：C10k 以 **100% 成功**完成，C100k 以 **100.00% 成功**完成，且保持在相同的内存包络内。
- **C1m 规模下内存包络依然成立**：C1m churn 运行（100k 保持的 TLS 连接上的 1M 请求）以 **100% 成功**无停滞完成，RSS 保持在 ~146 MB —— 没有内存螺旋，也没有崩溃。
- **数据安全**：SQLite 在 SIGINT 时安全持久化所有数据（C10k 时 256 次 FS 输出）。

### 吞吐量基准测试

上面的基准测试衡量的是**连接可扩展性**，而非绝对的**请求吞吐量**。为了衡量服务器的原始吞吐上限，我们使用 `h2load`（不带 `-r` 速率限制）通过 HTTP/2 运行了一次无限制测试。

| 项目 | 值 |
|------|-------|
| 命令 | `h2load -c512 -n100000 https://127.0.0.1:8888/` |
| Workers | 12 |
| 并发连接数 | 512 |
| 总请求数 | 100,000 |
| 成功数 | 100,000 |
| 失败 / 错误 / 超时 | 0 |
| 持续时间 | 13.95 s |
| 平均 RPS | **7167.28** |
| 平均吞吐量 | **290.51 MB/s** |
| 请求延迟 (h2load `time for request`) | min 183 µs, mean 30.69 ms, max 209.00 ms, sd 11.18 ms |

#### 使用 `wrk` 的 HTTP/1.1 对比

作为对比，同一端点使用 `wrk` 通过 HTTP/1.1 进行了测试。由于协议和基准工具均不同，以下数值**不能直接与上方 HTTP/2 h2load 结果比较**。

| 项目 | 值 |
|------|-------|
| 命令 | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` |
| 持续时间 | 60 s |
| 每秒请求数 | **1282.49** |
| 每秒传输量 | 52.29 MB |
| Latency | Avg 138.61 ms, Stdev 39.26 ms, Max 311.70 ms |

这些数字展示了引擎在集中、非速率限制负载下的绝对吞吐上限。它们与上面的连接可扩展性测试是分开的。
