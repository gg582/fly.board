# fly.board

![fly.board logo](img/logo.png)

> 연결이 증가할 때 메모리를 거의 일정하게 유지하는 몇 안 되는 심플한 블로그 엔진입니다: idle 시 **~104 MB RSS**(4 workers; single worker 운영 시 실제 프로덕션 서버에서 **68–120 MB** 유지), 그리고 C10k, C100k, 심지어 C1m에서도 **~110–146 MB**를 유지합니다.
> C 기반 CWIST 웹 프레임워크 위에 구축된 가벼운 게시판 겸 블로그 엔진으로, HTTPS/3, Argon2id, PQC 서명, NATS 메시징을 지원합니다.

## 특징

- **메모리 효율 및 연결 확장성** – 스택+힙 C 구현. idle 시 **~104 MB RSS**; C10k부터 C1m 동시 연결까지 RSS가 **~110–146 MB**를 유지합니다.
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
| Idle | **~104 MB** (106,192 KB) | — | 4 workers, no connections |
| C10k | **~110 MB** (112,436 KB) | +6,244 KB | 10,000 concurrent connections |
| C100k | **~146 MB** (148,848 KB) | +36,412 KB | 100,000 concurrent connections |
| C1m churn | **~146 MB** (149,816 KB) | +968 KB | 100k held TLS conns, 1M-request churn |

C100k에서 C1m churn 실행까지의 총 RSS 변화량은 **+968 KB**입니다 — 사실상 측정 노이즈 수준입니다. 이것이 이 벤치마크에서 가장 중요한 결과입니다.

RSS 값은 서버 프로세스에 대해 `/usr/bin/time -v`가 보고한 **Maximum resident set size (kbytes)**입니다.

### 메모리 비용

| 전환 | Δ RSS | Δ 연결 수 | 연결당 대략적 비용 |
|---|---|---|---|
| Idle → C10k | +6,244 KB | 10,000 | 연결당 ~0.6 KB |
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
| 대략적 백분위 지연* | p50 ~30.7 ms, p95 ~49.1 ms, p99 ~56.7 ms |

\* 백분위는 보고된 평균과 표준편차로 추정한 값입니다. h2load 기본 출력은 min/max/mean/sd만 제공하며, 정확한 백분위 히스토그램을 위해서는 `--latency-collect` 옵션을 사용하세요.

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
