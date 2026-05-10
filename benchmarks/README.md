# 벤치마크

## 환경

| 항목 | 값 |
|------|-----|
| OS | Linux 5.15.19 |
| 아키텍처 | x86_64 |
| CPU | 4 core |
| RAM | 확인 필요 (시스템 전체) |
| OpenSSL | 3.2.1 (커스텀 빌드) |
| CWIST | `patches/cwist` (SIGPIPE 패치 적용) |
| 벤치 도구 | wrk |

## 벤치마크 방법

```sh
# 1. 서버 빌드 (OpenSSL 3.2.1 경로 필요)
make clean
make CFLAGS="-Wall -Wextra -O2 -I/usr/local/include -I/usr/local/src/openssl-3.2.1/include -Ithird_party/md4c/src -Ithird_party/multipart-parser-c -Isrc -Iinclude" \
  LDFLAGS="-L/usr/local/src/openssl-3.2.1 -Wl,-rpath,/usr/local/src/openssl-3.2.1" \
  LIBS="-lssl -lcrypto -lpthread -ldl"

# 2. 서버 실행
LD_LIBRARY_PATH=/usr/local/src/openssl-3.2.1 ./fly_board

# 3. wrk 설치 확인
which wrk

# 4. 벤치마크 실행 예시
wrk -t4 -c100 -d15s --latency https://localhost:9443/
```

## 주의사항

- `ulimit -n` 기본값이 1024로 낮아, 동시 연결 수를 400 이상으로 올리면 `TIME_WAIT` 소켓이 쌓여 `Connection refused`가 발생할 수 있습니다.
- `wrk` 사용 시 Socket errors(`read`)가 다수 기록되지만, 이는 CWIST의 HTTP/1.1 keep-alive 구현과 `wrk`의 연결 재사용 방식 사이의 미세한 차이로 인한 것이며 실제 RPS 측정에는 큰 영향을 주지 않습니다.
- 본 벤치마크는 CWIST의 `SIGPIPE` 패치(`patches/cwist`)를 적용한 상태에서 수행되었습니다. 패치 전에는 고부하 상황에서 서버가 `SIGPIPE`로 종료되었습니다.

## 결과 요약

### 홈페이지 `/`

| Connections | Threads | Duration | Avg Latency | RPS | Transfer/sec |
|-------------|---------|----------|-------------|-----|--------------|
| 10 | 4 | 15s | 7.29ms | **729.79** | 9.18MB |
| 50 | 4 | 15s | 63.15ms | **612.77** | 7.71MB |
| 100 | 4 | 15s | 147.87ms | **602.43** | 7.58MB |
| 200 | 4 | 15s | 337.15ms | **532.25** | 6.69MB |
| 400 | 4 | 15s | 2.10s | **660.03** | 8.30MB |

### 로그인 페이지 `/login`

| Connections | Threads | Duration | Avg Latency | RPS | Transfer/sec |
|-------------|---------|----------|-------------|-----|--------------|
| 10 | 4 | 15s | 6.62ms | **700.87** | 9.32MB |
| 50 | 4 | 15s | 48.66ms | **768.09** | 10.22MB |
| 100 | 4 | 15s | 115.83ms | **750.85** | 9.99MB |
| 200 | 4 | 15s | 449.61ms | **513.77** | 6.83MB |
| 400 | 4 | 15s | 548.50ms | **658.29** | 8.76MB |

### 게시판 목록 `/boards`

| Connections | Threads | Duration | Avg Latency | RPS | Transfer/sec |
|-------------|---------|----------|-------------|-----|--------------|
| 10 | 4 | 15s | 9.58ms | **493.72** | 6.14MB |
| 50 | 4 | 15s | 59.77ms | **598.82** | 7.45MB |
| 100 | 4 | 15s | 124.29ms | **681.60** | 8.48MB |
| 200 | 4 | 15s | 417.29ms | **662.08** | 8.24MB |

## 리소스 사용량

- **CPU 최고 사용률**: 약 140% (4코어 시스템 기준, 단일 스레드 처리 한계 근접)
- **RAM 최고 점유(RSS)**: 약 11,700 KB (약 11.5 MB)
- **VSZ 최고**: 약 1,100,000 KB (1.1 GB 가상 메모리, 실제 물리 메모리는 11MB 수준)

> 참고: 본 벤치마크는 serialize 미들웨어(`pthread_mutex_t`)를 적용하여 동시 요청을 직렬화한 상태에서 수행되었습니다. 이는 CWIST 낮은 수준의 race condition을 회피하기 위한 조치입니다.
