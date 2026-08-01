# fly.board

![fly.board logo](img/logo.png)

> 接続数が増えてもメモリをほぼ平坦に保つ、数少ないシンプルなブログエンジンの一つ: idle 時 **~82 MB RSS**（4 workers 構成。single worker 構成では実運用サーバーで **68–120 MB** を維持）、C10k、C100k、さらに C1m でも **~94–96 MB**。
> C 言語製 CWIST Web フレームワークをベースに、HTTPS/3、Argon2id、PQC 署名、NATS メッセージングをサポートする軽量な掲示板＆ブログエンジン。

## 機能

- **メモリ効率と接続スケーラビリティ** – スタック＋ヒープの C 実装。idle 時 **~82 MB RSS**、C10k から C1m までの同時接続で RSS は **~94–96 MB** 前後に維持される。
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

- `blog.settings` – ブログタイトル、サブタイトル、フッター、ポート、アップロード上限
- `admin.settings` – 管理者アカウント（2 行: `username`\n`password`）

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

ワーカー数は負荷に応じてスケールされ、各テストを現実的に保ちます: C10k では **4 workers**、C100k では **12 workers**、C1m では **24 workers** です。これにより、3 回の実行で CPU 使用率の数値が異なる理由も説明されます。

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
| CWIST | `/usr/local/lib/libcwist.a` |

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
| Idle | **~82 MB** (83,708 KB) | — | 4 workers, no connections |
| C10k | **~96 MB** (96,252 KB) | +12.25 MB | 10,000 concurrent connections |
| C100k | **~94 MB** (94,352 KB) | -1,900 KB | 100,000 concurrent connections |
| C1m | **~95 MB** (94,944 KB) | +592 KB | 1,000,000 concurrent connections |

**C10k から C1m までの RSS 変化は -1,308 KB** — 実質的に測定ノイズの範囲です。これが本ベンチマークにおいて最も重要な結果です。

RSS 値は、サーバープロセスに対する `/usr/bin/time -v` の **Maximum resident set size (kbytes)** です。

### メモリコスト

| 遷移 | Δ RSS | Δ 接続数 | 追加接続あたりの概算コスト |
|---|---|---|---|
| Idle → C10k | +12.25 MB | 10,000 | 接続あたり ~1.3 KB |
| C10k → C1m | -1,308 KB | 990,000 | 追加接続あたり ~-1.4 bytes（ノイズ） |

Idle から C10k への初期ジャンプは、TLS 状態、接続バッファ、worker オーバーヘッドを前払いするコストです。その後の C10k から C1m までの RSS 変化は測定ノイズの範囲であり、接続あたりのメモリコストは実質的に平坦です。

### C10k 同時接続テスト

`h2load` で 10,000 同時接続を維持して測定。

| 項目 | 値 |
|------|-------|
| Workers | 4 |
| 同時接続数 | 10,000 |
| 継続時間 | 13.62 s |
| 最大 RSS | **~96 MB** (96,252 KB) |
| CPU 使用率 | ~578% |
| User time | 72.53 s |
| System time | 6.23 s |
| Major page faults | 0 |
| Minor page faults | 82,722 |
| Voluntary context switches | 1,689,448 |
| Involuntary context switches | 18,959 |
| File system outputs | 200 |
| 総リクエスト数 | 20000 |
| 成功数 | 20000 |
| 失敗数 | 0 |
| 概算合計 RPS | **2490.60** |
| 成功率 | **100.00%** |
| 終了ステータス | **0** |

### C100k 同時接続テスト

`h2load` で 100,000 同時接続を維持して測定。

| 項目 | 値 |
|------|-------|
| Workers | 12 |
| 同時接続数 | 100,000 |
| 継続時間 | 1:24.88 |
| 最大 RSS | **~94 MB** (94,352 KB) |
| CPU 使用率 | ~871% |
| User time | 701.20 s |
| System time | 38.28 s |
| Major page faults | 0 |
| Minor page faults | 292,073 |
| Voluntary context switches | 3,522,910 |
| Involuntary context switches | 184,003 |
| File system outputs | 208 |
| 総リクエスト数 | 200000 |
| 成功数 | 200000 |
| 失敗数 | 0 |
| 概算合計 RPS | **2541.24** |
| 成功率 | **100.00%** |
| 終了ステータス | **0** |

### C1m 同時接続テスト

`h2load` で 1,000,000 個の同時接続を維持しながら測定。

| 項目 | 値 |
|------|-------|
| Workers | 24 |
| 同時接続数 | 1,000,000 |
| 継続時間 | 7:05.71 |
| 最大 RSS | **~95 MB** (94,944 KB) |
| CPU 使用率 | ~641% |
| User time | 2517.01 s |
| System time | 215.52 s |
| Major page faults | 0 |
| Minor page faults | 789,451 |
| Voluntary context switches | 23,921,809 |
| Involuntary context switches | 943,712 |
| File system outputs | 208 |
| 総リクエスト数 | 2000000 |
| 成功数 | 711274 |
| 失敗数 | 1288726 |
| 概算合計 RPS | **1694.48** |
| 成功率 | **35.56%** |
| 終了ステータス | **0** |

> 注記: HTTP/2（TLS 1.3）上で実際のクライアント接続を維持しながら測定した値です。ワーカー数はテストごとに異なります。詳細は「このベンチマークが測定するもの」を参照してください。

**主なポイント**

- **接続スケーラビリティ**: RSS は 10,000 から 1,000,000 同時接続まで **~94–96 MB** 前後を維持。接続あたりのメモリコストは実質的に平坦です。
- **現実的な負荷下で安定**: C10k と C100k は **100% 成功**で完了し、同じメモリ領域内に収まりました。
- **C1m でもメモリ領域を維持**: テストハードウェアが 1,000,000 接続すべてを完全に処理できなかった場合（35.56% 成功）でも、メモリ使用量は事実上変わらず — サーバーは暴走しませんでした。
- **データ安全性**: SQLite は SIGINT ですべてのデータを安全に永続化しました（C10k で 200 FS outputs）。

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
| 概算パーセンタイルレイテンシ* | p50 ~30.7 ms, p95 ~49.1 ms, p99 ~56.7 ms |

\* パーセンタイルは報告された平均と標準偏差から推定しています。h2load のデフォルト出力は min/max/mean/sd のみです。正確なパーセンタイルヒストグラムには `--latency-collect` オプションを使用してください。

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
