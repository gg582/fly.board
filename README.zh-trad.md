# fly.board

![fly.board logo](img/logo.png)

> 少數能在連線數增加時仍幾乎維持記憶體平坦的簡易部落格引擎之一：閒置時 **~82 MB RSS**（4 個 workers；在單一 worker 的實際生產伺服器上則維持於 **68–120 MB**），即使在 C10k、C100k 乃至 C1m 下仍維持 **~146 MB**。  
> 以 C 語言 CWIST Web 框架為基礎，支援 HTTPS/3、Argon2id、PQC 簽章與 NATS 訊息的輕量化論壇兼部落格引擎。

## 特性

- **記憶體高效且具連線擴展性** – 堆疊+堆積 C 實作。閒置時 **~82 MB RSS**；從 C10k 到 C1m 的併發連線下，RSS 皆維持在 **~146 MB**。
- **現代傳輸層** – 預設 TLS 1.3 + HTTP/3（QUIC）。可選 ECH（Encrypted Client Hello）。
- **安全認證** – 用戶端 SHA-512 預雜湊 + 伺服端 **Argon2id**（OpenSSL 3 KDF）。JWT 工作階段 Cookie。
- **論壇 / 部落格混合** – Slug 式 Markdown 文章 + 多看板 + 巢狀評論。
- **即時預覽** – 從 Markdown 編輯器即時渲染的伺服端預覽。
- **PQC 簽章** – 在文章上附加/驗證後量子密碼（PQC）簽章。
- **檔案儲存** – ≤1 MB 存於 SQLite，較大檔案存於磁碟區。自動嵌入圖片/影片/音訊。
- **NATS 整合** – 透過 `NATS_URL` 環境變數連接分散式訊息閘道。
- **深色模式** – 基於 Cookie 的主題切換與動態 CSS 變數。

## 建置

```sh
make
./keygen.sh
```

相依套件：
- [CWIST](https://github.com/religiya-serdtsa/cwist) — TLS 1.3 / HTTP/3（QUIC）由 CWIST 內建的 BoringSSL 處理，無需額外設定。
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
# or
BLOG_ECH_DIR=ech ./fly_board
```

若 OpenSSL 建置不支援 ECH，將記錄警告並繼續使用一般 HTTPS/3。

### NATS 整合（可選）

```sh
NATS_URL=nats://localhost:4222 ./fly_board
```

## 主要功能

| 功能 | 路徑 | 說明 |
|---------|------|-------------|
| 首頁 | `/` | 最新文章列表 |
| 看板 | `/boards` | 多看板管理（admin-only 支援） |
| 文章 | `/post/:slug` | md4c Markdown 渲染 + 評論 + 附件 |
| 登入/註冊 | `/login`、`/register` | Argon2id + JWT Cookie |
| 個人資料 | `/profile` | 暱稱、簡介、大頭貼、加入日期 |
| 帳戶設定 | `/account/settings` | 編輯個人資料 |
| 修改密碼 | `/account/password` | 驗證目前密碼後以 Argon2id 重新雜湊 |
| 管理員 | `/admin/users` | 變更使用者角色、刪除使用者 |
| 檔案儲存 | `/files` | 上傳/下載/刪除 |

## 設定

- `blog.settings` – 部落格標題、副標題、頁尾、埠號與上傳限制
- `admin.settings` – 管理員帳戶（2 行：`username`\n`password`）

## 資料庫

SQLite3（`data/blog.db`）。應用程式啟動時會自動遷移綱要。

```
users       – 帳戶、Argon2id 雜湊、角色、個人資料
boards      – 看板名稱/Slug/說明/admin_only
posts       – Markdown 正文、PQC 簽章、摘要
files       – 附件路徑/大小/MIME
comments    – 巢狀評論（target_type, parent_id）
board_permissions – 私人看板存取權限
```

## 架構

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

## 可擴展性基準測試

### 此基準測試測量什麼

這些測試使用 `h2load` **並加上 `-r`（rate-limit）選項**。它們刻意**不是**最大吞吐量測試，而是測量伺服器在處理受控的每程序請求速率時，是否能夠**維持大量併發 HTTP/2 連線**。

由於負載受到速率限制：

- 回報的 **RPS 反映設定的請求速率**，而非伺服器的絕對吞吐量上限。
- 首要指標是連線數從 10,000 成長到 1,000,000 時，**常駐記憶體集（RSS）的穩定性**。

worker 數量會隨負載調整，讓每項測試貼近現實：C10k 為 **4 個 workers**、C100k 為 **12 個 workers**、C1m 為 **24 個 workers**。這也解釋了三次執行中 CPU 使用率數字的差異。

### 主機環境

| 項目 | 值 |
|------|-------|
| OS | Linux 7.1.0-mountain-rc6+ |
| Architecture | x86_64 |
| CPU | 12 logical cores |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| OpenSSL | 3.5.6 |
| 基準測試工具 | h2load nghttp2/1.64.0 |
| CWIST | `/usr/local/lib/libcwist.a` |

### 系統調校

| 參數 | 值 |
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

### 記憶體使用量

| 狀態 | RSS | 較前項變化 | 備註 |
|-------|-----|-----------------|-------|
| 閒置 | **~82 MB** (83,708 KB) | — | 4 workers, no connections |
| C10k | **~146 MB** (145,928 KB) | +62.22 MB | 10,000 concurrent connections |
| C100k | **~146 MB** (146,076 KB) | +148 KB | 100,000 concurrent connections |
| C1m | **~146 MB** (146,420 KB) | +344 KB | 1,000,000 concurrent connections |

從 **C10k 到 C1m 的總 RSS 增長僅約 492 KB** —— 基本上只是雜訊。這是本基準測試最重要的結果。

### C10k 併發連線測試

使用 `h2load` 維持 10,000 個併發連線進行測量。

| 項目 | 值 |
|------|-------|
| Workers | 4 |
| Concurrent connections | 10,000 |
| Duration | 17.04 s |
| Max RSS | **~146 MB** (145,928 KB) |
| CPU usage | ~480% |
| User time | 73.54 s |
| System time | 8.25 s |
| Major page faults | 51 |
| Minor page faults | 267,239 |
| Voluntary context switches | 1,959,611 |
| Involuntary context switches | 17,100 |
| File system outputs | 10,600 |
| Total requests | 20000 |
| Total succeeded | 20000 |
| Total failed | 0 |
| Approx total RPS | **2383.81** |
| Success rate | **100.00%** |
| Exit status | **0** |

### C100k 併發連線測試

使用 `h2load` 維持 100,000 個併發連線進行測量。

| 項目 | 值 |
|------|-------|
| Workers | 12 |
| Concurrent connections | 100,000 |
| Duration | 1:30.30 |
| Max RSS | **~146 MB** (146,076 KB) |
| CPU usage | ~824% |
| User time | 700.38 s |
| System time | 44.12 s |
| Major page faults | 0 |
| Minor page faults | 472,679 |
| Voluntary context switches | 3,908,475 |
| Involuntary context switches | 165,739 |
| File system outputs | 101,672 |
| Total requests | 200000 |
| Total succeeded | 200000 |
| Total failed | 0 |
| Approx total RPS | **2458.23** |
| Success rate | **100.00%** |
| Exit status | **0** |

### C1m 併發連線測試

使用 `h2load` 維持 1,000,000 個併發連線進行測量。

| 項目 | 值 |
|------|-------|
| Workers | 24 |
| Concurrent connections | 1,000,000 |
| Duration | 7:02.81 |
| Max RSS | **~146 MB** (146,420 KB) |
| CPU usage | ~654% |
| User time | 2553.88 s |
| System time | 211.70 s |
| Major page faults | 3 |
| Minor page faults | 895,633 |
| Voluntary context switches | 24,007,690 |
| Involuntary context switches | 931,088 |
| File system outputs | 366,248 |
| Total requests | 2000000 |
| Total succeeded | 722910 |
| Total failed | 1277090 |
| Approx total RPS | **1744.04** |
| Success rate | **36.14%** |
| Exit status | **0** |

> 注意：這些數值是在 HTTP/2（TLS 1.3）上維持實際客戶端連線時測得。每次測試的 worker 數量不同；請參閱「此基準測試測量什麼」。

**重點摘要**

- **連線擴展性**：從 10,000 到 1,000,000 個併發連線，RSS 皆維持在 **~146 MB**。每條連線的記憶體成本幾乎是平坦的。
- **在現實負載下穩定**：C10k 與 C100k 在 **100% 成功率**下完成，且記憶體使用維持在相同範圍內。
- **C1m 仍維持記憶體範圍**：即使測試硬體無法完整服務全部 1,000,000 條連線（成功率 36.14%），記憶體使用量仍幾乎不變 —— 伺服器並未失控膨脹。
- **資料安全性**：SQLite 在 SIGINT 時安全地持久化所有資料（C10k 時為 10,600 次 FS 輸出）。

### 吞吐量基準測試

上述基準測試測量的是**連線擴展性**，而非絕對的**請求吞吐量**。為了測量伺服器的原始吞吐量上限，我們使用 `h2load`（無 `-r` 速率限制）透過 HTTP/2 執行了無限制測試。

| 項目 | 值 |
|------|-------|
| Command | `h2load -c512 -n100000 https://127.0.0.1:8888/` |
| Workers | 12 |
| Concurrent connections | 512 |
| Total requests | 100,000 |
| Succeeded | 100,000 |
| Failed / Errored / Timeout | 0 |
| Duration | 13.95 s |
| Mean RPS | **7167.28** |
| Mean throughput | **290.51 MB/s** |

作為比較，同一端點使用 `wrk` 透過 HTTP/1.1 進行了測試：

| 項目 | 值 |
|------|-------|
| Command | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` |
| Duration | 60 s |
| Requests/sec | **1282.49** |
| Transfer/sec | 52.29 MB |

這些數字顯示了引擎在集中、未限速負載下的絕對吞吐量上限，與上述的連線擴展性測試是分開的。
