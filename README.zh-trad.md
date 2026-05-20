# fly.board

![fly.board logo](img/logo.png)

> 閒置時僅 **~577 MB RSS**，C10k（10,000 併發連線）下峰值仍僅約 **658 MB** 的極簡部落格系統。  
> 基於 C 語言 CWIST Web 框架，支援 HTTPS/3、Argon2id、PQC 簽章與 NATS 訊息的輕量化論壇兼部落格引擎。

## 特性

- **記憶體節省** – 堆疊+堆積 C 實作。閒置時 **~577 MB**，10,000 併發連線（C10k）下最大 RSS 僅約 **658 MB**。
- **最新傳輸層** – 預設 TLS 1.3 + HTTP/3（QUIC）。可選 ECH（Encrypted Client Hello）。
- **安全認證** – 用戶端 SHA-512 預雜湊 + 伺服端 **Argon2id**（OpenSSL 3 KDF）。JWT 工作階段 Cookie。
- **論壇 / 部落格混合** – Slug 式 Markdown 文章 + 多看板 + 巢狀評論。
- **即時預覽** – Markdown 編輯器中輸入即時伺服端預覽。
- **PQC 簽章** – 為文章附加/驗證後量子密碼（PQC）簽章。
- **檔案倉庫** – 1 MB 以內存 SQLite，超過則磁碟區儲存。圖片/影片/音訊自動嵌入。
- **NATS 整合** – 透過 `NATS_URL` 環境變數連接分散式訊息閘道。
- **深色模式** – 基於 Cookie 的主題切換 + 動態 CSS 變數。

## 建置

```sh
make
./keygen.sh
```

相依套件：
- [CWIST](https://github.com/religiya-serdtsa/cwist) — TLS 1.3 / HTTP/3（QUIC）由 CWIST 內建的 BoringSSL 處理，無需額外安裝。
- OpenSSL 3.x（Argon2id KDF）
- ngtcp2 / nghttp3（HTTP/3）
- cJSON、SQLite3

`Makefile` 會複製並建置 `third_party/md4c` 為靜態函式庫。

## 執行

```sh
./fly_board
```

預設埠號遵循 `blog.settings` 中的 `port` 值（預設 9443）。

```text
https://localhost:9443
```

HTTP/3 在同一埠號的 UDP 上監聽。

### 啟用 ECH（可選）

```sh
BLOG_ECH_KEY=ech/server.ech ./fly_board
# 或
BLOG_ECH_DIR=ech ./fly_board
```

若 OpenSSL 建置不支援 ECH，將記錄警告紀錄並繼續使用常規 HTTPS/3。

### NATS 整合（可選）

```sh
NATS_URL=nats://localhost:4222 ./fly_board
```

## 主要功能

| 功能 | 路徑 | 說明 |
|------|------|------|
| 首頁 | `/` | 最新文章列表 |
| 看板 | `/boards` | 多看板管理（支援 admin-only） |
| 文章 | `/post/:slug` | md4c Markdown 渲染 + 評論 + 附件 |
| 登入/註冊 | `/login`、`/register` | Argon2id + JWT Cookie |
| 個人資料 | `/profile` | 暱稱、簡介、大頭貼、註冊日期 |
| 帳戶設定 | `/account/settings` | 修改個人資料 |
| 修改密碼 | `/account/password` | 驗證目前密碼後以 Argon2id 重新雜湊 |
| 管理員 | `/admin/users` | 變更使用者角色、刪除使用者 |
| 檔案倉庫 | `/files` | 上傳/下載/刪除 |

## 設定檔

- `blog.settings` – 部落格標題、副標題、頁尾、埠號
- `admin.settings` – 管理員帳戶（2 行：`使用者名稱`\n`密碼`）

## 資料庫

基於 SQLite3（`data/blog.db`）。應用程式在啟動時自動遷移綱要。

```
users       – 帳戶、Argon2id 雜湊、角色、個人資料
boards      – 看板名稱/Slug/說明/admin_only
posts       – Markdown 正文、PQC 簽章、摘要
files       – 附件路徑/大小/MIME
comments    – 巢狀評論（target_type, parent_id）
board_permissions – 私人看板存取權限
```

## 架構概覽

```
CWIST（HTTP/3, TLS 1.3）
  ├── src/auth/     – Argon2id、JWT、工作階段
  ├── src/db/       – SQLite3 CRUD
  ├── src/handlers/ – 路由/業務邏輯
  ├── src/render/   – cwist_html_element SSR + md4c
  ├── src/crypto/   – PQC 簽章/驗證
  └── src/nats/     – 訊息發布/訂閱
```

## 授權條款

MIT License

---

## 效能基準測試

### 主機環境

| 項目 | 值 |
|------|-------|
| OS | Linux 7.0.0-mountain+ |
| 架構 | x86_64 |
| CPU | AMD Ryzen 5 5600X @ 3.70GHz (6 cores / 12 threads) |
| RAM | 64 GB |
| 磁碟 | Samsung SSD 980 1TB (NVMe) |
| OpenSSL | 3.5.5 |
| 基準工具 | wrk, h2load |
| CWIST | `patches/cwist` |

### 最大吞吐量（RPS）

`wrk -t4 -c400 -d30s` (TLS 1.3)

| 端點 | 峰值 RPS | 平均延遲 | 說明 |
|----------|----------|-------------|-------|
| `/` (Home) | **941.67** | 174.60ms | DB query + markdown rendering |
| `/login` | **927.08** | 175.83ms | Static form |
| `/boards` | **920.36** | 178.16ms | DB-driven list |

> 注意：TLS 連線斷開時會出現 socket read error，但不影響吞吐量測量。

### 記憶體使用量

| 狀態 | RSS | 備註 |
|-------|-----|-------|
| 閒置 | **~577 MB** (590,528 KB) | 4 workers, no connections |
| C10k | **~658 MB** (673,688 KB) | 10,000 concurrent connections |
| C100k | **~692 MB** (708,300 KB) | 100,000 concurrent connections |

### C10k 併發連線測試

使用 `h2load` 維持 10,000 個併發連線進行測量。

| 項目 | 值 |
|------|-------|
| 併發連線數 | 10,000 |
| 持續時間 | 21.98 s |
| 最大 RSS | **約 658 MB** (673,688 KB) |
| CPU 使用率 | ~199% |
| User time | 36.41 s |
| System time | 7.43 s |
| Major page faults | **0** |
| Minor page faults | 170,352 |
| Voluntary context switches | 2,197,128 |
| Involuntary context switches | 293,375 |
| File system outputs | 72 |
| 結束狀態 | **0** |

### C100k 併發連線測試

使用 `h2load` 維持 100,000 個併發連線進行測量。

| 項目 | 值 |
|------|-------|
| 併發連線數 | 100,000 |
| 持續時間 | 2:38.55 |
| 最大 RSS | **約 692 MB** (708,300 KB) |
| CPU 使用率 | ~91% |
| User time | 120.81 s |
| System time | 24.13 s |
| Major page faults | **0** |
| Minor page faults | 191,633 |
| Voluntary context switches | 6,371,528 |
| Involuntary context switches | 842,479 |
| File system outputs | 72 |
| 結束狀態 | **0** |

> 注意：在 HTTP/2 (TLS 1.3) 上維持實際客戶端連線時測得的值。

**C10k 基準測試核心優勢**
- **記憶體高效**: 10,000 併發連線下 RSS 仍低於 660 MB（每連線約 66 KB）
- **零磁碟 I/O**: Major page faults 0, Swaps 0, FS inputs 0 — 負載下純記憶體處理
- **高 CPU 利用率**: 穩定維持 ~199% CPU 使用率
- **長時間穩定性**: 持續 21.98 秒的 C10k 滿載後正常結束（Exit status 0）
- **資料安全性**: SIGINT 後 SQLite 安全持久化資料（72 FS outputs）
