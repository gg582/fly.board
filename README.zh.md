# fly.board

![fly.board logo](img/logo.png)

> 空闲时仅 **~577 MB RSS**，C10k（10,000 并发连接）下峰值仍仅约 **658 MB** 的极简博客系统。  
> 基于 C 语言 CWIST Web 框架，支持 HTTPS/3、Argon2id、PQC 签名与 NATS 消息的轻量级论坛兼博客引擎。

## 特性

- **内存节省** – 栈+堆 C 实现。空闲时 **~577 MB**，10,000 并发连接（C10k）下最大 RSS 仅约 **658 MB**。
- **最新传输层** – 默认 TLS 1.3 + HTTP/3(QUIC)。可选 ECH(Encrypted Client Hello)。
- **安全认证** – 客户端 SHA-512 预哈希 + 服务端 **Argon2id** (OpenSSL 3 KDF)。JWT 会话 Cookie。
- **论坛 / 博客混合** – Slug 式 Markdown 文章 + 多板块 + 嵌套评论。
- **实时预览** – Markdown 编辑器中输入即时服务端预览。
- **PQC 签名** – 为文章附加/验证后量子密码(PQC)签名。
- **文件仓库** – 1 MB 以内存 SQLite，超过则卷存储。图片/视频/音频自动嵌入。
- **NATS 集成** – 通过 `NATS_URL` 环境变量连接分布式消息网关。
- **深色模式** – 基于 Cookie 的主题切换 + 动态 CSS 变量。

## 构建

```sh
make
./keygen.sh
```

依赖：
- [CWIST](https://github.com/religiya-serdtsa/cwist) — TLS 1.3 / HTTP/3（QUIC）由 CWIST 内置的 BoringSSL 处理，无需额外安装。
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

如果 OpenSSL 构建不支持 ECH，将记录警告日志并继续使用常规 HTTPS/3。

### NATS 集成（可选）

```sh
NATS_URL=nats://localhost:4222 ./fly_board
```

## 主要功能

| 功能 | 路径 | 说明 |
|------|------|------|
| 首页 | `/` | 最新文章列表 |
| 板块 | `/boards` | 多板块管理（支持 admin-only） |
| 文章 | `/post/:slug` | md4c Markdown 渲染 + 评论 + 附件 |
| 登录/注册 | `/login`、`/register` | Argon2id + JWT Cookie |
| 个人资料 | `/profile` | 昵称、简介、头像、注册日期 |
| 账户设置 | `/account/settings` | 修改个人资料 |
| 修改密码 | `/account/password` | 验证当前密码后用 Argon2id 重新哈希 |
| 管理员 | `/admin/users` | 更改用户角色、删除用户 |
| 文件仓库 | `/files` | 上传/下载/删除 |

## 配置文件

- `blog.settings` – 博客标题、副标题、页脚、端口
- `admin.settings` – 管理员账户（2 行：`用户名`\n`密码`）

## 数据库

基于 SQLite3 (`data/blog.db`)。应用在启动时自动迁移模式。

```
users       – 账户、Argon2id 哈希、角色、个人资料
boards      – 板块名称/Slug/说明/admin_only
posts       – Markdown 正文、PQC 签名、摘要
files       – 附件路径/大小/MIME
comments    – 嵌套评论 (target_type, parent_id)
board_permissions – 私有板块访问权限
```

## 架构概览

```
CWIST (HTTP/3, TLS 1.3)
  ├── src/auth/     – Argon2id、JWT、会话
  ├── src/db/       – SQLite3 CRUD
  ├── src/handlers/ – 路由/业务逻辑
  ├── src/render/   – cwist_html_element SSR + md4c
  ├── src/crypto/   – PQC 签名/验证
  └── src/nats/     – 消息发布/订阅
```

## 许可证

MIT License

---

## 性能基准测试

### 主机环境

| 项目 | 值 |
|------|-------|
| OS | Linux 7.0.0-mountain+ |
| 架构 | x86_64 |
| CPU | AMD Ryzen 5 5600X @ 3.70GHz (6 cores / 12 threads) |
| RAM | 64 GB |
| 磁盘 | Samsung SSD 980 1TB (NVMe) |
| OpenSSL | 3.5.5 |
| 基准工具 | wrk, h2load |
| CWIST | `patches/cwist` |

### 最大吞吐量 (RPS)

`wrk -t4 -c400 -d30s` (TLS 1.3)

| 端点 | 峰值 RPS | 平均延迟 | 说明 |
|----------|----------|-------------|-------|
| `/` (Home) | **941.67** | 174.60ms | DB query + markdown rendering |
| `/login` | **927.08** | 175.83ms | Static form |
| `/boards` | **920.36** | 178.16ms | DB-driven list |

> 注意：TLS 连接断开时会出现 socket read error，但不影响吞吐量测量。

### 内存使用量

| 状态 | RSS | 备注 |
|-------|-----|-------|
| 空闲 | **~577 MB** (590,528 KB) | 4 workers, no connections |
| C10k | **~658 MB** (673,688 KB) | 10,000 concurrent connections |
| C100k | **~692 MB** (708,300 KB) | 100,000 concurrent connections |

### C10k 并发连接测试

使用 `h2load` 维持 10,000 个并发连接进行测量。

| 项目 | 值 |
|------|-------|
| 并发连接数 | 10,000 |
| 持续时间 | 21.98 s |
| 最大 RSS | **约 658 MB** (673,688 KB) |
| CPU 使用率 | ~199% |
| User time | 36.41 s |
| System time | 7.43 s |
| Major page faults | **0** |
| Minor page faults | 170,352 |
| Voluntary context switches | 2,197,128 |
| Involuntary context switches | 293,375 |
| File system outputs | 72 |
| 退出状态 | **0** |

### C100k 并发连接测试

使用 `h2load` 维持 100,000 个并发连接进行测量。

| 项目 | 值 |
|------|-------|
| 并发连接数 | 100,000 |
| 持续时间 | 2:38.55 |
| 最大 RSS | **约 692 MB** (708,300 KB) |
| CPU 使用率 | ~91% |
| User time | 120.81 s |
| System time | 24.13 s |
| Major page faults | **0** |
| Minor page faults | 191,633 |
| Voluntary context switches | 6,371,528 |
| Involuntary context switches | 842,479 |
| File system outputs | 72 |
| 退出状态 | **0** |

> 注意：在 HTTP/2 (TLS 1.3) 上维持实际客户端连接时测得的值。

**C10k 基准测试核心优势**
- **内存高效**: 10,000 并发连接下 RSS 仍低于 660 MB（每连接约 66 KB）
- **零磁盘 I/O**: Major page faults 0, Swaps 0, FS inputs 0 — 负载下纯内存处理
- **高 CPU 利用率**: 稳定维持 ~199% CPU 使用率
- **长时间稳定性**: 持续 21.98 秒的 C10k 满载后正常退出（Exit status 0）
- **数据安全性**: SIGINT 后 SQLite 安全持久化数据（72 FS outputs）
