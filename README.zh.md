# fly.board

![fly.board logo](img/logo.png)

> 为数不多的简单博客引擎之一：在一台桌面级主机上保持 **100 万个并发连接**（明文 HTTP/1.1，实测），并以 100% 成功率通过 C10k/C100k TLS 负载测试。
> 基于 C 语言 CWIST Web 框架的轻量级论坛兼博客引擎，支持 HTTPS/3、Argon2id、PQC 签名与 NATS 消息。

## 特性

- **连接可扩展** – 基于 cwist 事件驱动 reactor 的栈+堆 C 实现。实测中保持并服务了 **100 万个并发明文 HTTP/1.1 连接**；匿名公开页面由 Big Dumb Reply 缓存提供。
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

### `s3.settings`（可选）

S3 兼容的对象存储，用于存放上传的文件（AWS S3、MinIO、R2、B2）。完全可选——留空时文件保留在本地磁盘的 `public/uploads/` 下。支持 `mode=mirror`（保留本地副本 + S3 备份）和 `mode=offload`（移至 S3，通过预签名重定向提供下载）。完整参考和示例配置见 [S3.md](S3.md)。

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

### 这些基准测试测量什么

测量的是两件不同的事，本节的旧版本把两者混为一谈：

- **并发连接数**（传统 C10K/C1M 的含义）：服务器同时保持打开并提供服务的连接数量。通过 `run_c1m_held_bench.sh` 使用 `tools/connhold` 测量。
- **保持连接上的请求处理（churn）**：`h2load` 测试组（`run_c10k_bench.sh`、`run_c100k_bench.sh`、`run_c1m_bench.sh`）。`h2load` 带 `-r`（速率限制）运行，因此 RPS 反映的是配置的负载，而不是吞吐量上限。`run_c1m_bench.sh` 是 10 万并发连接、100 万次请求，名称沿用历史。

### 主机环境

| 项目 | 值 |
|------|-------|
| OS | Linux 6.12.107+deb13-amd64 (Debian 13) |
| CPU | AMD Ryzen 5 5600X（6 核 / 12 线程） |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| 负载生成器 | h2load nghttp2/1.64.0、`tools/connhold`（BoringSSL） |
| CWIST | `main` `468a94d7`（2026-09-29） |
| TLS 证书 | ECDSA P-256（`keygen.sh` 默认） |
| 服务模式 | `CWIST_C1M_MODE=1`（事件驱动 reactor） |

### 系统调优

| 参数 | 值 |
|-----------|-------|
| ulimit -n | 1,050,000 |
| fs.file-max | 8,388,608（100 万连接在客户端和服务器端共需 200 万个 fd） |
| fs.nr_open | 1,050,000 |
| net.netfilter.nf_conntrack_max | 4,194,304（loopback 连接也会被跟踪） |
| net.core.somaxconn | 1,050,000 |
| net.ipv4.tcp_max_syn_backlog | 1,050,000 |
| net.ipv4.ip_local_port_range | 1024 65535 |
| vm.max_map_count | 1,048,576 |
| kernel.pid_max | 4,194,304 |
| CPU governor | ecodemand |

### C1M：100 万并发连接（2026-09-29）

`run_c1m_held_bench.sh`：12 个 worker，客户端与服务器在同一台主机，100 万个连接分布在 48 个 loopback 地址上。每个连接发送 `GET /robots.txt`，之后每 120 秒重发一次，使 keep-alive 计时器不会到期。失败的连接只计数，不重试。

| | 明文 HTTP/1.1 | TLS 1.3 + HTTP/1.1 |
|---|---|---|
| 打开的连接 | 1,000,000 | 1,000,000 |
| 连接速率 | 每秒 40,000 | 每秒 20,000 |
| 峰值保持数（已打开且通过 TLS） | **1,000,000** | 395,729 |
| 同时被服务的峰值 | **1,000,000** | **25** |
| 响应数（含 keep-alive GET） | 1,124,246 | 52 |
| 失败的连接 | 0 | 1,000,000 |
| 峰值时服务器内存（PSS，全部 worker） | 24.0 GB | 7.1 GB |

- **明文 HTTP/1.1 保持并服务了全部 100 万个连接。** cwist 的明文路径是事件驱动的，空闲连接只占内存（这里每个约 24 KB），不占线程。
- **TLS 连接能被保持，但不能被同时服务。** 握手在非阻塞的 shepherd 线程上完成，但握手后的连接在整个生命周期内都由一个 HTTPS 池线程服务（HTTP/1.1 keep-alive 在那里等待下一个请求，HTTP/2 等到连接空闲）。同时被服务的 TLS 连接大约只有池线程那么多，其余的在等待中被服务器 45 秒握手预算或客户端 60 秒响应期限关闭。
- 负载构造陷阱：Linux 的 `connect()` 先分配偶数临时端口，用完后转入缓慢的奇数端口搜索，因此每个目标地址只能快速建立约 3.2 万个连接。使用 24 个地址时每次都卡在约 77.4 万；请至少使用 `连接数 / 32,000` 个地址。

### 每连接内存

2026-09-29 测量，对全部服务器 worker 进程（而不只是 master）的 PSS 求和。

| 场景 | 服务器 | 内核（slab + TCP 缓冲区） | 客户端 |
|---|---|---|---|
| 10 万个 TLS/HTTP/2 连接，h2load C100k | 约 6.7 KB（预热后空闲 1.11 GB → 1.77 GB） | 约 12.7 KB | h2load 约 60 KB |
| 100 万个明文 HTTP/1.1 连接，connhold | 约 24 KB（合计 24.0 GB） | — | connhold 约 0.06 KB |

> **更正：** 本 README 的旧版本称从 C10k 到 C1m RSS 保持在约 110–146 MB。这些数值是 `/usr/bin/time -v` 测得的 master 进程单独的最大 RSS；`cwist_app_listen()` 会 fork 出服务 worker，而它们的内存从未被计入。以上合计取代了旧数值。

### h2load 测试组：请求处理（2026-09-29）

| 测试 | 并发连接 | 请求 | 成功 | 耗时 | RPS（各进程之和） |
|---|---|---|---|---|---|
| C10k（4 个 worker） | 10,000 | 20,000 | **100%** | 4.56 秒 | 7,445 |
| C100k（12 个 worker） | 100,000 | 200,000 | **100%** | 24.31 秒 | 9,254 |
| C1m churn（12 个 worker） | 100,000 | 1,000,000 | **100%** | 49.65 秒 | 21,812 |

耗时是服务器进程的生命周期（包括启动和 5 秒 drain）。响应是完整的 79 KB 首页；h2load 不请求压缩。

同一天较早时使用 RSA-4096 证书，C100k 降到 72.6%，C1m churn 降到 65.2%：几乎所有繁忙的 CPU 都花在每次 TLS 1.3 完整握手的 RSA CertificateVerify 签名上，排队的握手因此触及 45 秒预算。改用 ECDSA P-256（`keygen.sh` 默认）、在请求 worker 上运行路由处理器、并用路由 Big Dumb Reply 缓存提供匿名公开页面后，恢复到 100%。

**要点**

- **C1M，明文：** 在一台桌面级主机上保持并服务 100 万个并发 HTTP/1.1 连接，零失败。
- **C1M，TLS：** 连接被接受并通过握手，但 TLS 连接的并发*服务*受 HTTPS 池线程数限制。TLS C100k churn（h2load）为 100%。
- **每连接内存是真实开销：** 服务器端约 7 KB（TLS/HTTP/2，h2load C100k）到约 24 KB（明文，100 万保持）；请据此规划内存。
- **TLS churn 由握手开销主导：** 请使用 ECDSA 证书。

### 吞吐量基准测试

> 这是 2026-08 测得的数值，早于 2026-09 的变更（异步路由、路由 BDR 缓存、ECDSA 证书），未重新测量。

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
