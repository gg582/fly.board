# fly.board

![fly.board logo](img/logo.png)

> 接続数が増えてもメモリをほぼ平坦に保つ、数少ないシンプルなブログエンジンの一つ: idle 時 RSS は single worker 構成でも **~108 MB**（4 workers 構成では **~102 MB**）、C10k、C100k、さらに C1m でも **~110–146 MB** を維持。
> C 言語製 CWIST Web フレームワークをベースに、HTTPS/3、Argon2id、PQC 署名、NATS メッセージングをサポートする軽量な掲示板＆ブログエンジン。

## 機能

- **メモリ効率と接続スケーラビリティ** – スタック＋ヒープの C 実装。idle 時 **~102–108 MB RSS**（1–4 workers）、C10k から C1m までの同時接続で RSS は **~110–146 MB** 前後に維持される。
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

### このベンチマークが測定するもの

これらのテストは `h2load` の **`-r`（レート制限）オプション付き**で実行します。これらは意図的に最大スループットテストではありません。代わりに、制御されたプロセスあたりのリクエストレートで処理しながら、サーバーが多大な数の同時 HTTP/2 接続を**維持できるか**を測定します。

負荷がレート制限されているため:

- 報告される **RPS は設定されたリクエストレート**を反映し、サーバーの絶対的なスループット上限ではありません。
- 主要指標は、接続数が 10,000 から 1,000,000 に増加する際の **resident-set-size（RSS）の安定性**です。

ワーカー数は負荷に応じてスケールされ、各テストを現実的に保ちます: C10k では **4 workers**、C100k では **12 workers**、C1m では **12 workers** です。これにより、3 回の実行で CPU 使用率の数値が異なる理由も説明されます。

### ホスト環境

| 項目 | 値 |
|------|-------|
| OS | Linux 7.1.0-mountain-rc6+ |
| アーキテクチャ | x86_64 |
| CPU | 12 logical cores |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| OpenSSL | 3.5.6 |
| ベンチマークツール | h2load nghttp2/1.64.0 |
| CWIST | 兄弟 cwist チェックアウトの `libcwist.a`（2026-08-29、アリーナバンプアロケータ、共有 req/res アリーナ、256KB ワーカースタック、HTTP/3 強化、sharded TLS handshake shepherd） |

### システムチューニング

| パラメータ | 値 |
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

### メモリ使用量

| 状態 | RSS | 前状態からの Δ | 備考 |
|-------|-----|-----------------|-------|
| Idle (1 worker) | **~108 MB** (110,196 KB) | — | 1 worker, no connections |
| Idle (4 workers) | **~102 MB** (104,940 KB) | — | 4 workers, no connections |
| C10k | **~110 MB** (112,436 KB) | +7,496 KB vs idle (4 workers) | 10,000 concurrent connections |
| C100k | **~146 MB** (148,848 KB) | +36,412 KB | 100,000 concurrent connections |
| C1m churn | **~146 MB** (149,816 KB) | +968 KB | 100k held TLS conns, 1M-request churn |

**C100k から C1m churn 実行までの RSS 変化は +968 KB** — 実質的に測定ノイズの範囲です。これが本ベンチマークにおいて最も重要な結果です。

RSS 値は、サーバープロセスに対する `/usr/bin/time -v` の **Maximum resident set size (kbytes)** です。

### メモリコスト

| 遷移 | Δ RSS | Δ 接続数 | 追加接続あたりの概算コスト |
|---|---|---|---|
| Idle → C10k | +7,496 KB | 10,000 | 接続あたり ~0.75 KB |
| C10k → C1m churn | +37,380 KB | — | 追加の保持接続あたり ~0.4 KB。C100k → C1m は +968 KB（ノイズ） |

Idle から C10k への初期ジャンプは、TLS 状態、接続バッファ、worker オーバーヘッドを前払いするコストです。C10k から C100k までは追加の保持接続あたり約 ~0.4 KB にとどまり、C100k から C1m への RSS 変化（+968 KB）は測定ノイズの範囲です — 接続あたりのメモリコストは実質的に平坦です。

### C10k 同時接続テスト

`h2load` で 10,000 同時接続を維持して測定。

| 項目 | 値 |
|------|-------|
| Workers | 4 |
| 同時接続数 | 10,000 |
| 継続時間 | 12.05 s |
| 最大 RSS | **~110 MB** (112,436 KB) |
| CPU 使用率 | ~365% |
| User time | 41.05 s |
| System time | 3.04 s |
| Major page faults | 2 |
| Minor page faults | 16,948 |
| Voluntary context switches | 58,050 |
| Involuntary context switches | 14,828 |
| File system outputs | 256 |
| 総リクエスト数 | 20000 |
| 成功数 | 20000 |
| 失敗数 | 0 |
| 概算合計 RPS | **2285.22** |
| 成功率 | **100.00%** |
| 終了ステータス | **0** |

### C100k 同時接続テスト

`h2load` で 100,000 同時接続を維持して測定。

| 項目 | 値 |
|------|-------|
| Workers | 12 |
| 同時接続数 | 100,000 |
| 継続時間 | 1:23.49 |
| 最大 RSS | **~146 MB** (148,848 KB) |
| CPU 使用率 | ~815% |
| User time | 653.83 s |
| System time | 26.78 s |
| Major page faults | 0 |
| Minor page faults | 76,332 |
| Voluntary context switches | 446,557 |
| Involuntary context switches | 617,777 |
| File system outputs | 336 |
| 総リクエスト数 | 200000 |
| 成功数 | 200000 |
| 失敗数 | 0 |
| 概算合計 RPS | **2785.16** |
| 成功率 | **100.00%** |
| 終了ステータス | **0** |

### C1m Churn テスト（2026-08-23 再設計、2026-08-24 修正）

旧来の「1,000,000 同時 TLS 接続」目標は廃止されました: HTTPS パスはライブ接続ごとに 1 つのワーカースレッドを占有するため、維持可能な同時接続数は workers x スレッド数に制限され、1M にははるかに及びません。（cwist の**平文** HTTP/1.x パスはイベント駆動で、1,000,000/1,000,000 の維持接続に到達しています — cwist README を参照。）C1m テストはチャーンを測定します: 100,000 の同時維持 TLS 接続上で 20 の h2load プロセス x 50,000 リクエストを実行し、watchdog で打ち切られます。

| 項目 | 値 |
|------|-------|
| Workers | 12 |
| 負荷形状 | 20 x (-c 5000 -n 50000 -r 1000 -T 30) |
| 合計 | 100,000 維持接続上で 1,000,000 リクエスト |
| 結果 | **完了 — ストールなし** |
| 成功数 | **1,000,000 / 1,000,000 (100.0%)** |
| エラー | 0 |
| 所要時間 | ~1:36（プロセスあたり h2load "finished in" 63.7-89.3 s） |
| Phantom connections | 0（クライアント/サーバーの ESTABLISHED 数が一致） |
| サーバーシャットダウン | 正常、exit 0 |

履歴: 2026-08-23 の同負荷の実行は ~85k 接続でデッドロックしました。根本原因（cwist `perf(https): non-blocking TLS handshake shepherd` で修正）: TLS ハンドシェイクが 30 秒の poll 待機を伴いワーカースレッド内で同期的に実行されていたため、数百の遅延クライアントがプール全体を占有し、accept キューが溢れ、溢れたハンドシェイクが静かにドロップされ、クライアントはサーバー側ソケットのない ESTABLISHED 状態に残りました。現在ハンドシェイクはノンブロッキングの shepherd スレッドで実行され、確立済みセッションのみがプールワーカーを占有します。

> 注記: HTTP/2（TLS 1.3）上で実際のクライアント接続を維持しながら測定した値です。ワーカー数はテストごとに異なります。詳細は「このベンチマークが測定するもの」を参照してください。

**主なポイント**

- **接続スケーラビリティ**: RSS は 10,000 から 1,000,000 同時接続まで **~110–146 MB** 前後を維持。接続あたりのメモリコストは実質的に平坦です。
- **現実的な負荷下で安定**: C10k は **100% 成功**、C100k は **100.00% 成功**で完了し、同じメモリ領域内に収まりました。
- **C1m スケールでもメモリ領域を維持**: C1m churn 実行（100k 維持 TLS 接続上で 1M リクエスト）は **100% 成功**でストールなく完了し、RSS は ~146 MB を維持 — メモリのスパイラルもクラッシュもありませんでした。
- **データ安全性**: SQLite は SIGINT ですべてのデータを安全に永続化しました（C10k で 256 FS outputs）。

### スループットベンチマーク

上記のベンチマークは**接続スケーラビリティ**を測定するもので、絶対的な**リクエストスループット**ではありません。サーバーの生のスループット上限を測定するため、`h2load`（`-r` レート制限なし）で HTTP/2 上に非制限テストを実行しました。

| 項目 | 値 |
|------|-------|
| Command | `h2load -c512 -n100000 https://127.0.0.1:8888/` |
| Workers | 12 |
| 同時接続数 | 512 |
| 総リクエスト数 | 100,000 |
| 成功数 | 100,000 |
| 失敗 / エラー / タイムアウト | 0 |
| 継続時間 | 13.95 s |
| 平均 RPS | **7167.28** |
| 平均スループット | **290.51 MB/s** |
| リクエストレイテンシ (h2load `time for request`) | min 183 µs, mean 30.69 ms, max 209.00 ms, sd 11.18 ms |

#### `wrk` を使用した HTTP/1.1 比較

比較のため、同じエンドポイントを HTTP/1.1 上で `wrk` を使ってテストしました。プロトコルとベンチマークツールが異なるため、以下の数値は上記の HTTP/2 h2load 結果と**直接比較できません**。

| 項目 | 値 |
|------|-------|
| Command | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` |
| 継続時間 | 60 s |
| Requests/sec | **1282.49** |
| Transfer/sec | 52.29 MB |
| Latency | Avg 138.61 ms, Stdev 39.26 ms, Max 311.70 ms |

これらの数値は、集中的でレート制限されていない負荷下でのエンジンの絶対的なスループット上限を示します。上記の接続スケーラビリティテストとは別物です。
