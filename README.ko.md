# fly.board

![fly.board logo](img/logo.png)

> **8–16 MB RSS**로 동작하는 몇 안 되는 심플 블로그 계통.  
> C 기반 CWIST 웹 프레임워크 위에 HTTPS/3, Argon2id, PQC 서명, NATS 메시징을 올린 가벼운 게시판 겸 블로그 엔진입니다.

## 특징

- **메모리 절약** – 스택+힙 기반 C 구현. 운영 환경에서 RSS가 **8–16 MB** 수준에 머무릅니다.
- **최신 전송 계층** – TLS 1.3 + HTTP/3(QUIC) 기본 지원. 선택적 ECH(Encrypted Client Hello).
- **안전한 인증** – 클라이언트 사이드 SHA-512 프리해시 + 서버 사이드 **Argon2id** (OpenSSL 3 KDF). JWT 세션 쿠키.
- **게시판 / 블로그 하이브리드** – 슬러그 기반 마크다운 포스트 + 다중 게시판(Board) + 댓글(계층형).
- **실시간 미리보기** – 마크다운 에디터에서 입력 즉시 서버 프리뷰.
- **PQC 서명** – 게시글에 양자 후 암호(PQC) 기반 서명을 첨부/검증.
- **파일 저장소** – 1 MB 이하는 SQLite, 초과는 볼륨 기반 저장. 이미지/비디오/오디오 자동 임베드.
- **NATS 연동** – `NATS_URL` 환경변수로 분산 메시징 게이트웨이 연결.
- **다크모드** – 쿠키 기반 테마 전환 + 동적 CSS 변수.

## 빌드

```sh
make
./keygen.sh
```

의존성:
- [CWIST](https://github.com/religiya-serdtsa/cwist)
- OpenSSL 3.x (Argon2id KDF, TLS 1.3, QUIC)
- ngtcp2 / nghttp3 (HTTP/3)
- cJSON, SQLite3

`Makefile`은 `third_party/md4c`를 클론/빌드하여 정적 라이브러리로 링크합니다.

## 실행

```sh
./fly_board
```

기본 포트는 `blog.settings`의 `port` 값(기본 9443)을 따릅니다.

```text
https://localhost:9443
```

HTTP/3는 동일 포트의 UDP로 수신합니다.

### ECH 활성화 (선택)

```sh
BLOG_ECH_KEY=ech/server.ech ./fly_board
# 또는
BLOG_ECH_DIR=ech ./fly_board
```

서버 ECH를 지원하지 않는 OpenSSL 빌드라면 경고 로그 후 일반 HTTPS/3로 계속 실행됩니다.

### NATS 연동 (선택)

```sh
NATS_URL=nats://localhost:4222 ./fly_board
```

## 주요 기능

| 기능 | 경로 | 설명 |
|------|------|------|
| 홈 | `/` | 최신 포스트 목록 |
| 게시판 | `/boards` | 다중 게시판 관리 (admin-only 지원) |
| 게시글 | `/post/:slug` | md4c 마크다운 렌더링 + 댓글 + 첨부파일 |
| 로그인/가입 | `/login`, `/register` | Argon2id + JWT 쿠키 |
| 프로필 | `/profile` | 닉네임, 바이오, 프로필 사진, 가입일 |
| 계정 설정 | `/account/settings` | 프로필 수정 |
| 비밀번호 변경 | `/account/password` | 현재 비밀번호 확인 후 Argon2id 재해싱 |
| 관리자 | `/admin/users` | 사용자 역할 변경, 삭제 |
| 파일 저장소 | `/files` | 업로드/다운로드/삭제 |

## 설정 파일

- `blog.settings` – 블로그 타이틀, 서브타이틀, 푸터, 포트
- `admin.settings` – 관리자 계정 (2줄: `username`\n`password`)

## 데이터베이스

SQLite3 (`data/blog.db`) 기반. 스키마는 앱 시작 시 자동 마이그레이션됩니다.

```
users       – 계정, Argon2id 해시, 역할, 프로필
boards      – 게시판 이름/슬러그/설명/admin_only
posts       – 마크다운 본문, PQC 서명, 요약
files       – 첨부 파일 경로/크기/MIME
comments    – 계층형 댓글 (target_type, parent_id)
board_permissions – 비공개 게시판 접근 권한
```

## 아키텍처 요약

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

## 성능 벤치마크

> 상세한 벤치마크 방법과 전체 결과는 [`benchmarks/README.md`](benchmarks/README.md)를 참조하세요.

### 호스트 환경

| 항목 | 값 |
|------|-----|
| OS | Linux 7.0.0-mountain+ |
| 아키텍처 | x86_64 |
| CPU | AMD Ryzen 5 5600X @ 3.70GHz (6 cores / 12 threads) |
| RAM | 64 GB |
| 디스크 | Samsung SSD 980 1TB (NVMe) |
| OpenSSL | 3.5.5 |
| 벤치 도구 | wrk |
| CWIST | `patches/cwist` |

### 최대 처리량 (RPS)

`wrk -t4 -c400 -d30s` (TLS 1.3, 직렬화 없음)

| 엔드포인트 | 최고 RPS | 평균 지연시간 | 설명 |
|-----------|----------|--------------|------|
| `/` (홈) | **3,409.92** | 121.84ms | DB 쿼리 + 마크다운 렌더링 |
| `/login` | **3,948.77** | 18.03ms | 정적 폼 (캐시 가능) |
| `/boards` | **3,901.77** | 17.26ms | DB 기반 목록 |

### 리소스 사용량 (최고 부하 시)

| 항목 | 값 |
|------|-----|
| CPU 사용률 | 약 600% (12스레드 기준) |
| RAM 점유 (RSS) | 약 12 MB |
| 가상 메모리 (VSZ) | 약 1.2 GB |

> 참고: 본 벤치마크는 동시 요청 직렬화(`pthread_mutex_t`) **없이** 수행되었습니다.  
> `ulimit -n`은 20,000으로 설정되어 있어 400 connections까지 안정적으로 측정 가능합니다.
