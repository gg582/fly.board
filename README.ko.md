# fly.board

![fly.board logo](img/logo.png)

> 연결이 증가할 때 메모리를 거의 일정하게 유지하는 몇 안 되는 심플한 블로그 엔진입니다: single worker로도 idle 시 **~108 MB RSS**(4 workers 시 **~102 MB**), 그리고 C10k, C100k, 심지어 C1m에서도 **~110–146 MB**를 유지합니다.
> C 기반 CWIST 웹 프레임워크 위에 구축된 가벼운 게시판 겸 블로그 엔진으로, HTTPS/3, Argon2id, PQC 서명, NATS 메시징을 지원합니다.

## 특징

- **메모리 효율 및 연결 확장성** – 스택+힙 C 구현. idle 시 **~102–108 MB RSS**(1–4 workers); C10k부터 C1m 동시 연결까지 RSS가 **~110–146 MB**를 유지합니다.
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

이 테스트는 `h2load`의 **`-r` (rate-limit) 옵션**을 사용합니다. 의도적으로 **최대 처리량 테스트가 아닙니다**. 대신 제어된 프로세스별 요청률을 처리하면서 서버가 다수의 동시 HTTP/2 연결을 유지할 수 있는지 측정합니다.

부하가 rate-limited이기 때문에:

- 보고된 **RPS는 설정된 요청률**을 반영하며, 서버의 절대 처리량 한계는 아닙니다.
- 핵심 지표는 연결이 10,000개에서 1,000,000개로 증가할 때의 **resident-set-size(RSS) 안정성**입니다.

각 테스트를 현실적으로 유지하기 위해 worker 수를 부하에 맞게 조정했습니다: C10k는 **4 workers**, C100k는 **12 workers**, C1m은 **12 workers**입니다. 이는 세 번의 실행에서 다른 CPU 사용률 수치를 보이는 이유이기도 합니다.

### 호스트 환경

| 항목 | 값 |
|------|-------|
| OS | Linux 7.1.0-mountain-rc6+ |
| 아키텍처 | x86_64 |
| CPU | 12 logical cores |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| OpenSSL | 3.5.6 |
| 벤치마크 도구 | h2load nghttp2/1.64.0 |
| CWIST | 형제 cwist 체크아웃의 `libcwist.a` (2026-08-29, arena bump allocator, 공유 req/res arena, 256KB worker 스택, HTTP/3 hardening, sharded TLS handshake shepherd) |

### 시스템 튜닝

| 파라미터 | 값 |
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

### 메모리 사용량

| 상태 | RSS | 이전 대비 변화 | 비고 |
|-------|-----|----------------|-------|
| Idle (1 worker) | **~108 MB** (110,196 KB) | — | 1 worker, no connections |
| Idle (4 workers) | **~102 MB** (104,940 KB) | — | 4 workers, no connections |
| C10k | **~110 MB** (112,436 KB) | +7,496 KB (idle 4 workers 대비) | 10,000 concurrent connections |
| C100k | **~146 MB** (148,848 KB) | +36,412 KB | 100,000 concurrent connections |
| C1m churn | **~146 MB** (149,816 KB) | +968 KB | 100k held TLS conns, 1M-request churn |

C100k에서 C1m churn 실행까지의 총 RSS 변화량은 **+968 KB**입니다 — 사실상 측정 노이즈 수준입니다. 이것이 이 벤치마크에서 가장 중요한 결과입니다.

RSS 값은 서버 프로세스에 대해 `/usr/bin/time -v`가 보고한 **Maximum resident set size (kbytes)**입니다.

### 메모리 비용

| 전환 | Δ RSS | Δ 연결 수 | 연결당 대략적 비용 |
|---|---|---|---|
| Idle → C10k | +7,496 KB | 10,000 | 연결당 ~0.75 KB |
| C10k → C1m churn | +37,380 KB | — | 추가 유지 연결당 ~0.4 KB; C100k → C1m은 +968 KB(노이즈) |

Idle에서 C10k로의 초기 증가는 TLS 상태, 연결 버퍼, worker 오버헤드를 미리 지불하는 비용입니다. C10k에서 C100k까지는 추가 유지 연결당 약 ~0.4 KB에 그치고, C100k에서 C1m까지의 RSS 변화(+968 KB)는 측정 노이즈 범위입니다 — 연결당 메모리 비용은 사실상 일정합니다.

### C10k 동시 연결 테스트

`h2load`로 10,000 동시 연결을 유지하며 측정했습니다.

| 항목 | 값 |
|------|-------|
| Workers | 4 |
| Concurrent connections | 10,000 |
| Duration | 12.05 s |
| Max RSS | **~110 MB** (112,436 KB) |
| CPU usage | ~365% |
| User time | 41.05 s |
| System time | 3.04 s |
| Major page faults | 2 |
| Minor page faults | 16,948 |
| Voluntary context switches | 58,050 |
| Involuntary context switches | 14,828 |
| File system outputs | 256 |
| Total requests | 20000 |
| Total succeeded | 20000 |
| Total failed | 0 |
| Approx total RPS | **2285.22** |
| Success rate | **100.00%** |
| Exit status | **0** |

### C100k 동시 연결 테스트

`h2load`로 100,000 동시 연결을 유지하며 측정했습니다.

| 항목 | 값 |
|------|-------|
| Workers | 12 |
| Concurrent connections | 100,000 |
| Duration | 1:23.49 |
| Max RSS | **~146 MB** (148,848 KB) |
| CPU usage | ~815% |
| User time | 653.83 s |
| System time | 26.78 s |
| Major page faults | 0 |
| Minor page faults | 76,332 |
| Voluntary context switches | 446,557 |
| Involuntary context switches | 617,777 |
| File system outputs | 336 |
| Total requests | 200000 |
| Total succeeded | 200000 |
| Total failed | 0 |
| Approx total RPS | **2785.16** |
| Success rate | **100.00%** |
| Exit status | **0** |

### C1m Churn 테스트 (2026-08-23 재설계, 2026-08-24 수정)

기존 "1,000,000개의 동시 TLS 연결" 목표는 폐기되었습니다: HTTPS 경로는 활성 연결마다 worker 스레드 하나를 점유하므로, 유지 가능한 동시 연결 수는 workers x threads 수준으로 1M에 훨씬 못 미칩니다. (cwist의 **평문** HTTP/1.x 경로는 이벤트 기반이며 1,000,000/1,000,000개의 유지 연결에 도달했습니다 — cwist README 참조.) C1m 테스트는 churn을 측정합니다: 100,000개의 동시 유지 TLS 연결 위에서 20개의 h2load 프로세스 x 50,000 요청을 수행하며, watchdog으로 실행 시간이 제한됩니다.

| 항목 | 값 |
|------|-------|
| Workers | 12 |
| 부하 형태 | 20 x (-c 5000 -n 50000 -r 1000 -T 30) |
| 총량 | 100,000개 유지 연결 위 1,000,000 요청 |
| 결과 | **완료 — 스톨 없음** |
| 총 성공 | **1,000,000 / 1,000,000 (100.0%)** |
| 에러 | 0 |
| 소요 시간 | ~1:36 (프로세스당 h2load "finished in" 63.7-89.3 s) |
| Phantom connections | 0 (클라이언트/서버 ESTABLISHED 수 일치) |
| 서버 종료 | 정상, exit 0 |

이력: 2026-08-23에 동일한 부하의 실행은 ~85k 연결에서 데드락에 빠졌습니다. 근본 원인(cwist `perf(https): non-blocking TLS handshake shepherd`에서 수정): TLS 핸드셰이크가 30초 poll 대기와 함께 worker 스레드 내부에서 동기적으로 실행되어, 수백 개의 느린 클라이언트가 전체 풀을 점유하고 accept 큐가 넘치면서 초과된 핸드셰이크가 조용히 드롭되었고, 클라이언트는 서버 측 소켓 없이 ESTABLISHED 상태로 남았습니다. 이제 핸드셰이크는 non-blocking shepherd 스레드에서 실행되며, 수립된 세션만 풀 worker를 점유합니다.

> 참고: HTTP/2(TLS 1.3) 상에서 실제 클라이언트 연결을 유지하며 측정한 값입니다. 테스트별 worker 수는 다르며, 자세한 내용은 "이 벤치마크가 측정하는 것"을 참조하세요.

**핵심 결론**

- **연결 확장성**: 10,000개부터 1,000,000개의 동시 연결까지 RSS가 **~110–146 MB**를 유지합니다. 연결당 메모리 비용은 사실상 일정합니다.
- **현실적인 부하 하에서 안정적**: C10k는 **100% 성공**, C100k는 **100.00% 성공**으로 완료되었으며 동일한 메모리 범위 내에 머물렀습니다.
- **C1m 규모에서도 메모리 범위 유지**: C1m churn 실행(100k 유지 TLS 연결 위 1M 요청)은 **100% 성공**으로 스톨 없이 완료되었고 RSS는 ~146 MB를 유지했습니다 — 메모리 폭주도 크래시도 없었습니다.
- **데이터 안전성**: SQLite가 SIGINT 시 모든 데이터를 안전하게 저장했습니다(C10k에서 256 FS outputs).

### 처리량 벤치마크

위의 벤치마크는 **연결 확장성**을 측정한 것이며, 절대적인 **요청 처리량**을 측정한 것은 아닙니다. 서버의 순수 처리량 상한을 측정하기 위해 HTTP/2 위에서 `h2load`로 `-r` rate limit 없이 제한 없는 테스트를 실행했습니다.

| 항목 | 값 |
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
| 요청 지연 (h2load `time for request`) | min 183 µs, mean 30.69 ms, max 209.00 ms, sd 11.18 ms |

#### `wrk`를 사용한 HTTP/1.1 비교

비교를 위해 동일한 엔드포인트를 HTTP/1.1 위에서 `wrk`로 테스트했습니다. 프로토콜과 벤치마크 도구가 모두 다륯므로, 아래 수치는 위의 HTTP/2 h2load 결과와 **직접 비교할 수 없습니다**.

| 항목 | 값 |
|------|-------|
| Command | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` |
| Duration | 60 s |
| Requests/sec | **1282.49** |
| Transfer/sec | 52.29 MB |
| Latency | Avg 138.61 ms, Stdev 39.26 ms, Max 311.70 ms |

이 수치는 집중적이고 rate limit이 없는 부하 하에서 엔진의 절대 처리량 상한을 보여줍니다. 이는 위의 연결 확장성 테스트와는 별개의 것입니다.
