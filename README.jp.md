# fly.board

![fly.board logo](img/logo.png)

> デスクトップ級ホスト 1 台で**同時接続 100 万**を保持し（平文 HTTP/1.1、TLS HTTP/1.1、TLS HTTP/2 で実測）、C10k/C100k の TLS 負荷テストを成功率 100% で通過する、数少ないシンプルなブログエンジンの一つ。
> C 言語製 CWIST Web フレームワークをベースに、HTTPS/3、Argon2id、PQC 署名、NATS メッセージングをサポートする軽量な掲示板＆ブログエンジン。

## 機能

- **接続スケーラビリティ** – cwist のイベント駆動 reactor 上のスタック＋ヒープ C 実装。平文 HTTP/1.1、TLS HTTP/1.1、TLS HTTP/2 の実測で**同時接続 100 万**を保持・処理。未ログインの公開ページは Big Dumb Reply キャッシュから返す。
- **最新トランスポート** – デフォルトで TLS 1.3 + HTTP/3（QUIC）。オプションで ECH（Encrypted Client Hello）も利用可能。
- **安全な認証** – クライアント側 SHA-512 プリハッシュ + サーバー側 **Argon2id**（OpenSSL 3 KDF）。JWT セッション Cookie。
- **掲示板 / ブログ ハイブリッド** – Slug ベースの Markdown 投稿 + 複数掲示板 + 入れ子コメント。
- **リアルタイムプレビュー** – Markdown エディタから即座にサーバー側でプレビューをレンダリング。
- **PQC 署名** – 投稿にポスト量子暗号（PQC）ベースの署名を付与・検証。
- **ファイルストレージ** – ≤1 MB は SQLite に、それ以上はボリュームに保存。画像・動画・音声を自動埋め込み。
- **NATS 統合** – 環境変数 `NATS_URL` による分散メッセージングゲートウェイ。
- **ダークモード** – Cookie ベースのテーマ切り替え + 動的 CSS 変数。

## ビルド

```sh
make
./keygen.sh
```

依存関係:
- [CWIST](https://github.com/religiya-serdtsa/cwist) — TLS 1.3 / HTTP/3（QUIC）は CWIST に組み込まれた BoringSSL で処理されます。別途インストールは不要です。
- OpenSSL 3.x（Argon2id KDF）
- ngtcp2 / nghttp3（HTTP/3）
- cJSON、SQLite3

`Makefile` は `third_party/md4c` をクローンし、静的ライブラリとしてビルドします。

## 実行

```sh
./fly_board
```

デフォルトのポートは `blog.settings` 内の `port` 値に従います（デフォルトは 9443）。

```text
https://localhost:9443
```

HTTP/3 は同一ポートの UDP でリッスンします。

### ECH の有効化（オプション）

```sh
BLOG_ECH_KEY=ech/server.ech ./fly_board
# または
BLOG_ECH_DIR=ech ./fly_board
```

OpenSSL のビルドが ECH をサポートしていない場合、警告ログが出力された上で通常の HTTPS/3 で継続します。

### NATS 統合（オプション）

```sh
NATS_URL=nats://localhost:4222 ./fly_board
```

## 主な機能

| 機能 | パス | 説明 |
|---------|------|-------------|
| ホーム | `/` | 最新投稿一覧 |
| 掲示板 | `/boards` | 複数掲示板の管理（管理者専用サポート） |
| 投稿 | `/post/:slug` | md4c Markdown レンダリング + コメント + 添付ファイル |
| ログイン/登録 | `/login`、`/register` | Argon2id + JWT Cookie |
| プロフィール | `/profile` | ニックネーム、自己紹介、プロフィール画像、参加日 |
| アカウント設定 | `/account/settings` | プロフィール編集 |
| パスワード変更 | `/account/password` | 現在のパスワードを検証し、Argon2id で再ハッシュ |
| 管理画面 | `/admin/users` | ユーザーロールの変更、ユーザーの削除 |
| ファイルストレージ | `/files` | アップロード/ダウンロード/削除 |

## 設定

設定は 3 つのファイル（初回起動時にデフォルトで自動生成されます）と、運用切り替え用の環境変数から読み込まれます。

### `admin.settings`

2 行のプレーンテキスト: 1 行目は管理者ユーザー名、2 行目は管理者パスワードです。

### `blog.settings`

`key=value` 形式のプレーンな行です。不明なキーは無視され、無効な値はデフォルトにフォールバックします。

| キー | デフォルト | 値 / スコープ |
|-----|---------|----------------|
| `title` | `CWIST Docker Blog` | トップバーに表示されるサイトタイトル |
| `subtitle` | `Explore boards and read stories.` | ヒーロー部分のサブタイトル |
| `brand_footer` | `Built with CWIST C Framework` | フッターテキスト |
| `root_url` | `https://localhost:8888/` | サイトの正規 URL（末尾に `/`）。RSS リンク、確認メール、証明書更新に使用 — 本番環境では公開 URL を設定してください |
| `port` | `8443` | TCP/UDP リッスンポート（HTTP/3 は同じポートを UDP で使用） |
| `accent` | `#3b82f6` | アクセントカラー（hex） |
| `use_tls` | `true` | `true`/`false` — HTTPS のオン/オフ（先に `./keygen.sh` を実行してください） |
| `use_http2` | `true` | TLS 上の HTTP/2 |
| `use_http3` | `true` | UDP 上の HTTP/3（QUIC） |
| `use_tasfa` | `true` | TASFA メディアパイプライン（ffmpeg による動画サムネイル/プレビュー） |
| `use_rss` | `false` | `/rss.xml` を公開 |
| `roundness` | `0.0` | UI の角丸、`0.0`–`1.0` |
| `max_upload_size` | `1G` | ファイルごとのアップロード上限。サフィックス `K/M/G/T` が使用可能（例: `500M`） |
| `max_total_parallel_uploads` | `8` | 全体の同時アップロード数（1–512） |
| `max_upload_parallel_chunks` | `32` | アップロードごとの並列チャンク数（1–64） |
| `max_concurrent_downloads` | `128` | 同時ダウンロード数（1–512） |
| `vote_only` | *（空 = `all`）* | 投稿に投票できるユーザー: `all`（匿名含む全員）、`authorized`（ログインユーザーのみ）、`admin`（管理者のみ） |
| `use_special_modes` | *（空）* | ライト/ダークテーマを置き換え: `lightTheme,darkTheme`（または単一テーマ）。利用可能なテーマ: `light`、`dark`、`ocean`、`forest`、`sepia`。例: `ocean,forest` |
| `home_img`、`boards_img`、`files_img` | *（空）* | ページごとのヒーロー/背景画像。`public/img/` 内のファイル名 |
| `*_dark`（`home_img_dark`、`boards_img_dark`、`files_img_dark`） | *（空）* | 上記のダークモード用バリアント |
| `blog_logo`、`blog_logo_dark` | *（空）* | `public/img/` 内のロゴ画像 |
| `invert_logo` | `false` | 画像がないモード向けにロゴを自動反転 |
| `favicon` | *（空）* | `public/img/` 内のファビコンファイル |
| `bg_full_light`、`bg_full_dark` | *（空）* | ページ全体の背景画像 |
| `bg_invert_color` | *（空）* | 不足しているモード側バリアントを、もう一方を反転して自動生成する対象（カンマ区切り）: `home`、`boards`、`files`、`toplevel`、`logo` |
| `bg_invert_algo` | `luminv` | 反転アルゴリズム: `luminv` または `oklch` |

### `fonts.settings`

タイポグラフィのオーバーライド: `font_body`、`font_heading`、`font_ui`、`font_code`、`font_blockquote`、`font_display`、`font_import_url`、`font_face_family`、`font_face_src`、および要素ごとの `letter_spacing_*` と `font_weight_*` の値。初回起動時にデフォルトが書き出されるので、生成されたファイルを開いて全キーを確認してください。

### `s3.settings`（オプション）

アップロードされたファイル向けの S3 互換オブジェクトストレージ（AWS S3、MinIO、R2、B2）。完全にオプションで、空のままにしておけばファイルは `public/uploads/` 配下のローカルディスクに保存されます。`mode=mirror`（ローカルコピーを保持しつつ S3 にバックアップ）と `mode=offload`（S3 に移動し、プリサインドリダイレクトでダウンロードを配信）をサポートします。完全なリファレンスと設定例は [S3.md](S3.md) を参照してください。

### 環境変数

**コア**

| 変数 | デフォルト | 説明 |
|----------|---------|-------------|
| `BLOG_ROOT` | *（未設定）* | プロジェクトルート。バイナリがその外部から起動された場合に使用。それ以外では `public/` を含むディレクトリが自動検出されます |
| `DEBUG` | *（オフ）* | `1`/`true`/`yes` で DEBUG/INFO ログを有効化。それ以外は警告/エラーのみ出力 |
| `NATS_URL` | *（未設定）* | 例: `nats://localhost:4222` — NATS メッセージングゲートウェイを有効化 |
| `BLOG_ECH_KEY` / `BLOG_ECH_DIR` | *（未設定）* | ECH（Encrypted Client Hello）キーファイル / キーディレクトリ |
| `CWIST_C1M_MODE` | `1` | イベント駆動 C1M リアクター。`0` に設定すると従来のスレッドプール経路を強制 |

**パフォーマンス / キャッシュ**

| 変数 | デフォルト | 説明 |
|----------|---------|-------------|
| `FLYBOARD_CACHE_MAX_MB` | `64` | ページキャッシュサイズ（MB、1–1024） |
| `FLYBOARD_ADVERTISE_H3` | `true` | HTTP/3 を告知する `Alt-Svc` ヘッダーを送信 |
| `FLYBOARD_ALT_SVC_MAX_AGE` | `300` | `Alt-Svc` の `ma` 値（秒、0–86400） |
| `FLYBOARD_INLINE_IMAGES` | *（オフ）* | 画像を base64 データ URI として HTML にインライン化 |
| `FLYBOARD_INLINE_ALL_ASSETS` | *（オフ）* | スクリプト/スタイルもインライン化 |
| `FLYBOARD_INLINE_BG_IMAGES` | *（オフ）* | 背景画像もインライン化（`ALL_ASSETS` でも明示的なオプトインが必要） |
| `FLYBOARD_INLINE_MAX_IMAGE_SIZE` | `49152` | インライン化する画像ごとの最大バイト数 |
| `FLYBOARD_INLINE_MAX_ASSET_SIZE` | `65536` | インライン化するスクリプト/スタイルチャンクごとの最大バイト数 |
| `FLY_MEDIA_MAX_CONCURRENT` | `2` | メディアプレビュー用の同時 ffmpeg 変換数 |
| `FLYBOARD_MEDIA_BACKFILL_ON_START` | *（オフ）* | 起動時にすべての従来メディアプレビューを再生成（メンテナンス実行専用） |

**TLS 証明書の自動更新**（ローカル ACME クライアントを使用。`keygen.sh` で作成した一時的な自己署名証明書は検出され、一切変更されません）

| 変数 | デフォルト | 説明 |
|----------|---------|-------------|
| `FLY_CERT_RENEWAL` | *（オフ）* | `true` で日次の期限ウォッチドッグを有効化。証明書の残り日数が `FLY_CERT_DAYS` 日以下になると更新し、再起動なしでホットリロード |
| `FLY_CERT_DAYS` | `30` | 更新しきい値（日数） |
| `FLY_CERT_EMAIL` | `admin@<host>` | ACME アカウントのメールアドレス |
| `FLY_CERT_LEGO_BIN` | `lego` | lego バイナリ名/パス（DNS チャレンジなどにはラッパースクリプトを指定） |

ウォッチドッグは `root_url` からドメインを導出し、HTTP-01 チャレンジで lego を実行するため、マシンにポート 80 へ到達できる必要があります。状態は `.lego/` 以下に保存され、更新された証明書は `server.crt`/`server.key` に上書きインストールされます。

**メール確認付きサインアップ**（デフォルトはオフ = 誰でも登録可能）

| 変数 | デフォルト | 説明 |
|----------|---------|-------------|
| `FLY_EMAIL_CERT` | *（オフ）* | `true` で新規登録時にログイン前のメール確認を必須化。24 時間有効なトークンリンクが SMTP で送信されます |
| `FLY_SMTP_HOST` | *（有効時は必須）* | SMTP リレーホスト |
| `FLY_SMTP_PORT` | `25`（暗黙的 TLS の場合は `465`） | SMTP ポート |
| `FLY_SMTP_TLS` | *（オフ）* | `starttls` または `implicit` |
| `FLY_SMTP_USER` / `FLY_SMTP_PASS` | *（未設定）* | AUTH LOGIN 認証情報（オプション） |
| `FLY_SMTP_FROM` | `FLY_SMTP_USER` | エンベロープ/ヘッダーの送信者 |

例 — メール確認付きサインアップと証明書自動更新を有効にした本番環境:

```sh
FLY_CERT_RENEWAL=true FLY_CERT_EMAIL=admin@example.com \
FLY_EMAIL_CERT=true FLY_SMTP_HOST=smtp.example.com FLY_SMTP_PORT=587 \
FLY_SMTP_TLS=starttls FLY_SMTP_USER=noreply@example.com FLY_SMTP_PASS=secret \
./fly_board
```

## データベース

SQLite3（`data/blog.db`）。アプリケーション起動時にスキーマが自動マイグレーションされます。

```
users       – アカウント、Argon2id ハッシュ、ロール、プロフィール
boards      – 掲示板名/Slug/説明/admin_only
posts       – Markdown 本文、PQC 署名、要約
files       – 添付ファイルのパス/サイズ/MIME
comments    – 入れ子コメント（target_type, parent_id）
board_permissions – プライベート掲示板のアクセス権限
```

## アーキテクチャ

```
CWIST（HTTP/3, TLS 1.3）
  ├── src/auth/     – Argon2id、JWT、セッション
  ├── src/db/       – SQLite3 CRUD
  ├── src/handlers/ – ルーティング/ビジネスロジック
  ├── src/render/   – cwist_html_element SSR + md4c
  ├── src/crypto/   – PQC 署名/検証
  └── src/nats/     – メッセージング Pub/Sub
```

## ライセンス

MIT License

---

## スケーラビリティベンチマーク

### このベンチマークが測るもの

異なる 3 つのものを測ります。このセクションの以前の版はこれらを混同していました。

- **同時接続数**（伝統的な C10K/C1M の意味）: サーバーが同時に開いたまま処理している接続の数。`run_c1m_held_bench.sh` から `tools/connhold` で測定します。
- **保持された接続上のリクエスト処理（churn）**: `h2load` スイート（`run_c10k_bench.sh`、`run_c100k_bench.sh`、`run_c1m_bench.sh`）。`h2load` は `-r`（レート制限）付きで動くため、RPS は設定した負荷を反映するだけでスループットの上限ではありません。`run_c1m_bench.sh` は同時接続 10 万・リクエスト 100 万件で、名前は歴史的なものです。
- **スループット**: トップページに対する無制限の `h2load` と `wrk` の実行。

### ホスト環境

| 項目 | 値 |
|------|-------|
| OS | Linux 6.12.107+deb13-amd64 (Debian 13) |
| CPU | AMD Ryzen 5 5600X（6 コア / 12 スレッド） |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| 負荷生成 | h2load nghttp2/1.64.0、wrk、`tools/connhold`（BoringSSL） |
| CWIST | `main` `11f3518d`（2026-09-29。同じ TLS 修正は v3.7.1 としてもリリース済みだが、fly.board は `_ex` ルート API のため `main` が必要） |
| TLS 証明書 | ECDSA P-256（`keygen.sh` のデフォルト） |
| 動作モード | `CWIST_C1M_MODE=1`（イベント駆動 reactor） |

クライアントとサーバーは同一ホストで動作しました。

### システムチューニング

| パラメータ | 値 |
|-----------|-------|
| ulimit -n | 1,050,000 |
| fs.file-max | 8,388,608（100 万接続にはクライアントとサーバー合わせて 200 万 fd が必要） |
| fs.nr_open | 1,050,000 |
| net.netfilter.nf_conntrack_max | 4,194,304（loopback 接続も追跡される） |
| net.core.somaxconn | 1,050,000 |
| net.ipv4.tcp_max_syn_backlog | 1,050,000 |
| net.ipv4.ip_local_port_range | 1024 65535 |
| vm.max_map_count | 1,048,576 |
| kernel.pid_max | 4,194,304 |
| CPU governor | ecodemand |

### C1M: 同時接続 100 万（2026-09-29）

`run_c1m_held_bench.sh`: ワーカー 12、100 万接続を 48 個の loopback アドレスに分散。各接続は `GET /robots.txt` を送り、keep-alive タイマーが切れないよう 60–120 秒ごとに再送します（HTTP/2 は毎回新しいストリーム）。失敗した接続は再試行せずそのまま数えます。

| | 平文 HTTP/1.1 | TLS 1.3 + HTTP/1.1 | TLS 1.3 + HTTP/2 |
|---|---|---|---|
| 開いた接続 | 1,000,000 | 1,000,000 | 1,000,000 |
| 接続レート | 毎秒 40,000 | 毎秒 8,000 | 毎秒 8,000 |
| 最大保持数 | **1,000,000** | **1,000,000** | **1,000,000** |
| 同時に処理された最大数 | **1,000,000** | **1,000,000** | **1,000,000** |
| 応答数（keep-alive GET を含む） | 2,124,089 | 2,632,665 | 2,052,855 |
| 失敗した接続 | 0 | 0 | 0 |
| ピーク時のサーバーメモリ（PSS、全ワーカー） | 24.0 GB | 19.9 GB | 30.1 GB |

- **3 つのモードすべてで同時接続 100 万を保持・処理し、失敗は 0。**
- TLS には CWIST v3.7.1 または `11f3518d` 以降の `main` が必要です。それ以前はアイドルな TLS 接続ごとにプールスレッドが 1 つ待っていたため、同時に処理される TLS 接続はプールスレッド数分だけでした。同じ実行で TLS 接続 395,729 本が保持されましたが、同時に処理されたのは最大 25 本でした。v3.7.1 はアイドルな TLS 接続を epoll 集合に駐車し、データが届くとプールに戻します。
- TLS の接続レートはフルハンドシェイクが制限します。毎秒 20,000 接続ではハンドシェイクの約 37% が 45 秒の予算を超え（処理された接続は HTTP/1.1 523,654 本、HTTP/2 510,023 本）、毎秒 8,000 では 1 つも超えませんでした。
- 負荷構成の注意: Linux の `connect()` は偶数のエフェメラルポートを先に使い、尽きると遅い奇数ポート探索に切り替わります。そのため宛先アドレス 1 つで速く開ける接続は約 3.2 万です。アドレス 24 個では毎回約 77.4 万で止まりました。アドレスは `接続数 / 32,000` 個以上にしてください。

### メモリ

すべてのサーバープロセス（マスターと fork されたワーカー）の PSS 合計。

| ケース | サーバー合計 | 1 接続あたり |
|---|---|---|
| アイドル、ワーカー 1 | 115 MB | — |
| アイドル、ワーカー 4 | 406 MB | — |
| TLS/HTTP/2 接続 10 万（h2load C100k） | 1.77 GB（ウォームアップ後アイドル 1.11 GB） | 約 6.7 KB |
| 平文 HTTP/1.1 接続 100 万（connhold） | 24.0 GB | 約 24 KB |
| TLS HTTP/1.1 接続 100 万（connhold） | 19.9 GB | 約 20 KB |
| TLS HTTP/2 接続 100 万（connhold） | 30.1 GB | 約 30 KB |

同一ホストで実行する際のクライアント側コスト: h2load は 1 接続あたり約 60 KB、connhold は約 0.06 KB にカーネルのソケットメモリ（C100k で接続ペアあたり約 12.7 KB）が加わります。

> **訂正:** この README の以前の版は、アイドル時約 102–108 MB、C10k から C1m まで約 110–146 MB と書いていました。その値は `/usr/bin/time -v` が測ったマスタープロセス 1 つの最大 RSS でした。`cwist_app_listen()` は処理ワーカーを fork しますが、そのメモリは一度も数えられていませんでした。上の合計がそれに代わります。

### h2load スイート: リクエスト処理（2026-09-29、CWIST `11f3518d`）

| テスト | 同時接続 | リクエスト | 成功 | 所要時間 | RPS（プロセス合計） |
|---|---|---|---|---|---|
| C10k（ワーカー 4） | 10,000 | 20,000 | **100%** | 8.50 秒 | 9,253 |
| C100k（ワーカー 12） | 100,000 | 200,000 | **100%** | 29.83 秒 | 8,958 |
| C1m churn（ワーカー 12） | 100,000 | 1,000,000 | **100%** | 57.28 秒 | 19,754 |

所要時間はサーバープロセスの寿命です（起動と 5 秒の終了 drain を含む）。応答は 79 KB のトップページ全体で、h2load は圧縮を要求しません。

同じ日の先の実行では、RSA-4096 証明書で C100k が 72.6%、C1m churn が 65.2% に落ちました。忙しい CPU のほぼすべてが TLS 1.3 フルハンドシェイクごとの RSA CertificateVerify 署名に使われ、待機中のハンドシェイクが 45 秒予算に達したためです。ECDSA P-256（`keygen.sh` のデフォルト）への切り替え、ルートハンドラのリクエストワーカー実行、未ログインの公開ページをルート Big Dumb Reply キャッシュから返すことで 100% に戻りました。

### スループットベンチマーク（2026-09-29、CWIST `11f3518d`）

トップページに対する無制限の負荷（`-r` なし）、ワーカー 12。

| ツール | コマンド | 結果 |
|---|---|---|
| h2load（HTTP/2） | `h2load -c512 -n100000 https://127.0.0.1:8888/` | **毎秒 12,337 件**、970.71 MB/s、100,000/100,000 成功、8.11 秒。リクエスト時間 最小 410 µs、平均 20.69 ms、最大 205.85 ms |
| wrk（HTTP/1.1） | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` | **毎秒 48,829 件**、3.76 GB/s。レイテンシ 平均 9.50 ms、標準偏差 4.15 ms、最大 132.20 ms。ソケットエラーなし |

2 つのツールはプロトコルが異なり、直接比較はできません。2026-08 には同じコマンドで h2load 毎秒 7,167 件、wrk 毎秒 1,282 件（読み取りエラー 77,027 件）でした。現在は未ログインのトップページ要求がルート Big Dumb Reply キャッシュから応答されます。

**要点**

- **C1M:** デスクトップ級ホスト 1 台で、平文 HTTP/1.1、TLS HTTP/1.1、TLS HTTP/2 のいずれでも同時接続 100 万を保持・処理し、失敗は 0。
- **1 接続あたりのメモリは実際にかかる:** 保持された接続 1 本あたりサーバー側で約 20–30 KB。RAM はそれに合わせて見積もってください。
- **TLS の接続レートはハンドシェイクが制限する:** ECDSA 証明書を使ってください。このホストでは毎秒約 8,000 回のフルハンドシェイクが取りこぼしなく完了しました。
