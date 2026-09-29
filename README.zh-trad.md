# fly.board

![fly.board logo](img/logo.png)

> 少數能在一台桌上型等級主機上保持 **100 萬個併發連線**（明文 HTTP/1.1、TLS HTTP/1.1 與 TLS HTTP/2 實測），並以 100% 成功率通過 C10k/C100k TLS 負載測試的簡易部落格引擎之一。
> 以 C 語言 CWIST Web 框架為基礎，支援 HTTPS/3、Argon2id、PQC 簽章與 NATS 訊息的輕量化論壇兼部落格引擎。

## 特性

- **連線擴展性** – 建立在 cwist 事件驅動 reactor 上的堆疊+堆積 C 實作。在明文 HTTP/1.1、TLS HTTP/1.1 與 TLS HTTP/2 的實測中保持並服務了 **100 萬個併發連線**；匿名公開頁面由 Big Dumb Reply 快取提供。
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

設定來自三個檔案（首次執行時會以預設值自動建立），再加上用於操作開關的環境變數。

### `admin.settings`

兩行原始文字：第 1 行為管理員使用者名稱，第 2 行為管理員密碼。

### `blog.settings`

純文字 `key=value` 行。未知的鍵會被忽略；無效的值會回退為預設值。

| 鍵 | 預設值 | 值 / 範圍 |
|-----|---------|----------------|
| `title` | `CWIST Docker Blog` | 顯示於頂欄的網站標題 |
| `subtitle` | `Explore boards and read stories.` | Hero 副標題 |
| `brand_footer` | `Built with CWIST C Framework` | 頁尾文字 |
| `root_url` | `https://localhost:8888/` | 站點的正式 URL（結尾需帶 `/`）。用於 RSS 連結、驗證信與憑證續期 —— 正式環境中請設為公開 URL |
| `port` | `8443` | TCP/UDP 監聽埠號（HTTP/3 在同一埠號上使用 UDP） |
| `accent` | `#3b82f6` | 強調色（hex） |
| `use_tls` | `true` | `true`/`false` —— HTTPS 開關（請先執行 `./keygen.sh`） |
| `use_http2` | `true` | TLS 上的 HTTP/2 |
| `use_http3` | `true` | UDP 上的 HTTP/3（QUIC） |
| `use_tasfa` | `true` | TASFA 媒體管線（透過 ffmpeg 產生影片縮圖/預覽） |
| `use_rss` | `false` | 開放 `/rss.xml` |
| `roundness` | `0.0` | UI 圓角程度，`0.0`–`1.0` |
| `max_upload_size` | `1G` | 單一檔案上傳上限。接受後綴 `K/M/G/T`（例如 `500M`） |
| `max_total_parallel_uploads` | `8` | 整體併發上傳數（1–512） |
| `max_upload_parallel_chunks` | `32` | 每次上傳的併發分塊數（1–64） |
| `max_concurrent_downloads` | `128` | 併發下載數（1–512） |
| `vote_only` | *(空白 = `all`)* | 誰可以對文章投票：`all`（任何人，含匿名）、`authorized`（僅登入使用者）、`admin`（僅管理員） |
| `use_special_modes` | *(空白)* | 取代淺色/深色主題：`lightTheme,darkTheme`（或單一主題）。可用主題：`light`、`dark`、`ocean`、`forest`、`sepia`。例如 `ocean,forest` |
| `home_img`、`boards_img`、`files_img` | *(空白)* | 各頁面的 Hero/背景圖片；`public/img/` 內的檔名 |
| `*_dark`（`home_img_dark`、`boards_img_dark`、`files_img_dark`） | *(空白)* | 上述項目的深色模式版本 |
| `blog_logo`、`blog_logo_dark` | *(空白)* | `public/img/` 中的 Logo 圖片 |
| `invert_logo` | `false` | 為沒有圖片的模式自動反轉 Logo |
| `favicon` | *(空白)* | `public/img/` 中的 Favicon 檔案 |
| `bg_full_light`、`bg_full_dark` | *(空白)* | 整頁背景圖片 |
| `bg_invert_color` | *(空白)* | 以逗號分隔的目標，其缺少的模式版本會以反轉另一個版本自動產生：`home`、`boards`、`files`、`toplevel`、`logo` |
| `bg_invert_algo` | `luminv` | 反轉演算法：`luminv` 或 `oklch` |

### `fonts.settings`

字體排版覆寫：`font_body`、`font_heading`、`font_ui`、`font_code`、`font_blockquote`、`font_display`、`font_import_url`、`font_face_family`、`font_face_src`，以及各元素的 `letter_spacing_*` 與 `font_weight_*` 值。首次執行時會寫出預設值，因此可開啟產生的檔案查看所有鍵。

### `s3.settings`（選用）

S3 相容物件儲存，用於存放上傳的檔案（AWS S3、MinIO、R2、B2）。完全選用——留空時，檔案會保留在本機磁碟的 `public/uploads/` 下。支援 `mode=mirror`（保留本機副本並備份至 S3）與 `mode=offload`（移至 S3，透過預先簽署的重導向提供下載）。完整參考與設定範例：[S3.md](S3.md)。

### 環境變數

**核心**

| 變數 | 預設值 | 說明 |
|----------|---------|-------------|
| `BLOG_ROOT` | *(未設定)* | 專案根目錄；當執行檔在專案根目錄以外啟動時使用。否則會自動偵測包含 `public/` 的目錄 |
| `DEBUG` | *(關閉)* | `1`/`true`/`yes` 啟用 DEBUG/INFO 日誌；否則只輸出警告/錯誤 |
| `NATS_URL` | *(未設定)* | 例如 `nats://localhost:4222` —— 啟用 NATS 訊息閘道 |
| `BLOG_ECH_KEY` / `BLOG_ECH_DIR` | *(未設定)* | ECH（Encrypted Client Hello）金鑰檔案 / 金鑰目錄 |
| `CWIST_C1M_MODE` | `1` | 事件驅動的 C1M reactor。設為 `0` 可強制使用舊式執行緒池路徑 |

**效能 / 快取**

| 變數 | 預設值 | 說明 |
|----------|---------|-------------|
| `FLYBOARD_CACHE_MAX_MB` | `64` | 頁面快取大小（MB，1–1024） |
| `FLYBOARD_ADVERTISE_H3` | `true` | 發送宣告 HTTP/3 的 `Alt-Svc` 標頭 |
| `FLYBOARD_ALT_SVC_MAX_AGE` | `300` | `Alt-Svc` 的 `ma` 值（秒，0–86400） |
| `FLYBOARD_INLINE_IMAGES` | *(關閉)* | 將圖片以 base64 data URI 內嵌於 HTML |
| `FLYBOARD_INLINE_ALL_ASSETS` | *(關閉)* | 同時內嵌腳本/樣式 |
| `FLYBOARD_INLINE_BG_IMAGES` | *(關閉)* | 同時內嵌背景圖片（即使啟用 `ALL_ASSETS` 仍需明確指定） |
| `FLYBOARD_INLINE_MAX_IMAGE_SIZE` | `49152` | 每張內嵌圖片的最大位元組數 |
| `FLYBOARD_INLINE_MAX_ASSET_SIZE` | `65536` | 每個內嵌腳本/樣式區塊的最大位元組數 |
| `FLY_MEDIA_MAX_CONCURRENT` | `2` | 媒體預覽的併發 ffmpeg 轉換數 |
| `FLYBOARD_MEDIA_BACKFILL_ON_START` | *(關閉)* | 啟動時重新產生所有舊版媒體預覽（僅限維護用途） |

**自動 TLS 憑證續期**（使用本機 ACME 客戶端；會偵測到 `keygen.sh` 產生的臨時自簽憑證，且絕不更動）

| 變數 | 預設值 | 說明 |
|----------|---------|-------------|
| `FLY_CERT_RENEWAL` | *(關閉)* | `true` 啟用每日到期檢查。當憑證剩餘天數 ≤ `FLY_CERT_DAYS` 時續期，並在不重新啟動的情況下熱載入 |
| `FLY_CERT_DAYS` | `30` | 續期閾值（天） |
| `FLY_CERT_EMAIL` | `admin@<host>` | ACME 帳戶電子郵件 |
| `FLY_CERT_LEGO_BIN` | `lego` | lego 執行檔名稱/路徑（可指向包裝腳本以使用 DNS 挑戰等） |

Watchdog 會從 `root_url` 推導網域，並以 HTTP-01 挑戰執行 lego，因此機器的 80 埠必須可從外部連線。狀態存放於 `.lego/` 之下；續期後的憑證會安裝覆蓋 `server.crt`/`server.key`。

**電子郵件驗證註冊**（預設關閉 = 開放註冊）

| 變數 | 預設值 | 說明 |
|----------|---------|-------------|
| `FLY_EMAIL_CERT` | *(關閉)* | `true` 時要求新註冊者先驗證電子郵件才能登入。系統會透過 SMTP 寄出 24 小時有效的 token 連結 |
| `FLY_SMTP_HOST` | *(啟用時必填)* | SMTP 中繼主機 |
| `FLY_SMTP_PORT` | `25`（隱含 TLS 時為 `465`） | SMTP 埠號 |
| `FLY_SMTP_TLS` | *(關閉)* | `starttls` 或 `implicit` |
| `FLY_SMTP_USER` / `FLY_SMTP_PASS` | *(未設定)* | AUTH LOGIN 憑證（可選） |
| `FLY_SMTP_FROM` | `FLY_SMTP_USER` | 信封/標頭寄件者 |

範例 —— 啟用驗證註冊與自動憑證續期的正式環境：

```sh
FLY_CERT_RENEWAL=true FLY_CERT_EMAIL=admin@example.com \
FLY_EMAIL_CERT=true FLY_SMTP_HOST=smtp.example.com FLY_SMTP_PORT=587 \
FLY_SMTP_TLS=starttls FLY_SMTP_USER=noreply@example.com FLY_SMTP_PASS=secret \
./fly_board
```

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

### 這些基準測試測量什麼

測量的是三件不同的事，本節的舊版本把它們混為一談：

- **併發連線數**（傳統 C10K/C1M 的含義）：伺服器同時保持開啟並提供服務的連線數量。透過 `run_c1m_held_bench.sh` 使用 `tools/connhold` 測量。
- **保持連線上的請求處理（churn）**：`h2load` 測試組（`run_c10k_bench.sh`、`run_c100k_bench.sh`、`run_c1m_bench.sh`）。`h2load` 帶 `-r`（速率限制）執行，因此 RPS 反映的是設定的負載，而不是吞吐量上限。`run_c1m_bench.sh` 是 10 萬併發連線、100 萬次請求，名稱沿用歷史。
- **吞吐量**：對首頁進行不限速的 `h2load` 與 `wrk` 測試。

### 主機環境

| 項目 | 值 |
|------|-------|
| OS | Linux 6.12.107+deb13-amd64 (Debian 13) |
| CPU | AMD Ryzen 5 5600X（6 核 / 12 執行緒） |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| 負載產生器 | h2load nghttp2/1.64.0、wrk、`tools/connhold`（BoringSSL） |
| CWIST | `main` `11f3518d`（2026-09-29，已包含在 v3.7.1 中） |
| TLS 憑證 | ECDSA P-256（`keygen.sh` 預設） |
| 服務模式 | `CWIST_C1M_MODE=1`（事件驅動 reactor） |

用戶端與伺服器在同一台主機上執行。

### 系統調校

| 參數 | 值 |
|-----------|-------|
| ulimit -n | 1,050,000 |
| fs.file-max | 8,388,608（100 萬連線在用戶端和伺服器端共需 200 萬個 fd） |
| fs.nr_open | 1,050,000 |
| net.netfilter.nf_conntrack_max | 4,194,304（loopback 連線也會被追蹤） |
| net.core.somaxconn | 1,050,000 |
| net.ipv4.tcp_max_syn_backlog | 1,050,000 |
| net.ipv4.ip_local_port_range | 1024 65535 |
| vm.max_map_count | 1,048,576 |
| kernel.pid_max | 4,194,304 |
| CPU governor | ecodemand |

### C1M：100 萬併發連線（2026-09-29）

`run_c1m_held_bench.sh`：12 個 worker，100 萬個連線分散在 48 個 loopback 位址上。每個連線送出 `GET /robots.txt`，之後每 60–120 秒重送一次，使 keep-alive 計時器不會到期（HTTP/2 每次使用新的串流）。失敗的連線只計數，不重試。

| | 明文 HTTP/1.1 | TLS 1.3 + HTTP/1.1 | TLS 1.3 + HTTP/2 |
|---|---|---|---|
| 開啟的連線 | 1,000,000 | 1,000,000 | 1,000,000 |
| 連線速率 | 每秒 40,000 | 每秒 8,000 | 每秒 8,000 |
| 峰值保持數 | **1,000,000** | **1,000,000** | **1,000,000** |
| 同時被服務的峰值 | **1,000,000** | **1,000,000** | **1,000,000** |
| 回應數（含 keep-alive GET） | 2,124,089 | 2,632,665 | 2,052,855 |
| 失敗的連線 | 0 | 0 | 0 |
| 峰值時伺服器記憶體（PSS，全部 worker） | 24.0 GB | 19.9 GB | 30.1 GB |

- **三種模式都保持並服務了 100 萬個併發連線，零失敗。**
- TLS 需要 CWIST v3.7.1 或 `11f3518d` 之後的 `main`。在此之前，每個閒置 TLS 連線都佔著一個池執行緒等待，因此同時被服務的 TLS 連線只有池執行緒那麼多：同一次執行中保持了 395,729 個 TLS 連線，但同時被服務的最多只有 25 個。v3.7.1 把閒置 TLS 連線停放到 epoll 集合中，資料到達時再交回執行緒池。
- TLS 連線速率受完整交握限制：每秒 20,000 個新連線時，約 37% 的交握超出 45 秒交握預算（被服務的連線 HTTP/1.1 為 523,654 個，HTTP/2 為 510,023 個）；每秒 8,000 個時沒有超出。
- 負載構造陷阱：Linux 的 `connect()` 先分配偶數暫時埠，用完後轉入緩慢的奇數埠搜尋，因此每個目標位址只能快速建立約 3.2 萬個連線。使用 24 個位址時每次都卡在約 77.4 萬；請至少使用 `連線數 / 32,000` 個位址。

### 記憶體

所有伺服器行程（master 與 fork 出的 worker）的 PSS 總和。

| 情境 | 伺服器合計 | 每連線 |
|---|---|---|
| 閒置，1 個 worker | 115 MB | — |
| 閒置，4 個 worker | 406 MB | — |
| 10 萬個 TLS/HTTP/2 連線（h2load C100k） | 1.77 GB（暖機後閒置 1.11 GB） | 約 6.7 KB |
| 100 萬個明文 HTTP/1.1 連線（connhold） | 24.0 GB | 約 24 KB |
| 100 萬個 TLS HTTP/1.1 連線（connhold） | 19.9 GB | 約 20 KB |
| 100 萬個 TLS HTTP/2 連線（connhold） | 30.1 GB | 約 30 KB |

同機執行時的用戶端開銷：h2load 每連線約 60 KB，connhold 約 0.06 KB，另加核心 socket 記憶體（C100k 時每對連線約 12.7 KB）。

> **更正：** 本 README 的舊版本稱閒置約 102–108 MB、從 C10k 到 C1m 約 110–146 MB。這些數值是 `/usr/bin/time -v` 測得的 master 行程單獨的最大 RSS；`cwist_app_listen()` 會 fork 出服務 worker，而它們的記憶體從未被計入。以上合計取代了舊數值。

### h2load 測試組：請求處理（2026-09-29，CWIST `11f3518d`）

| 測試 | 併發連線 | 請求 | 成功 | 耗時 | RPS（各行程之和） |
|---|---|---|---|---|---|
| C10k（4 個 worker） | 10,000 | 20,000 | **100%** | 8.50 秒 | 9,253 |
| C100k（12 個 worker） | 100,000 | 200,000 | **100%** | 29.83 秒 | 8,958 |
| C1m churn（12 個 worker） | 100,000 | 1,000,000 | **100%** | 57.28 秒 | 19,754 |

耗時是伺服器行程的生命週期（包括啟動和 5 秒關閉 drain）。回應是完整的 79 KB 首頁；h2load 不請求壓縮。

同一天較早時使用 RSA-4096 憑證，C100k 降到 72.6%，C1m churn 降到 65.2%：幾乎所有忙碌的 CPU 都花在每次 TLS 1.3 完整交握的 RSA CertificateVerify 簽章上，排隊的交握因此觸及 45 秒預算。改用 ECDSA P-256（`keygen.sh` 預設）、在請求 worker 上執行路由處理器、並用路由 Big Dumb Reply 快取提供匿名公開頁面後，恢復到 100%。

### 吞吐量基準測試（2026-09-29，CWIST `11f3518d`）

對首頁的不限速負載（無 `-r`），12 個 worker。

| 工具 | 命令 | 結果 |
|---|---|---|
| h2load（HTTP/2） | `h2load -c512 -n100000 https://127.0.0.1:8888/` | **每秒 12,337 次**，970.71 MB/s，100,000/100,000 成功，8.11 秒；請求時間 最小 410 µs，平均 20.69 ms，最大 205.85 ms |
| wrk（HTTP/1.1） | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` | **每秒 48,829 次**，3.76 GB/s；延遲 平均 9.50 ms，標準差 4.15 ms，最大 132.20 ms；無 socket 錯誤 |

兩個工具協定不同，不能直接比較。2026-08 時同樣的命令結果為 h2load 每秒 7,167 次、wrk 每秒 1,282 次（讀取錯誤 77,027 次）；現在匿名首頁請求由路由 Big Dumb Reply 快取回應。

**重點**

- **C1M：** 在一台桌上型等級主機上，明文 HTTP/1.1、TLS HTTP/1.1 與 TLS HTTP/2 都保持並服務了 100 萬個併發連線，零失敗。
- **每連線記憶體是真實開銷：** 伺服器端每個保持的連線約 20–30 KB；請據此規劃記憶體。
- **TLS 連線速率受交握限制：** 請使用 ECDSA 憑證；在這台主機上每秒約 8,000 次完整交握無一遺漏。
