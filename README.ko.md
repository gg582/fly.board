# fly.board

![fly.board logo](img/logo.png)

> 연결이 증가할 때 메모리를 거의 일정하게 유지하는 몇 안 되는 심플한 블로그 엔진입니다: idle 시 **~82 MB RSS**(4 workers; single worker 운영 시 실제 프로덕션 서버에서 **68–120 MB** 유지), 그리고 C10k, C100k, 심지어 C1m에서도 **~146 MB**를 유지합니다.  
> C 기반 CWIST 웹 프레임워크 위에 구축된 가벼운 게시판 겸 블로그 엔진으로, HTTPS/3, Argon2id, PQC 서명, NATS 메시징을 지원합니다.

## 특징

- **메모리 효율 및 연결 확장성** – 스택+힙 C 구현. idle 시 **~82 MB RSS**; C10k부터 C1m 동시 연결까지 RSS가 **~146 MB**를 유지합니다.
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

- `blog.settings` – 블로그 제목, 부제목, 푸터, 포트, 업로드 제한
- `admin.settings` – 관리자 계정 (2줄: `username`\n`password`)

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

각 테스트를 현실적으로 유지하기 위해 worker 수를 부하에 맞게 조정했습니다: C10k는 **4 workers**, C100k는 **12 workers**, C1m은 **24 workers**입니다. 이는 세 번의 실행에서 다른 CPU 사용률 수치를 보이는 이유이기도 합니다.

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
| CWIST | `/usr/local/lib/libcwist.a` |

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
| Idle | **~82 MB** (83,708 KB) | — | 4 workers, no connections |
| C10k | **~146 MB** (145,928 KB) | +62.22 MB | 10,000 concurrent connections |
| C100k | **~146 MB** (146,076 KB) | +148 KB | 100,000 concurrent connections |
| C1m | **~146 MB** (146,420 KB) | +344 KB | 1,000,000 concurrent connections |

C10k에서 C1m까지의 총 RSS 증가량은 **약 492 KB**에 불과합니다 — 사실상 노이즈 수준입니다. 이것이 이 벤치마크에서 가장 중요한 결과입니다.

### C10k 동시 연결 테스트

`h2load`로 10,000 동시 연결을 유지하며 측정했습니다.

| 항목 | 값 |
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

### C100k 동시 연결 테스트

`h2load`로 100,000 동시 연결을 유지하며 측정했습니다.

| 항목 | 값 |
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

### C1m 동시 연결 테스트

`h2load`로 1,000,000 동시 연결을 유지하며 측정했습니다.

| 항목 | 값 |
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

> 참고: HTTP/2(TLS 1.3) 상에서 실제 클라이언트 연결을 유지하며 측정한 값입니다. 테스트별 worker 수는 다르며, 자세한 내용은 "이 벤치마크가 측정하는 것"을 참조하세요.

**핵심 결론**

- **연결 확장성**: 10,000개부터 1,000,000개의 동시 연결까지 RSS가 **~146 MB**를 유지합니다. 연결당 메모리 비용은 사실상 일정합니다.
- **현실적인 부하 하에서 안정적**: C10k와 C100k는 동일한 메모리 범위 내에서 **100% 성공**으로 완료되었습니다.
- **C1m에서도 메모리 범위 유지**: 테스트 하드웨어가 1,000,000개 연결을 모두 처리하지 못했을 때(36.14% 성공)에도 메모리 사용량은 본질적으로 변하지 않았습니다 — 서버가 통제 불능 상태로 빠지지 않았습니다.
- **데이터 안전성**: SQLite가 SIGINT 시 모든 데이터를 안전하게 저장했습니다(C10k에서 10,600 FS outputs).

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

비교를 위해 동일한 엔드포인트를 HTTP/1.1 위에서 `wrk`로 테스트했습니다:

| 항목 | 값 |
|------|-------|
| Command | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` |
| Duration | 60 s |
| Requests/sec | **1282.49** |
| Transfer/sec | 52.29 MB |

이 수치는 집중적이고 rate limit이 없는 부하 하에서 엔진의 절대 처리량 상한을 보여줍니다. 이는 위의 연결 확장성 테스트와는 별개의 것입니다.
