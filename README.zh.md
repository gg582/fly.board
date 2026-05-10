# fly.board

![fly.board logo](img/logo.png)

> 极少数能在 **8–16 MB RSS** 下运行的简约博客系统。  
> 基于 C 语言 CWIST Web 框架，支持 HTTPS/3、Argon2id、PQC 签名与 NATS 消息的轻量级论坛兼博客引擎。

## 特性

- **内存节省** – 栈+堆 C 实现。生产环境 RSS 维持在 **8–16 MB**。
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
- [CWIST](https://github.com/religiya-serdtsa/cwist)
- OpenSSL 3.x (Argon2id KDF、TLS 1.3、QUIC)
- ngtcp2 / nghttp3 (HTTP/3)
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

> 详细的基准测试方法与完整结果请参阅 [`benchmarks/README.md`](benchmarks/README.md)。

### 主机环境

| 项目 | 值 |
|------|-----|
| 操作系统 | Linux 5.15.19 |
| 架构 | x86_64 |
| CPU | 4 核 |
| OpenSSL | 3.2.1 |
| 基准工具 | wrk |
| CWIST | `patches/cwist` (已应用 SIGPIPE 补丁) |

### 最大吞吐量 (RPS)

| 端点 | 最高 RPS | 平均延迟 | 说明 |
|------|----------|----------|------|
| `/` (首页) | **729.79** | 7.29ms | 数据库查询 + Markdown 渲染 |
| `/login` | **768.09** | 48.66ms | 静态表单（可缓存） |
| `/boards` | **681.60** | 124.29ms | 数据库驱动列表 |

### 资源占用（峰值负载）

| 项目 | 值 |
|------|-----|
| CPU 使用率 | 约 140% (4 核系统) |
| RAM (RSS) | 约 11.5 MB |
| 虚拟内存 (VSZ) | 约 1.1 GB |

> 注意：基准测试在请求被序列化（`pthread_mutex_t`）的状态下执行。  
> `ulimit -n` 限制为 1024，因此超过 400 个并发连接时可能因 `TIME_WAIT` 累积而拒绝连接。
