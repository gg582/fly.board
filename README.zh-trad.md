# fly.board

![fly.board logo](img/logo.png)

> 閒置時僅 **22-30 MB RSS**，C10k（10,000 併發連線）下峰值仍僅約 **369 MB** 的極簡部落格系統。  
> 基於 C 語言 CWIST Web 框架，支援 HTTPS/3、Argon2id、PQC 簽章與 NATS 訊息的輕量化論壇兼部落格引擎。

## 特性

- **記憶體節省** – 堆疊+堆積 C 實作。閒置時 **22-30 MB**，10,000 併發連線（C10k）下最大 RSS 僅約 **369 MB**。
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
- [CWIST](https://github.com/religiya-serdtsa/cwist)
- OpenSSL 3.x（Argon2id KDF、TLS 1.3、QUIC）
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

> 詳細的基準測試方法與完整結果請參閱 [`benchmarks/README.md`](benchmarks/README.md)。

### 主機環境

| 項目 | 數值 |
|------|-----|
| 作業系統 | Linux 7.0.0-mountain+ |
| 架構 | x86_64 |
| CPU | AMD Ryzen 5 5600X @ 3.70GHz（6 核 / 12 執行緒） |
| RAM | 64 GB |
| 磁碟 | Samsung SSD 980 1TB（NVMe） |
| OpenSSL | 3.5.5 |
| 基準工具 | wrk |
| CWIST | `patches/cwist`（已套用 SIGPIPE 修補程式） |

### 最大吞吐量（RPS）

`wrk -t4 -c400 -d30s`（TLS 1.3，無序列化）

| 端點 | 最高 RPS | 平均延遲 | 說明 |
|------|----------|----------|------|
| `/`（首頁） | **3,409.92** | 121.84ms | 資料庫查詢 + Markdown 渲染 |
| `/login` | **3,948.77** | 18.03ms | 靜態表單（可快取） |
| `/boards` | **3,901.77** | 17.26ms | 資料庫驅動列表 |

### 資源佔用（峰值負載）

| 項目 | 數值 |
|------|-----|
| CPU 使用率 | 約 600%（12 執行緒系統） |
| RAM（RSS） | 約 12 MB |
| 虛擬記憶體（VSZ） | 約 1.2 GB |

> 注意：基準測試在**未**對請求進行序列化（`pthread_mutex_t`）的狀態下執行。  
> `ulimit -n` 已設定為 20,000，因此最多 400 個併發連線均可穩定測量。

### C10k 併發連線測試

在實際執行環境中維持 10,000 個併發連線（C10k）進行測量（`sudo -E /usr/bin/time -v ./fly_board`）。

| 項目 | 數值 |
|------|-----|
| 併發連線數 | 10,000 |
| 持續時間 | 24 分 46 秒 |
| 最大 RSS | **約 369 MB** (368,644 KB) |
| 平均 CPU 併用率 | 約 93% |
| User time | 444.17 秒 |
| System time | 951.76 秒 |
| Major page faults | **0** (無磁碟 I/O) |
| Minor page faults | 219,629 |
| Swaps | **0** |
| File system inputs | **0** |
| File system outputs | 89,208 (安全資料落盤) |
| Voluntary context switches | 346,110,015 |
| Involuntary context switches | 1,690,588 |
| 結束狀態 | **0** (SIGINT 後正常結束) |

> 注意：在 HTTP/3（QUIC）over TLS 1.3 環境下維持 10,000 個真實客戶端連線測得。

**C10k 基準測試核心優勢**
- **記憶體高效**: 10,000 併發連線下 RSS 仍低於 400 MB（每連線約 37 KB）
- **零磁碟 I/O**: Major page faults 0、Swaps 0、FS inputs 0 — 高負載下仍純記憶體處理
- **CPU 充分利用**: 93% CPU 佔用率下依然穩定運行
- **長時間穩定性**: 持續 24 分 46 秒的 C10k 滿載後正常結束（Exit status 0）
- **資料安全**: 收到 SIGINT 後 NukeDB 仍安全保存全部資料
