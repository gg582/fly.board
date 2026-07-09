# 잠재적 메모리 할당 실패 복구 전략

> fly.board C 코드베이스의 `malloc`/`calloc`/`realloc`/`strdup` 및 `cwist_alloc` 계열 할당 실패에 대한 복구 전략

## 1. 현재 상태 요약

코드베이스에는 세 가지 메모리 할당 패밀리가 혼용되어 있으며, 대부분의 할당 지점에서 실패(`NULL` 반환)을 검사하지 않습니다.

| 할당 패밀리 | 주요 사용처 | 현재 OOM 대응 |
|-------------|-------------|---------------|
| `cwist_alloc` / `cwist_sstring_*` | `src/` 전반 (~1,195여 호출) | 대부분 미검사. `cwist_sstring_append()` 반환값도 무시됨 |
| 표준 C (`malloc`/`calloc`/`realloc`/`strdup`) | `src/render/`, `src/utils/image_inline.c`, `src/utils/legal.c`, profile-pic 등 | 일부만 `NULL` 검사, 나머지는 즉시 역참조 위험 |
| SQLite/cJSON 내部分配 | `src/db/*` | `SQLITE_NOMEM` 미전파, cJSON 실패 무시 |

특히 위험한 영역:

- `src/utils/utils.c` — multipart form 파싱 (`mp_flush`, `mp_data`): `cwist_alloc`/`realloc` 결과를 검사하지 않고 `memset`/`strcpy`
- `src/handlers/post.c` — 게시글 생성/수정: 제목, 본문, 미디어 메타데이터 할당 후 즉시 사용
- `src/handlers/auth.c` — 계정 설정: `boundary`, `nickname`, `bio`, 프로필 사진 URL 할당 미검사
- `src/render/` — HTML 렌더링: `cwist_sstring` 생성/append 실패 무시로 잘린 응답 전송 가능
- `src/handlers/tasfa/` — 대용량 파일/암호화: XOR 복구 버퍼, 세션 데이터, 복호화 버퍼 등 다수 미검사
- `src/db/db.c` — `db_sqlite3_rows_to_json()`: cJSON 내部分配 실패 시 `SQLITE_NOMEM` 미반환

## 2. 복구 전략 원칙

1. **Fail-fast for init, graceful degradation for request**
   - 서버 초기화 단계의 치명적 할당 실패는 즉시 종료(`EXIT_FAILURE`) — 복구 불가
   - HTTP 요청 처리 중 할당 실패는 해당 요청에 대해 `500 Internal Server Error`를 본문 없이 반환하고 연결을 정리

2. **Never dereference unchecked allocation result**
   - 모든 `malloc`/`calloc`/`realloc`/`strdup`/`cwist_alloc` 결과는 사용 전 `NULL` 검사
   - `realloc`은 새 버퍼 확인 전에 기존 포인터를 덮어쓰지 않음

3. **Central allocator contract**
   - `cwist_alloc` 계열을 표준 래퍼로 일원화하거나, 실패 시 즉시 `CWIST_LOG_ERROR` + cleanup을 보장하는 helper 사용
   - 표준 C 할당은 가능한 한 `cwist_alloc`으로 대체하거나, 실패 처리 래퍼(`xmalloc` vs `try_alloc`)로 감쌈

4. **Request-scoped cleanup**
   - 각 HTTP 요청은 `request_cleanup` 또는 `goto cleanup` 패턴으로 모든 임시 버퍼 해제 보장
   - 부분적으로 실패한 multipart 필드, cJSON 객체, sstring은 `cleanup` 레이블에서 안전하게 해제

5. **Propagate allocation errors**
   - DB 함수는 cJSON 실패를 `SQLITE_NOMEM`으로 전파
   - render 함수는 `cwist_sstring_append` 실패를 상위로 전파
   - handler는 할당 실패 시 즉시 `cwist_http_response_status_set(req, 500)` 후 return

## 3. 단계별 실행 계획

### Phase 1: 치명적 취약점 제거 (즉시)

목표: 서버 충돌/NULL 역참조를 막는 최소한의 방어

- [ ] `src/utils/utils.c`의 `mp_flush()`, `mp_data()`에 모든 `cwist_alloc`/`realloc` NULL 검사 추가
  - 실패 시 `ctx->error = 1` 설정 후 현재까지 할당된 필드 해제
- [ ] `src/handlers/post.c`의 `handler_post_create()`, `handler_post_edit()`에서 모든 `cwist_alloc`/`strdup` 결과 검사
  - 실패 시 이미 파싱된 필드만 해제하고 `500` 반환
- [ ] `src/handlers/auth.c`의 `handler_account_settings_post()`에서 `boundary`, `nickname`, `bio`, 프로필 URL 할당 검사
- [ ] `src/handlers/tasfa/crypto.c`, `htp.c`, `session.c`의 민감한 버퍼 할당 검사
- [ ] 모든 `strdup` 호출 19곳에 NULL 검사 추가

### Phase 2: Allocator 래퍼 및 일관된 에러 전파 (단기)

목표: 코드 전체에서 반복되는 보일러플레이트 제거 및 실패 처리 일관성 확보

- [ ] `src/utils/alloc.h` / `src/utils/alloc.c` 신규 작성
  ```c
  // 실패 시 로그 남기고 NULL 반환 — 요청 처리에서 사용
  void *try_alloc(size_t size);
  void *try_calloc(size_t nmemb, size_t size);
  void *try_realloc(void *ptr, size_t size);
  char *try_strdup(const char *s);

  // 초기화 단계 등 복구 불가능한 경로에서 사용
  void *xalloc(size_t size);          // 실패 시 abort/exit
  void *xcalloc(size_t nmemb, size_t size);
  char *xstrdup(const char *s);
  ```
- [ ] 표준 C 할당 지점을 `try_*`/`x*` 래퍼로 마이그레이션
- [ ] `cwist_sstring_create()`/`cwist_sstring_append()` 결과를 항상 검사하는 매크로/인라인 helper 도입
  ```c
  #define SS_APPEND(ss, fmt, ...) \
      do { if (cwist_sstring_appendf((ss), (fmt), ##__VA_ARGS__) != CWIST_OK) goto cleanup; } while(0)
  ```
- [ ] `src/db/db.c`의 `db_sqlite3_rows_to_json()`에서 cJSON 생성 실패 검사 후 `SQLITE_NOMEM` 반환

### Phase 3: Graceful Degradation 및 모니터링 (중기)

목표: 메모리 부족 상황에서도 서버 전체가 아닌 개별 요청만 실패하도록 개선

- [ ] 요청 스코프 arena allocator 도입 검토
  - 요청 시작 시 `arena_init()`, 요청 종료 시 `arena_free_all()`
  - multipart 필드, 쿼리 파라미터, 렌더링 중간 버퍼를 arena에서 할당하면 부분 실패 시 전체를 폐기
- [ ] 메모리 압력 감지:
  - `mallinfo()`/`malloc_info()` 또는 `/proc/meminfo` 기반 부하 상태 확인
  - 활성 요청 수가 임계값을 초과하거나 가용 메모리가 낮으면 요청을 일찍 거부(`503 Service Unavailable`)
- [ ] 대용량 요청 사전 차단:
  - `Content-Length`가 설정된 요청이 서버 가용 메모리의 일정 비율을 초과하면 `413 Payload Too Large`
- [ ] `malloc_trim(0)` 호출 주기 개선 (현재 `global_middleware()`에서 5초 단위로 이미 수행 중)

## 4. 구체적 구현 가이드라인

### 4.1 표준 C 할당 처리

**안티패턴:**
```c
char *buf = malloc(size);
memcpy(buf, src, size);   // buf가 NULL이면 UB
```

**권장:**
```c
char *buf = try_alloc(size);
if (!buf) {
    CWIST_LOG_ERROR("Out of memory allocating %zu bytes", size);
    goto cleanup;
}
memcpy(buf, src, size);
```

### 4.2 `realloc` 안전 처리

**안티패턴:**
```c
ptr = realloc(ptr, new_size);  // 실패 시 원래 ptr 손실
```

**권장:**
```c
void *tmp = realloc(ptr, new_size);
if (!tmp) {
    // ptr은 여전히 유효
    goto cleanup;
}
ptr = tmp;
```

### 4.3 `cwist_sstring` 처리

**안티패턴:**
```c
cwist_sstring_t *ss = cwist_sstring_create();
cwist_sstring_assign(ss, "...");   // ss가 NULL이면 UB
cwist_sstring_append(ss, "...");   // 반환값 무시
```

**권장:**
```c
cwist_sstring_t *ss = cwist_sstring_create();
if (!ss || cwist_sstring_assign(ss, "...") != CWIST_OK) {
    cwist_sstring_destroy(ss);
    goto cleanup;
}
if (cwist_sstring_append(ss, "...") != CWIST_OK) {
    cwist_sstring_destroy(ss);
    goto cleanup;
}
```

### 4.4 cJSON 할당 실패 전파

**안티패턴:**
```c
cJSON_AddStringToObject(row, "name", name);
// 내部分配 실패를 감지하지 못함
```

**권장:**
```c
if (!cJSON_AddStringToObject(row, "name", name)) {
    cJSON_Delete(row);
    *err = SQLITE_NOMEM;
    return NULL;
}
```

### 4.5 Handler에서의 복구

**권장 패턴:**
```c
static void handler_foo(cwist_http_request_t *req, cwist_http_response_t *resp) {
    char *title = NULL, *body = NULL;
    int status = 500;

    title = cwist_alloc(title_len + 1);
    if (!title) goto cleanup;

    body = cwist_alloc(body_len + 1);
    if (!body) goto cleanup;

    // ... 비즈니스 로직 ...
    status = 200;

cleanup:
    cwist_free(title);
    cwist_free(body);
    if (status != 200) {
        cwist_http_response_status_set(req, 500);
        cwist_http_response_body_set(req, NULL, 0);
    }
}
```

## 5. 우선순위 높은 수정 대상

| 우선순위 | 파일 | 이유 |
|----------|------|------|
| P0 | `src/utils/utils.c` (`mp_flush`, `mp_data`) | multipart 파싱은 모든 파일 업로드 요청의 입구 |
| P0 | `src/handlers/post.c` (`handler_post_create`, `handler_post_edit`) | 핵심 기능, 사용자 입력 직접 처리 |
| P0 | `src/handlers/auth.c` (`handler_account_settings_post`) | 인증/계정 데이터 누수 위험 |
| P1 | `src/handlers/tasfa/crypto.c`, `htp.c`, `session.c` | 대용량 파일/암호화 — 메모리 압력 시 취약 |
| P1 | `src/db/db.c` (`db_sqlite3_rows_to_json`) | cJSON 실패가 상위로 전파되지 않음 |
| P1 | `src/render/render_page.c`, `render_md.c`, `render_board.c`, `render_post.c` | 잘린 HTML 응답 전송 가능 |
| P2 | `src/handlers/board.c`, `src/handlers/handlers.c` (profile pic) | unchecked `strdup` |
| P2 | `src/utils/cache.c` | 이미 잘 보호되어 있으나 표준 `malloc`과의 일관성 검토 |

## 6. 검증 및 테스트 방안

1. **정적 분석**
   - `clang --analyze` 또는 `cppcheck`로 `NULL` 역참조 경고 최소화
   - 커스텀 grep: `malloc\|calloc\|realloc\|strdup\|cwist_alloc\|cwist_sstring_create` 호출 후 바로 `if (!` 또는 `== NULL` 검사 여부

2. **단위 테스트**
   - `tests/`에 메모리 할당 실패를 시뮬레이션하는 fake allocator 추가
   - 핵심 함수(`mp_flush`, `handler_post_create`, DB JSON 변환)마다 OOM 시나리오 테스트

3. **런타임 테스트**
   - `ulimit -v`로 가상 메모리 제한 후 서버 부하 테스트
   - 큰 파일 업로드, 큰 게시글 생성, 동시 다수 요청 시 `500` 응답과 서버 생존 확인

4. **모니터링**
   - `CWIST_LOG_ERROR`에 OOM 로그 카테고리 추가
   - 메모리 사용량/활성 요청 수 메트릭 노출

## 7. 결론

현재 fly.board는 대부분의 메모리 할당 실패를 검사하지 않아 메모리 부족 상황에서 충돌, 잘린 응답, 또는 데이터 누수가 발생할 수 있습니다. 가장 먼저 **Phase 1의 핵심 지점 NULL 검사**를 적용하여 충돌을 방지하고, 이어서 ** allocator 래퍼**와 **request-scoped cleanup**으로 일관된 복구 메커니즘을 구축해야 합니다. 장기적으로는 메모리 압력 기반 요청 거부와 arena allocator 도입을 검토합니다.
