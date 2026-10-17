#include "engine/async_route.h"
#include "engine/bdr.h"
#include <cwist/core/log.h>
#include <cwist/core/mem/gc.h>
#include <cwist/net/http/async.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Every route handler runs on a request worker instead of the thread that
 * owns the connection. cwist parks a pool thread per TLS connection and one
 * reactor loop per pinned worker, so a handler that renders a page or waits
 * on SQLite there stalls every other connection it multiplexes; deferring
 * frees that thread for the handler's whole run.
 *
 * Hand-off ordering: cwist forbids touching req/res until dispatch has
 * unwound every middleware (the compress middleware still reads res->body
 * after next() returns). The route trampoline therefore only records the
 * job in a thread-local slot; the gate, the outermost middleware, queues it
 * after its own next() returns, when nothing on the dispatch thread will
 * touch the exchange again. */

typedef struct fly_async_route {
    cwist_handler_func handler;
    bool cacheable; /* Anonymous GET replies may be kept in the route BDR. */
} fly_async_route;

typedef struct fly_async_job {
    cwist_async *a;
    cwist_http_request *req;
    cwist_http_response *res;
    cwist_handler_func handler;
    bool mutating;      /* POST: stale the route BDR once the handler ran. */
    bool store;         /* Seed the route BDR with the finished reply. */
    uint64_t version;   /* BDR content version read before the route ran. */
    char key[ENGINE_BDR_KEY_MAX];
    int64_t created_ms; /* When the deferred job was created (queue wait). */
    int queue_depth;    /* Backlog length when the job was enqueued. */
    struct fly_async_job *next;
} fly_async_job;

static cwist_middleware_func g_compress = NULL;
static engine_async_hit_func g_hit_hook = NULL;
static cwist_db *g_db = NULL;
static engine_async_finish_func g_finish = NULL;

static pthread_mutex_t g_queue_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cond = PTHREAD_COND_INITIALIZER;
static fly_async_job *g_queue_head = NULL;
static fly_async_job *g_queue_tail = NULL;
static size_t g_queue_len = 0;
/* cwist_app_listen() forks the serving processes and threads do not survive
 * fork(), so workers are started lazily by the process that serves. */
static pid_t g_workers_pid = 0;
static bool g_workers_ok = false;

static __thread bool t_in_gate = false;
static __thread fly_async_job *t_pending = NULL;
/* Set by the gate for the current request, read by the trampoline. */
static __thread const char *t_bdr_key = NULL;
static __thread uint64_t t_bdr_version = 0;
static __thread bool t_route_cacheable = false;

static size_t worker_count(void) {
    const char *value = getenv("FLY_REQUEST_WORKERS");
    if (value && value[0]) {
        char *end = NULL;
        unsigned long parsed = strtoul(value, &end, 10);
        if (end && *end == '\0' && parsed >= 1 && parsed <= 256) return (size_t)parsed;
        FLY_LOG_ERROR("Ignoring invalid FLY_REQUEST_WORKERS value; expected 1..256");
    }
    return 16;
}

/* Slow-request observability: any stage (queue wait, worker run, inline
 * fallback) exceeding FLY_SLOW_REQUEST_MS (default 2000, <=0 disables) is
 * logged with a per-stage breakdown so a stall can be attributed to
 * backlog pressure, a slow handler/DB, or the inline fallback that runs
 * routes on the reactor thread. */
static int slow_threshold_ms(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("FLY_SLOW_REQUEST_MS");
        long parsed = (v && v[0]) ? strtol(v, NULL, 10) : 2000;
        cached = (parsed > 0 && parsed <= 3600000) ? (int)parsed : (parsed <= 0 ? 0 : 3600000);
    }
    return cached;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *method_name(const cwist_http_request *req) {
    switch (req->method) {
        case CWIST_HTTP_GET: return "GET";
        case CWIST_HTTP_POST: return "POST";
        case CWIST_HTTP_PUT: return "PUT";
        case CWIST_HTTP_DELETE: return "DELETE";
        case CWIST_HTTP_PATCH: return "PATCH";
        case CWIST_HTTP_HEAD: return "HEAD";
        case CWIST_HTTP_OPTIONS: return "OPTIONS";
        default: return "?";
    }
}

static void log_if_slow(int64_t started_ms, cwist_http_request *req, cwist_http_response *res,
                        const char *mode) {
    int threshold = slow_threshold_ms();
    int64_t elapsed = now_ms() - started_ms;
    if (threshold <= 0 || elapsed < threshold || !req) return;
    CWIST_LOG_WARN("slow request (%s): %s %s status=%d total=%lldms",
                   mode, method_name(req), req->path ? req->path->data : "?",
                   res ? res->status_code : 0, (long long)elapsed);
}

/* Backlog cap for the defer queue. Deep queues only delay the inevitable:
 * once the backlog outruns the workers' sustainable rate, latency explodes
 * while the site still looks "up". Beyond the cap (with workers alive) new
 * requests are rejected with 503 + Retry-After instead of falling back to
 * inline execution on the reactor thread, which would stall every
 * connection that thread multiplexes. */
static size_t queue_max(void) {
    static size_t cached = 0;
    if (cached == 0) {
        const char *v = getenv("FLY_ASYNC_QUEUE_MAX");
        long parsed = (v && v[0]) ? strtol(v, NULL, 10) : 0;
        size_t want = worker_count() * 16;
        if (want < 64) want = 64;
        if (want > 8192) want = 8192;
        if (parsed >= 64 && parsed <= 65536) want = (size_t)parsed;
        cached = want;
    }
    return cached;
}


static void noop_next(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)res;
}

static void run_job(fly_async_job *job) {
    if (!job->a) {
        /* BDR hit side effect: the route did not run, replay what it would
         * have recorded (key holds the request path). */
        if (g_hit_hook) g_hit_hook(g_db, job->key);
        free(job);
        return;
    }
    /* The gate queues the job before dispatch has unwound: until the
     * dispatch ack, cwist still reads res (res->deferred above all, which
     * the compress step below clears for a moment) and may send and free
     * it. Touch nothing before the ack. Inline runs (the gate's fallback)
     * are on the dispatch thread and do not wait. */
    int64_t start = now_ms();
    cwist_async_wait_dispatched(job->a);
    int64_t after_wait = now_ms();
    job->handler(job->req, job->res);
    /* Bump only after the write landed, so no reader re-caches old data
     * under the new version. */
    if (job->mutating) engine_bdr_bump();
    if (g_finish) g_finish(job->req, job->res);
    /* The compress middleware bails on res->deferred (in the middleware
     * chain that guards against a response another thread may be completing).
     * This job owns the exchange exclusively until cwist_async_respond_with()
     * below, so drop the flag for the duration of the compression call —
     * otherwise every deferred response reaches clients uncompressed. */
    bool was_deferred = job->res->deferred;
    job->res->deferred = false;
    if (g_compress) g_compress(job->req, job->res, noop_next);
    job->res->deferred = was_deferred;
    if (job->store) engine_bdr_store(job->req, job->res, job->key, job->version);
    if (!cwist_async_respond_with(job->a, job->res)) {
        FLY_LOG_ERROR("deferred response lost to another completion");
    }
    int threshold = slow_threshold_ms();
    int64_t total = now_ms();
    if (threshold > 0 && total - job->created_ms >= threshold) {
        CWIST_LOG_WARN("slow request (deferred): %s %s status=%d total=%lldms "
                       "(queue=%lldms run=%lldms depth=%d)",
                       method_name(job->req),
                       job->req->path ? job->req->path->data : "?",
                       job->res->status_code,
                       (long long)(total - job->created_ms),
                       (long long)(start - job->created_ms),
                       (long long)(total - after_wait),
                       job->queue_depth);
    }
    /* request_worker threads never exit; sweep this job's tracked leaks now
     * instead of letting them pile up on the thread's pending-sweep list. */
    cwist_gc_scope_flush();
    free(job);
}

static void *request_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_queue_mtx);
        while (!g_queue_head) pthread_cond_wait(&g_queue_cond, &g_queue_mtx);
        fly_async_job *job = g_queue_head;
        g_queue_head = job->next;
        if (!g_queue_head) g_queue_tail = NULL;
        g_queue_len--;
        pthread_mutex_unlock(&g_queue_mtx);
        run_job(job);
    }
    return NULL;
}

/* Called with g_queue_mtx held. */
static bool ensure_workers_locked(void) {
    pid_t pid = getpid();
    if (g_workers_pid == pid) return g_workers_ok;
    /* Inherited queue entries belong to the parent's connections. */
    g_queue_head = g_queue_tail = NULL;
    g_queue_len = 0;
    g_workers_pid = pid;
    g_workers_ok = false;
    size_t want = worker_count();
    size_t started = 0;
    for (size_t i = 0; i < want; i++) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, request_worker, NULL) != 0) break;
        pthread_detach(thread);
        started++;
    }
    if (started == 0) {
        FLY_LOG_ERROR("Failed to start request workers; handlers run inline");
        return false;
    }
    g_workers_ok = true;
    return true;
}

typedef enum { QUEUE_OK, QUEUE_NO_WORKERS, QUEUE_FULL } queue_state_t;

static queue_state_t queue_state(void) {
    pthread_mutex_lock(&g_queue_mtx);
    queue_state_t st;
    if (!ensure_workers_locked()) st = QUEUE_NO_WORKERS;
    else if (g_queue_len >= queue_max()) st = QUEUE_FULL;
    else st = QUEUE_OK;
    pthread_mutex_unlock(&g_queue_mtx);
    return st;
}

/* Reject instead of blocking: a full defer queue means the workers are
 * outrun; running the handler inline here would run it on the reactor
 * thread and stall every connection that thread multiplexes. */
static void respond_busy(cwist_http_request *req, cwist_http_response *res, const char *why) {
    res->status_code = CWIST_HTTP_SERVICE_UNAVAILABLE;
    cwist_sstring_assign(res->status_text, "Server busy");
    cwist_http_header_add(&res->headers, "Retry-After", "5");
    cwist_sstring_assign(res->body, "The server is momentarily overloaded; retry in a few seconds.");
    CWIST_LOG_WARN("async defer unavailable (%s): %s %s", why, method_name(req),
                   req->path ? req->path->data : "?");
}

static bool enqueue(fly_async_job *job) {
    pthread_mutex_lock(&g_queue_mtx);
    bool ok = ensure_workers_locked();
    if (ok) {
        job->next = NULL;
        if (g_queue_tail) g_queue_tail->next = job;
        else g_queue_head = job;
        g_queue_tail = job;
        g_queue_len++;
        job->queue_depth = (int)g_queue_len;
        pthread_cond_signal(&g_queue_cond);
    }
    pthread_mutex_unlock(&g_queue_mtx);
    return ok;
}

/* cwist completes deferred exchanges on the HTTP/1.x reactor and pool paths,
 * TLS, and HTTP/2; HTTP/3 and in-memory dispatch have no completion path. */
static bool can_defer(cwist_http_request *req) {
    /* handler_not_found re-dispatches HEAD as GET from inside cwist's error
     * path and touches res after the nested dispatch returns. */
    if (cwist_http_header_get(req->headers, "X-Fly-Head-Redispatch")) return false;
    if (req->version && req->version->data && strcmp(req->version->data, "HTTP/3") == 0) return false;
    return req->h2_queue || req->https_conn || req->async_conn || req->client_fd >= 0;
}

static void async_trampoline(void *ctx, cwist_http_request *req, cwist_http_response *res) {
    const fly_async_route *route = (const fly_async_route *)ctx;
    t_route_cacheable = route->cacheable;
    if (!t_in_gate || t_pending) {
        int64_t t0 = now_ms();
        route->handler(req, res);
        log_if_slow(t0, req, res, "inline");
        return;
    }
    /* HTTP/3 and in-memory dispatch have no deferred completion path: the
     * handler can only run on this thread (the slow-request log flags it
     * when such a run blocks). */
    if (!can_defer(req)) {
        int64_t t0 = now_ms();
        route->handler(req, res);
        log_if_slow(t0, req, res, "inline");
        return;
    }
    queue_state_t qs = queue_state();
    if (qs == QUEUE_NO_WORKERS) {
        /* Worker threads could not start at all: keep serving inline
         * (degraded) rather than hard-down every route. */
        int64_t t0 = now_ms();
        route->handler(req, res);
        log_if_slow(t0, req, res, "inline");
        return;
    }
    if (qs == QUEUE_FULL) {
        respond_busy(req, res, "queue full");
        return;
    }
    fly_async_job *job = calloc(1, sizeof(*job));
    if (!job) {
        respond_busy(req, res, "allocation failed");
        return;
    }
    job->a = cwist_async_defer(req, res);
    if (!job->a) {
        free(job);
        respond_busy(req, res, "defer failed");
        return;
    }
    job->req = req;
    job->res = res;
    job->handler = route->handler;
    job->created_ms = now_ms();
    job->mutating = req->method == CWIST_HTTP_POST;
    if (route->cacheable && t_bdr_key) {
        job->store = true;
        job->version = t_bdr_version;
        snprintf(job->key, sizeof(job->key), "%s", t_bdr_key);
    }
    t_pending = job;
}

static void async_gate(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    if (t_in_gate) {
        next(req, res);
        return;
    }
    char key[ENGINE_BDR_KEY_MAX];
    bool keyed = engine_bdr_key(req, key, sizeof(key));
    /* BDR hit: the stored reply already carries the final headers and the
     * encoded body, so neither the middleware nor the route runs. */
    if (keyed && engine_bdr_serve(req, res, key)) {
        if (g_hit_hook && req->method == CWIST_HTTP_GET &&
            !cwist_http_header_get(req->headers, "X-Fly-Head-Rewrite")) {
            fly_async_job *hit = calloc(1, sizeof(*hit));
            if (hit) {
                snprintf(hit->key, sizeof(hit->key), "%s", req->path->data);
                if (!enqueue(hit)) free(hit);
            }
        }
        return;
    }

    t_in_gate = true;
    t_bdr_key = keyed ? key : NULL;
    t_bdr_version = keyed ? engine_bdr_version() : 0;
    t_route_cacheable = false;
    int64_t gate_start = now_ms();
    next(req, res);
    t_in_gate = false;
    t_bdr_key = NULL;
    fly_async_job *job = t_pending;
    t_pending = NULL;
    if (!job) {
        /* The route ran inline. */
        log_if_slow(gate_start, req, res, "inline");
        if (req->method == CWIST_HTTP_POST) engine_bdr_bump();
        else if (keyed && t_route_cacheable && res->status_code < 500)
            engine_bdr_store(req, res, key, t_bdr_version);
        return;
    }
    /* Workers vanished between the room check and now: finish inline.
     * cwist holds an inline completion until dispatch acknowledges. */
    if (job && !enqueue(job)) run_job(job);
}

void engine_async_init(cwist_app *app, cwist_middleware_func compress,
                       engine_async_finish_func finish) {
    g_compress = compress;
    g_finish = finish;
    g_db = app ? app->db : NULL;
    cwist_app_use(app, async_gate);
}

static void *route_ctx(cwist_handler_func handler, bool cacheable) {
    fly_async_route *route = malloc(sizeof(*route));
    if (!route) return NULL;
    route->handler = handler;
    route->cacheable = cacheable;
    return route;
}

static void register_get(cwist_app *app, const char *path, cwist_handler_func handler,
                         bool cacheable) {
    void *ctx = route_ctx(handler, cacheable);
    if (!ctx) {
        cwist_app_get(app, path, handler);
        return;
    }
    cwist_app_get_ex(app, path, async_trampoline, ctx, free);
}

void engine_async_get(cwist_app *app, const char *path, cwist_handler_func handler) {
    register_get(app, path, handler, false);
}

void engine_async_get_cached(cwist_app *app, const char *path, cwist_handler_func handler) {
    register_get(app, path, handler, true);
}

void engine_async_post(cwist_app *app, const char *path, cwist_handler_func handler) {
    void *ctx = route_ctx(handler, false);
    if (!ctx) {
        cwist_app_post(app, path, handler);
        return;
    }
    cwist_app_post_ex(app, path, async_trampoline, ctx, free);
}

void engine_async_set_hit_hook(engine_async_hit_func hook) {
    g_hit_hook = hook;
}
