# fly.board

![fly.board logo](img/logo.png)

> 데스크톱급 호스트 한 대에서 **동시 연결 100만 개**를 유지하고(평문 HTTP/1.1, TLS HTTP/1.1, TLS HTTP/2 실측), C10k/C100k TLS 부하 테스트를 100% 성공으로 통과하는 몇 안 되는 심플한 블로그 엔진입니다.
> C 기반 CWIST 웹 프레임워크 위에 구축된 가벼운 게시판 겸 블로그 엔진으로, HTTPS/3, Argon2id, PQC 서명, NATS 메시징을 지원합니다.

## 특징

- **연결 확장성** – cwist 이벤트 기반 reactor 위의 스택+힙 C 구현. 평문 HTTP/1.1, TLS HTTP/1.1, TLS HTTP/2 실측에서 **동시 연결 100만 개**를 유지하고 서비스했습니다. 비로그인 공개 페이지는 Big Dumb Reply 캐시에서 서빙합니다.
- **최신 전송 계층** – 기본적으로 TLS 1.3 + HTTP/3 (QUIC). 선택적 ECH(Encrypted Client Hello).
- **안전한 인증** – 클라이언트 측 SHA-512 프리해시 + 서버 측 **Argon2id** (OpenSSL 3 KDF). JWT 세션 쿠키.
- **게시판 / 블로그 하이브리드** – 슬러그 기반 마크다운 포스트 + 다중 게시판 + 계층형 댓글.
- **실시간 미리보기** – 마크다운 에디터에서 입력 즉시 서버 측 프리뷰 렌더링.
- **PQC 서명** – 게시글에 양자 내성 암호(PQC) 기반 서명을 첨부/검증.
- **파일 저장소** – 1 MB 이하는 SQLite에, 더 큰 파일은 볼륨에 저장. 이미지/비디오/오디오 자동 임베드.
- **NATS 연동** – `NATS_URL` 환경 변수를 통한 분산 메시징 게이트웨이.
- **다크 모드** – 쿠키 기반 테마 전환 및 동적 CSS 변수.

## 빌드

```sh
make
./keygen.sh
```

의존성:
- [CWIST](https://github.com/religiya-serdtsa/cwist) — TLS 1.3 / HTTP/3 (QUIC)는 CWIST에 내장된 BoringSSL에서 처리되며 별도 설정이 필요 없습니다.
- OpenSSL 3.x (Argon2id KDF)
- ngtcp2 / nghttp3 (HTTP/3)
- cJSON, SQLite3

`Makefile`은 `third_party/md4c`를 정적 라이브러리로 클론 및 빌드합니다.

## 실행

```sh
./fly_board
```

기본 포트는 `blog.settings`의 `port` 값을 따릅니다(기본값 9443).

```text
https://localhost:9443
```

HTTP/3는 동일한 포트의 UDP에서 수신합니다.

### ECH 활성화 (선택)

```sh
BLOG_ECH_KEY=ech/server.ech ./fly_board
# 또는
BLOG_ECH_DIR=ech ./fly_board
```

OpenSSL 빌드가 ECH를 지원하지 않으면 경고를 로그에 남기고 일반 HTTPS/3로 계속 실행됩니다.

### NATS 연동 (선택)

```sh
NATS_URL=nats://localhost:4222 ./fly_board
```

## 주요 기능

| 기능 | 경로 | 설명 |
|---------|------|-------------|
| 홈 | `/` | 최신 포스트 목록 |
| 게시판 | `/boards` | 다중 게시판 관리 (admin-only 지원) |
| 포스트 | `/post/:slug` | md4c 마크다운 렌더링 + 댓글 + 첨부파일 |
| 임시저장/예약 | `/account/drafts` | 초안 저장, 예약 발행(미래 시각), 공개 취소. 작성자와 관리자만 열람 |
| 로그인/가입 | `/login`, `/register` | Argon2id + JWT 쿠키 |
| 프로필 | `/profile` | 닉네임, 바이오, 프로필 사진, 가입일 |
| 계정 설정 | `/account/settings` | 프로필 수정 |
| 비밀번호 변경 | `/account/password` | 현재 비밀번호 확인 후 Argon2id로 재해싱 |
| 관리자 | `/admin/users` | 사용자 역할 변경, 삭제 |
| 파일 저장소 | `/files` | 업로드/다운로드/삭제 |

## 설정

설정은 세 개의 파일(첫 실행 시 기본값으로 자동 생성)과 운영 토글용 환경변수로 이루어집니다.

### `admin.settings`

두 줄짜리 원본 텍스트: 1행은 관리자 아이디, 2행은 관리자 비밀번호.

### `blog.settings`

`key=value` 형식. 모르는 키는 무시되고, 잘못된 값은 기본값으로 대체됩니다.

| 키 | 기본값 | 값 / 범위 |
|-----|---------|-----------|
| `title` | `CWIST Docker Blog` | 상단 바에 표시되는 사이트 제목 |
| `subtitle` | `Explore boards and read stories.` | 히어로 부제목 |
| `brand_footer` | `Built with CWIST C Framework` | 푸터 문구 |
| `root_url` | `https://localhost:8888/` | 사이트의 정규 URL(끝에 `/` 필수). RSS 링크, 인증 메일, 인증서 갱신에 사용 — 운영 시 공개 URL로 설정 |
| `port` | `8443` | TCP/UDP 리슨 포트(HTTP/3는 같은 포트를 UDP로 사용) |
| `accent` | `#3b82f6` | 강조 색상(hex) |
| `use_tls` | `true` | HTTPS on/off(먼저 `./keygen.sh` 실행 필요) |
| `use_http2` | `true` | TLS 위 HTTP/2 |
| `use_http3` | `true` | UDP 위 HTTP/3 (QUIC) |
| `use_tasfa` | `true` | TASFA 미디어 파이프라인(ffmpeg 썸네일/프리뷰) |
| `use_rss` | `false` | `/rss.xml` 노출 |
| `roundness` | `0.0` | UI 모서리 둥글기, `0.0`–`1.0` |
| `max_upload_size` | `1G` | 파일당 업로드 한도. `K/M/G/T` 접미사 사용 가능(예: `500M`) |
| `max_total_parallel_uploads` | `8` | 전체 동시 업로드 수(1–512) |
| `max_upload_parallel_chunks` | `32` | 업로드당 병렬 청크 수(1–64) |
| `max_concurrent_downloads` | `128` | 동시 다운로드 수(1–512) |
| `vote_only` | *(비어 있으면 `all`)* | 글 추천 가능 범위: `all`(익명 포함 전체), `authorized`(로그인 사용자만), `admin`(관리자만) |
| `use_special_modes` | *(비어 있음)* | 라이트/다크 테마 대체: `라이트테마,다크테마`(또는 단일 테마). 사용 가능한 테마: `light`, `dark`, `ocean`, `forest`, `sepia`. 예: `ocean,forest` |
| `home_img`, `boards_img`, `files_img` | *(비어 있음)* | 페이지별 히어로/배경 이미지. `public/img/` 안의 파일명 |
| `*_dark` (`home_img_dark`, `boards_img_dark`, `files_img_dark`) | *(비어 있음)* | 위 항목의 다크 모드 변형 |
| `blog_logo`, `blog_logo_dark` | *(비어 있음)* | `public/img/` 안의 로고 이미지 |
| `invert_logo` | `false` | 이미지가 없는 모드용으로 로고를 자동 반전 |
| `favicon` | *(비어 있음)* | `public/img/` 안의 파비콘 |
| `bg_full_light`, `bg_full_dark` | *(비어 있음)* | 전체 페이지 배경 이미지 |
| `bg_invert_color` | *(비어 있음)* | 한쪽 모드 이미지만 있을 때 반전으로 나머지를 생성할 대상(쉼표 구분): `home`, `boards`, `files`, `toplevel`, `logo` |
| `bg_invert_algo` | `luminv` | 반전 알고리즘: `luminv` 또는 `oklch` |

### `fonts.settings`

타이포그래피 재정의: `font_body`, `font_heading`, `font_ui`, `font_code`, `font_blockquote`, `font_display`, `font_import_url`, `font_face_family`, `font_face_src`와 요소별 `letter_spacing_*`, `font_weight_*`. 첫 실행 시 기본값이 모두 기록되므로 생성된 파일을 열어 전체 키를 확인할 수 있습니다.

### `s3.settings` (선택)

업로드 파일용 S3 호환 오브젝트 스토리지(AWS S3, MinIO, R2, B2). 완전한 선택 사항이며, 비어 있으면 파일은 `public/uploads/` 로컬 디스크에 저장됩니다. `mode=mirror`(로컬 사본 유지 + S3 백업)와 `mode=offload`(S3로 이동, 다운로드는 presigned 리다이렉트)를 지원합니다. 전체 레퍼런스와 설정 예시: [S3.ko.md](S3.ko.md).

### 환경변수

**코어**

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `BLOG_ROOT` | *(없음)* | 프로젝트 루트. 바이너리를 다른 디렉터리에서 실행할 때 사용. 미설정 시 `public/`이 있는 위치를 자동 탐색 |
| `DEBUG` | *(끔)* | `1`/`true`/`yes`면 DEBUG/INFO 로그 출력, 아니면 경고/에러만 |
| `NATS_URL` | *(없음)* | 예: `nats://localhost:4222` — NATS 메시징 게이트웨이 활성화 |
| `BLOG_ECH_KEY` / `BLOG_ECH_DIR` | *(없음)* | ECH(Encrypted Client Hello) 키 파일 / 키 디렉터리 |
| `CWIST_C1M_MODE` | `1` | 이벤트 기반 C1M 리액터. `0`으로 설정하면 레거시 스레드 풀 경로 사용 |

**성능 / 캐시**

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `FLYBOARD_CACHE_MAX_MB` | `64` | 페이지 캐시 크기(MB, 1–1024) |
| `FLYBOARD_ADVERTISE_H3` | `true` | HTTP/3를 알리는 `Alt-Svc` 헤더 전송 |
| `FLYBOARD_ALT_SVC_MAX_AGE` | `300` | `Alt-Svc`의 `ma` 값(초, 0–86400) |
| `FLYBOARD_INLINE_IMAGES` | *(끔)* | 이미지를 base64 data URI로 HTML에 인라인 |
| `FLYBOARD_INLINE_ALL_ASSETS` | *(끔)* | 스크립트/스타일까지 인라인 |
| `FLYBOARD_INLINE_BG_IMAGES` | *(끔)* | 배경 이미지도 인라인(`ALL_ASSETS`와 별개의 명시적 옵트인) |
| `FLYBOARD_INLINE_MAX_IMAGE_SIZE` | `49152` | 인라인할 이미지당 최대 바이트 |
| `FLYBOARD_INLINE_MAX_ASSET_SIZE` | `65536` | 인라인할 스크립트/스타일 청크당 최대 바이트 |
| `FLY_MEDIA_MAX_CONCURRENT` | `2` | 미디어 프리뷰용 ffmpeg 동시 변환 수 |
| `FLYBOARD_MEDIA_BACKFILL_ON_START` | *(끔)* | 시작 시 레거시 미디어 프리뷰 전체 재생성(유지보수용) |

**TLS 인증서 자동 갱신** — 로컬 ACME 클라이언트 사용. `keygen.sh`의 자가서명 임시 인증서는 자동으로 감지해 건드리지 않음

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `FLY_CERT_RENEWAL` | *(끔)* | `true`면 매일 만료 감시. 남은 기간이 `FLY_CERT_DAYS` 이하일 때 갱신하고 재시작 없이 핫리로드 |
| `FLY_CERT_DAYS` | `30` | 갱신 임계일 |
| `FLY_CERT_EMAIL` | `admin@<호스트>` | ACME 계정 이메일 |
| `FLY_CERT_LEGO_BIN` | `lego` | lego 바이너리 이름/경로(DNS 챌린지 등은 래퍼 스크립트 지정) |

도메인은 `root_url`에서 추출하며 HTTP-01 챌린지를 사용하므로 80번 포트가 서버에 도달해야 합니다. 상태는 `.lego/`에 저장되고, 갱신된 인증서는 `server.crt`/`server.key`에 설치됩니다.

**이메일 인증 가입** — 기본은 꺼짐(자유 가입)

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `FLY_EMAIL_CERT` | *(끔)* | `true`면 가입 시 입력한 이메일로 인증 링크(24시간 유효)를 SMTP로 발송하고, 인증 전까지 로그인 차단 |
| `FLY_SMTP_HOST` | *(켰을 때 필수)* | SMTP 릴레이 호스트 |
| `FLY_SMTP_PORT` | `25`(implicit TLS면 `465`) | SMTP 포트 |
| `FLY_SMTP_TLS` | *(끔)* | `starttls` 또는 `implicit` |
| `FLY_SMTP_USER` / `FLY_SMTP_PASS` | *(없음)* | AUTH LOGIN 자격 증명(선택) |
| `FLY_SMTP_FROM` | `FLY_SMTP_USER` | envelope/헤더 발신자 |

예시 — 이메일 인증 가입 + 인증서 자동 갱신 운영:

```sh
FLY_CERT_RENEWAL=true FLY_CERT_EMAIL=admin@example.com \
FLY_EMAIL_CERT=true FLY_SMTP_HOST=smtp.example.com FLY_SMTP_PORT=587 \
FLY_SMTP_TLS=starttls FLY_SMTP_USER=noreply@example.com FLY_SMTP_PASS=secret \
./fly_board
```

## 데이터베이스

SQLite3 (`data/blog.db`). 스키마는 앱 시작 시 자동 마이그레이션됩니다.

```
users       – 계정, Argon2id 해시, 역할, 프로필
boards      – 게시판 이름/슬러그/설명/admin_only
posts       – 마크다운 본문, PQC 서명, 요약
files       – 첨부 파일 경로/크기/MIME
comments    – 계층형 댓글 (target_type, parent_id)
board_permissions – 비공개 게시판 접근 권한
```

## 아키텍처

```
CWIST (HTTP/3, TLS 1.3)
  ├── src/auth/     – Argon2id, JWT, 세션
  ├── src/db/       – SQLite3 CRUD
  ├── src/handlers/ – 라우팅/비즈니스 로직
  ├── src/render/   – cwist_html_element SSR + md4c
  ├── src/crypto/   – PQC 서명/검증
  └── src/nats/     – 메시징 Pub/Sub
```

## 라이선스

MIT License

---

## 확장성 벤치마크

### 이 벤치마크가 측정하는 것

서로 다른 세 가지를 잽니다. 이 섹션의 예전 버전은 이것들을 섞어 썼습니다.

- **동시 연결** (전통적인 C10K/C1M의 의미): 서버가 동시에 열어 두고 서비스하는 연결 수. `run_c1m_held_bench.sh`에서 `tools/connhold`로 측정합니다.
- **유지된 연결 위의 요청 처리(churn)**: `h2load` 묶음(`run_c10k_bench.sh`, `run_c100k_bench.sh`, `run_c1m_bench.sh`). `h2load`는 `-r`(속도 제한)로 돌기 때문에 RPS는 설정한 부하를 반영할 뿐 처리량 상한이 아닙니다. `run_c1m_bench.sh`는 동시 연결 10만 개에 요청 100만 건이며, 이름은 예전 것을 그대로 쓰고 있습니다.
- **처리량**: 첫 페이지에 대한 제한 없는 `h2load`와 `wrk` 실행.

### 호스트 환경

| 항목 | 값 |
|------|-------|
| OS | Linux 6.12.107+deb13-amd64 (Debian 13) |
| CPU | AMD Ryzen 5 5600X (6코어 / 12스레드) |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| 부하 발생기 | h2load nghttp2/1.64.0, wrk, `tools/connhold` (BoringSSL) |
| CWIST | `main` `11f3518d` (2026-09-29, v3.7.1에 포함) |
| TLS 인증서 | ECDSA P-256 (`keygen.sh` 기본값) |
| 서빙 모드 | `CWIST_C1M_MODE=1` (이벤트 기반 reactor) |

클라이언트와 서버는 같은 호스트에서 돌았습니다.

### 시스템 튜닝

| 파라미터 | 값 |
|-----------|-------|
| ulimit -n | 1,050,000 |
| fs.file-max | 8,388,608 (연결 100만 개는 클라이언트와 서버를 합쳐 fd 200만 개 필요) |
| fs.nr_open | 1,050,000 |
| net.netfilter.nf_conntrack_max | 4,194,304 (loopback 연결도 추적됨) |
| net.core.somaxconn | 1,050,000 |
| net.ipv4.tcp_max_syn_backlog | 1,050,000 |
| net.ipv4.ip_local_port_range | 1024 65535 |
| vm.max_map_count | 1,048,576 |
| kernel.pid_max | 4,194,304 |
| CPU governor | ecodemand |

### C1M: 동시 연결 100만 개 (2026-09-29)

`run_c1m_held_bench.sh`: 워커 12개, 연결 100만 개를 loopback 주소 48개에 나눠 엽니다. 각 연결은 `GET /robots.txt`를 보낸 뒤 keep-alive 타이머가 만료되지 않도록 60–120초마다 다시 보냅니다(HTTP/2는 매번 새 스트림). 실패한 연결은 재시도하지 않고 그대로 셉니다.

| | 평문 HTTP/1.1 | TLS 1.3 + HTTP/1.1 | TLS 1.3 + HTTP/2 |
|---|---|---|---|
| 연 연결 | 1,000,000 | 1,000,000 | 1,000,000 |
| 연결 속도 | 초당 40,000 | 초당 8,000 | 초당 8,000 |
| 최대 유지 연결 | **1,000,000** | **1,000,000** | **1,000,000** |
| 동시에 서비스된 최대 연결 | **1,000,000** | **1,000,000** | **1,000,000** |
| 응답 수 (keep-alive GET 포함) | 2,124,089 | 2,632,665 | 2,052,855 |
| 실패한 연결 | 0 | 0 | 0 |
| 최대 시점 서버 메모리 (PSS, 전체 워커) | 24.0 GB | 19.9 GB | 30.1 GB |

- **세 모드 모두 동시 연결 100만 개를 유지하고 서비스했으며, 실패는 0입니다.**
- TLS는 CWIST v3.7.1 또는 `11f3518d` 이후의 `main`이 필요합니다. 그 전에는 idle TLS 연결마다 풀 스레드 하나가 기다리고 있어서, 동시에 서비스되는 TLS 연결이 풀 스레드 수만큼뿐이었습니다. 같은 실행에서 TLS 연결 395,729개가 유지됐지만 동시에 서비스된 것은 최대 25개였습니다. v3.7.1은 idle TLS 연결을 epoll 집합에 주차했다가 데이터가 오면 풀로 돌려보냅니다.
- TLS 연결 속도는 풀 핸드셰이크가 제한합니다. 초당 20,000 연결에서는 핸드셰이크의 약 37%가 45초 한도를 넘겼고(서비스된 연결 HTTP/1.1 523,654개, HTTP/2 510,023개), 초당 8,000에서는 하나도 넘기지 않았습니다.
- 부하 구성 주의: Linux `connect()`는 짝수 임시 포트를 먼저 쓰고, 다 떨어지면 느린 홀수 포트 탐색으로 넘어갑니다. 그래서 목적지 주소 하나당 빠르게 열 수 있는 연결은 약 3.2만 개입니다. 주소 24개로는 매번 약 77.4만에서 멈췄습니다. 주소는 `연결 수 / 32,000`개 이상 쓰세요.

### 메모리

모든 서버 프로세스(마스터와 fork된 워커)의 PSS 합계입니다.

| 경우 | 서버 합계 | 연결당 |
|---|---|---|
| idle, 워커 1개 | 115 MB | — |
| idle, 워커 4개 | 406 MB | — |
| TLS/HTTP/2 연결 10만 개 (h2load C100k) | 1.77 GB (워밍업 후 idle 1.11 GB) | 약 6.7 KB |
| 평문 HTTP/1.1 연결 100만 개 (connhold) | 24.0 GB | 약 24 KB |
| TLS HTTP/1.1 연결 100만 개 (connhold) | 19.9 GB | 약 20 KB |
| TLS HTTP/2 연결 100만 개 (connhold) | 30.1 GB | 약 30 KB |

같은 호스트에서 돌릴 때 클라이언트 비용: h2load는 연결당 약 60 KB, connhold는 약 0.06 KB에 커널 소켓 메모리(C100k에서 연결 쌍당 약 12.7 KB)가 더해집니다.

> **정정:** 이 README의 예전 버전은 idle 약 102–108 MB, C10k부터 C1m까지 약 110–146 MB라고 했습니다. 그 값은 `/usr/bin/time -v`가 잰 마스터 프로세스 하나의 최대 RSS였습니다. `cwist_app_listen()`은 서빙 워커를 fork하는데, 그 워커들의 메모리는 한 번도 세지 않았습니다. 위의 합계가 그 값을 대신합니다.

### h2load 묶음: 요청 처리 (2026-09-29, CWIST `11f3518d`)

| 테스트 | 동시 연결 | 요청 | 성공 | 소요 시간 | RPS (프로세스 합) |
|---|---|---|---|---|---|
| C10k (워커 4) | 10,000 | 20,000 | **100%** | 8.50초 | 9,253 |
| C100k (워커 12) | 100,000 | 200,000 | **100%** | 29.83초 | 8,958 |
| C1m churn (워커 12) | 100,000 | 1,000,000 | **100%** | 57.28초 | 19,754 |

소요 시간은 서버 프로세스의 수명입니다(시작과 5초 종료 drain 포함). 응답은 79 KB짜리 첫 페이지 전체이고, h2load는 압축을 요청하지 않습니다.

같은 날 앞서 RSA-4096 인증서로는 C100k가 72.6%, C1m churn이 65.2%로 떨어졌습니다. 바쁜 CPU 거의 전부가 TLS 1.3 풀 핸드셰이크마다 하는 RSA CertificateVerify 서명에 쓰여서, 대기 중인 핸드셰이크가 45초 한도에 걸렸습니다. ECDSA P-256(`keygen.sh` 기본값)으로 바꾸고, 라우트 핸들러를 요청 워커에서 돌리고, 비로그인 공개 페이지를 라우트 Big Dumb Reply 캐시로 서빙해서 100%로 돌아왔습니다.

### 처리량 벤치마크 (2026-09-29, CWIST `11f3518d`)

첫 페이지에 대한 제한 없는 부하(`-r` 없음), 워커 12개.

| 도구 | 명령 | 결과 |
|---|---|---|
| h2load (HTTP/2) | `h2load -c512 -n100000 https://127.0.0.1:8888/` | **초당 12,337건**, 970.71 MB/s, 100,000/100,000 성공, 8.11초; 요청 시간 최소 410 µs, 평균 20.69 ms, 최대 205.85 ms |
| wrk (HTTP/1.1) | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` | **초당 48,829건**, 3.76 GB/s; 지연 평균 9.50 ms, 표준편차 4.15 ms, 최대 132.20 ms; 소켓 에러 없음 |

두 도구는 프로토콜이 달라 직접 비교할 수 없습니다. 2026-08에는 같은 명령으로 h2load 초당 7,167건, wrk 초당 1,282건(읽기 에러 77,027건)이었습니다. 지금은 비로그인 첫 페이지 요청이 라우트 Big Dumb Reply 캐시에서 응답됩니다.

**핵심 요약**

- **C1M:** 데스크톱급 호스트 한 대에서 평문 HTTP/1.1, TLS HTTP/1.1, TLS HTTP/2 모두 동시 연결 100만 개를 유지하고 서비스했으며, 실패는 0입니다.
- **연결당 메모리는 실제로 듭니다:** 유지된 연결당 서버에서 약 20–30 KB. RAM을 그에 맞게 잡으세요.
- **TLS 연결 속도는 핸드셰이크가 제한합니다:** ECDSA 인증서를 쓰세요. 이 호스트에서는 초당 약 8,000번의 풀 핸드셰이크가 놓침 없이 끝났습니다.
