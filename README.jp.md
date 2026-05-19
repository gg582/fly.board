# fly.board

![fly.board logo](img/logo.png)

> idle 時 **100-200 MB RSS**、C10k（10,000 同時接続）時も **約 369 MB** で動作する、極めて軽量なブログエンジンの一つ。  
> C 言語製 CWIST Web フレームワークをベースに、HTTPS/3・Argon2id・PQC 署名・NATS メッセージングをサポートする、軽量な掲示板＆ブログエンジン。

## 機能

- **メモリ効率** – スタック＋ヒープの C 実装。idle 時は **100-200 MB**、10,000 同時接続（C10k）時の最大 RSS も **約 369 MB** に抑えられる。
- **最新トランスポート** – デフォルトで TLS 1.3 + HTTP/3（QUIC）。オプションで ECH（Encrypted Client Hello）も利用可能。
- **安全な認証** – クライアント側 SHA-512 プリハッシュ + サーバー側 **Argon2id**（OpenSSL 3 KDF）。JWT セッション Cookie。
- **掲示板 / ブログ ハイブリッド** – Slug ベースの Markdown 投稿 + 複数掲示板 + 入れ子コメント。
- **リアルタイムプレビュー** – Markdown エディタから即座にサーバー側でプレビューをレンダリング。
- **PQC 署名** – 投稿にポスト量子暗号（PQC）ベースの署名を付与・検証。
- **ファイルストレージ** – 1 MB 以下は SQLite に、それ以上はボリュームに保存。画像・動画・音声を自動埋め込み。
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

- `blog.settings` – ブログタイトル、サブタイトル、フッター、ポート
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

## 性能ベンチマーク

> 詳細なベンチマーク手法と完全な結果は [`benchmarks/README.md`](benchmarks/README.md) を参照。

### ホスト環境

| 項目 | 値 |
|------|-------|
| OS | Linux 7.0.0-mountain+ |
| アーキテクチャ | x86_64 |
| CPU | AMD Ryzen 5 5600X @ 3.70GHz（6 コア / 12 スレッド） |
| RAM | 64 GB |
| ディスク | Samsung SSD 980 1TB（NVMe） |
| OpenSSL | 3.5.5 |
| ベンチマークツール | wrk |
| CWIST | `patches/cwist`（SIGPIPE パッチ適用済み） |

### 最大スループット（RPS）

`wrk -t4 -c400 -d30s`（TLS 1.3、シリアライズなし）

| エンドポイント | ピーク RPS | 平均レイテンシ | 備考 |
|----------|----------|-------------|-------|
| `/`（ホーム） | **3,409.92** | 121.84ms | DB クエリ + Markdown レンダリング |
| `/login` | **3,948.77** | 18.03ms | 静的フォーム（キャッシュ可能） |
| `/boards` | **3,901.77** | 17.26ms | DB 駆動リスト |

### リソース使用量（ピーク負荷時）

| 項目 | 値 |
|------|-------|
| CPU 使用率 | ~600%（12 スレッドシステム上） |
| RAM（RSS） | ~12 MB |
| 仮想メモリ（VSZ） | ~1.2 GB |

> 注: ベンチマークはリクエストの**シリアライズなし**（`pthread_mutex_t`）で実行。  
> `ulimit -n` は 20,000 に設定されており、400 接続まで安定して計測可能。

### C10k 同時接続テスト

実運用環境で 10,000 同時接続（C10k）を維持しながら測定（`sudo -E /usr/bin/time -v ./fly_board`）。

| 項目 | 値 |
|------|-------|
| 同時接続数 | 10,000 |
| 継続時間 | 24 分 46 秒 |
| 最大 RSS | **約 369 MB** (368,644 KB) |
| 平均 CPU 使用率 | ~93% |
| User time | 444.17 秒 |
| System time | 951.76 秒 |
| Major page faults | **0** (ディスク I/O なし) |
| Minor page faults | 219,629 |
| Swaps | **0** |
| File system inputs | **0** |
| File system outputs | 89,208 (安全なデータ保存) |
| Voluntary context switches | 346,110,015 |
| Involuntary context switches | 1,690,588 |
| 終了ステータス | **0** (SIGINT 後も正常終了) |

> 注: HTTP/3（QUIC）over TLS 1.3 環境で実際に 10,000 クライアント接続を維持しながら測定した値。

**C10k ベンチマークの主な強み**
- **メモリ効率**: 10,000 同時接続でも RSS が 400 MB 未満（接続あたり約 37 KB）
- **I/O フリー**: Major page faults 0、Swaps 0、FS inputs 0 — 負荷時もディスク I/O なしの純メモリ処理
- **CPU 活用**: 93% の CPU 使用率を維持しながら安定動作
- **長時間安定性**: 24 分 46 秒間の継続的な C10k 負荷の後も正常終了（Exit status 0）
- **データ安全性**: SIGINT 受信後も NukeDB がデータを安全に保存
