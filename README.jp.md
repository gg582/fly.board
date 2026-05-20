# fly.board

![fly.board logo](img/logo.png)

> idle 時 **~577 MB RSS**、C10k（10,000 同時接続）時も **約 658 MB** で動作する、極めて軽量なブログエンジンの一つ。  
> C 言語製 CWIST Web フレームワークをベースに、HTTPS/3・Argon2id・PQC 署名・NATS メッセージングをサポートする、軽量な掲示板＆ブログエンジン。
>
> **Fairly small, greater usability.**  
> TASFA は意図的に RPS を犠牲にする。チャンク暗号化・HTP 検証・ビットマップセッション・適応型ペーシングにより、遅くても品質の悪いネットワークでアップロードが途切れず、DoS 攻撃やチャンクの置換まで防ぐ信頼性最大化の転送を追求する。  
> PQC 署名は ML-DSA-65 のオーバーヘッドを犠牲にして、量子コンピューティング時代における投稿本文の改ざん検出性を確保した。  
> HTTP/1.1・HTTP/2・HTTP/3 の同時支援は、単一プロトコルの最適化を放棄する代わりに、あらゆるファイアウォール・プロキシ・端末からのアクセスを可能にする最大公約数を作る。

## 機能

- **メモリ効率** – スタック＋ヒープの C 実装。idle 時は **~577 MB**、10,000 同時接続（C10k）時の最大 RSS も **約 658 MB** に抑えられる。
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

### ホスト環境

| 項目 | 値 |
|------|-------|
| OS | Linux 7.0.0-mountain+ |
| アーキテクチャ | x86_64 |
| CPU | AMD Ryzen 5 5600X @ 3.70GHz (6 cores / 12 threads) |
| RAM | 64 GB |
| ディスク | Samsung SSD 980 1TB (NVMe) |
| OpenSSL | 3.5.5 |
| ベンチマークツール | wrk, h2load |
| CWIST | `patches/cwist` |

### 最大スループット（RPS）

`wrk -t4 -c400 -d30s` (TLS 1.3)

| エンドポイント | ピーク RPS | 平均レイテンシ | 備考 |
|----------|----------|-------------|-------|
| `/` (Home) | **941.67** | 174.60ms | DB query + markdown rendering |
| `/login` | **927.08** | 175.83ms | Static form |
| `/boards` | **920.36** | 178.16ms | DB-driven list |

> 注記: TLS 接続終了時にソケット read error が発生しますが、スループット測定には影響しません。

### メモリ使用量

| 状態 | RSS | 備考 |
|-------|-----|-------|
| Idle | **~577 MB** (590,528 KB) | 4 workers, no connections |
| C10k | **~658 MB** (673,688 KB) | 10,000 concurrent connections |
| C100k | **~692 MB** (708,300 KB) | 100,000 concurrent connections |

### C10k 同時接続テスト

`h2load` で 10,000 同時接続を維持して測定。

| 項目 | 値 |
|------|-------|
| 同時接続数 | 10,000 |
| 継続時間 | 21.98 s |
| 最大 RSS | **約 658 MB** (673,688 KB) |
| CPU 使用率 | ~199% |
| User time | 36.41 s |
| System time | 7.43 s |
| Major page faults | **0** |
| Minor page faults | 170,352 |
| Voluntary context switches | 2,197,128 |
| Involuntary context switches | 293,375 |
| File system outputs | 72 |
| 終了ステータス | **0** |

### C100k 同時接続テスト

`h2load` で 100,000 同時接続を維持して測定。

| 項目 | 値 |
|------|-------|
| 同時接続数 | 100,000 |
| 継続時間 | 2:38.55 |
| 最大 RSS | **約 692 MB** (708,300 KB) |
| CPU 使用率 | ~91% |
| User time | 120.81 s |
| System time | 24.13 s |
| Major page faults | **0** |
| Minor page faults | 191,633 |
| Voluntary context switches | 6,371,528 |
| Involuntary context switches | 842,479 |
| File system outputs | 72 |
| 終了ステータス | **0** |

> 注記: HTTP/2 (TLS 1.3) 上で実際のクライアント接続を維持しながら測定した値です。

**C10k ベンチマークの主な強み**
- **メモリ効率**: 10,000 同時接続でも RSS が 660 MB 未満（接続あたり約 66 KB）
- **ゼロディスク I/O**: Major page faults 0, Swaps 0, FS inputs 0 — 負荷時も純メモリ処理
- **高い CPU 利用率**: ~199% CPU 使用率を安定的に維持
- **長時間安定性**: 21.98 s 間の継続的な C10k 負荷の後も正常終了（Exit status 0）
- **データ安全性**: SIGINT 受信後も SQLite がデータを安全に保存（72 FS outputs）
