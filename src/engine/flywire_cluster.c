#define _POSIX_C_SOURCE 200809L
#include "engine/flywire.h"
#include "engine/flywire_cluster.h"
#include "engine/forkgate.h"
#include "engine/bdr.h"
#include "db/db.h"
#include "db/db_internal.h"
#include "config/config.h"
#include <cwist/core/log.h>
#include <cwist/core/mem/gc.h>
#include <cwist/net/http/http.h>
#include <cjson/cJSON.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* FlyWire failover cluster.
 *
 * flywire.settings lists the sites (nodes=, in priority order) and names this
 * one (self=). Every node then runs this thread in its supervisor process:
 * each tick it reads every peer's GET /flywire/status and decides
 *
 *  - adopt:   a peer reports a higher epoch -> follow its primary. The epoch is
 *             bumped on every role change, so the newest decision wins.
 *  - failover: the primary has been silent for failover_seconds, and a
 *             majority of the sites (this one plus peers reporting the same
 *             outage and naming the same candidate) agree -> the candidate
 *             promotes itself with epoch+1. Candidate: the first live node in
 *             nodes order (failover=priority) or the most up-to-date live
 *             node, then the fastest (failover=auto).
 *  - fence:   a primary that cannot reach a majority for failover_seconds
 *             stops taking writes (503), so a partitioned primary and a newly
 *             elected one never both accept writes. A primary also starts
 *             fenced until its first tick confirms a majority and no newer
 *             epoch, so a restarted old primary cannot take writes it will
 *             immediately lose.
 *  - fail back (priority mode): when a higher-priority node follows this
 *             primary and has applied its whole journal, this primary fences
 *             itself and hands over; the other node promotes with epoch+1.
 *
 * One replicated log: a replica mirrors every applied journal row into its own
 * sync_journal under the primary's seq (and originates none itself), so all
 * nodes share one seq space. A new primary keeps appending after its last
 * seq, and followers keep their checkpoint and fetch whatever they missed
 * from it. A node holding rows past the new primary's base seq (the old
 * primary's last writes that never replicated) drops them from its journal,
 * logs how many were lost, and resumes from the base.
 *
 * Role state lives in a MAP_SHARED page created before the worker fork, so
 * every worker serving requests sees role changes at once. */

typedef struct {
    _Atomic long long epoch;
    _Atomic long long base_seq;   /* journal seq at which the primary took over */
    _Atomic int primary_idx;
    _Atomic int fenced;
    _Atomic int primary_down;     /* replica: primary silent >= failover_seconds */
    _Atomic int candidate_idx;    /* replica: who should take over, -1 none */
    _Atomic int handover_to;      /* primary: node being handed the role, -1 none */
    _Atomic int mean_rtt_ms;      /* to reachable peers, for failover=auto */
} flywire_shared_t;

static flywire_shared_t *g_fs = NULL;
static _Atomic bool g_cluster_running = false;
static pthread_t g_cluster_thread;
static bool g_cluster_thread_started = false;
/* Workers inherit g_cluster_thread_started across fork but not the thread. */
static pid_t g_cluster_owner = 0;

#define FLYWIRE_CLUSTER_STATE "data/.flywire_cluster"
#define FLYWIRE_CLUSTER_STATE_TMP "data/.flywire_cluster.tmp"

typedef struct {
    bool ok;
    int rtt_ms;
    long long epoch;
    long long base_seq;
    long long journal_max;
    int primary_idx;
    int primary_down;
    int candidate_idx;
    int handover_to;
    int mean_rtt_ms;
} peer_view_t;

bool flywire_cluster_active(void) {
    return g_fs != NULL && flywire_cluster_configured();
}

bool flywire_cluster_ready(void) {
    return g_fs != NULL;
}

int flywire_cluster_current_primary(void) {
    if (!g_fs) return flywire_cluster_configured() ? 0 : -1;
    return atomic_load(&g_fs->primary_idx);
}

long long flywire_cluster_epoch(void) {
    return g_fs ? atomic_load(&g_fs->epoch) : 0;
}

bool flywire_write_fenced(void) {
    if (!flywire_cluster_active()) return false;
    return atomic_load(&g_fs->primary_idx) == g_flywire.self_idx && atomic_load(&g_fs->fenced);
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- Addressing ---- */

struct curl_slist *flywire_curl_target(CURL *curl, int node_idx, const char *path_and_query) {
    if (!curl || node_idx < 0 || node_idx >= g_flywire.n_nodes) return NULL;
    const char *node = g_flywire.nodes[node_idx];
    const char *path = path_and_query ? path_and_query : "/";
    char url[1024];
    if (strstr(node, "://")) {
        snprintf(url, sizeof(url), "%s%s", node, path);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        return NULL;
    }
    const char *host = g_flywire.site_host[0] ? g_flywire.site_host : node;
    snprintf(url, sizeof(url), "https://%s%s", host, path);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (!g_flywire.site_host[0]) return NULL;
    char map[600];
    snprintf(map, sizeof(map), "%s:443:%s:443", g_flywire.site_host, node);
    struct curl_slist *to = curl_slist_append(NULL, map);
    curl_easy_setopt(curl, CURLOPT_CONNECT_TO, to);
    return to;
}

/* ---- Persisted role state ---- */

static void state_save(void) {
    FILE *f = fopen(FLYWIRE_CLUSTER_STATE_TMP, "w");
    if (!f) return;
    fprintf(f, "%lld %d %lld\n", (long long)atomic_load(&g_fs->epoch), atomic_load(&g_fs->primary_idx),
            (long long)atomic_load(&g_fs->base_seq));
    if (fclose(f) == 0) rename(FLYWIRE_CLUSTER_STATE_TMP, FLYWIRE_CLUSTER_STATE);
}

static void state_load(void) {
    long long epoch = 0, base = 0;
    int primary = 0;
    FILE *f = fopen(FLYWIRE_CLUSTER_STATE, "r");
    if (f) {
        if (fscanf(f, "%lld %d %lld", &epoch, &primary, &base) != 3) {
            epoch = 0;
            primary = 0;
            base = 0;
        }
        fclose(f);
    }
    if (primary < 0 || primary >= g_flywire.n_nodes) primary = 0;
    atomic_store(&g_fs->epoch, epoch);
    atomic_store(&g_fs->primary_idx, primary);
    atomic_store(&g_fs->base_seq, base);
}

/* ---- Local journal ---- */

static long long journal_max(sqlite3 *conn) {
    long long v = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT COALESCE(MAX(seq),0) FROM sync_journal", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

/* How far this node's copy of the log reaches. A replica's checkpoint can be
 * ahead of its mirrored journal when it ran before mirroring existed. */
static long long applied_seq(sqlite3 *conn, bool is_primary) {
    long long jm = journal_max(conn);
    if (is_primary) return jm;
    long long cp = flywire_checkpoint_read();
    return cp > jm ? cp : jm;
}

/* New local journal rows must continue after @p base, even when this node's
 * own AUTOINCREMENT counter is behind the shared seq space. */
static void journal_continue_after(sqlite3 *conn, long long base) {
    char sql[160];
    snprintf(sql, sizeof(sql),
             "UPDATE sqlite_sequence SET seq=MAX(seq,%lld) WHERE name='sync_journal'", base);
    sqlite3_exec(conn, sql, NULL, NULL, NULL);
    if (sqlite3_changes(conn) == 0) {
        snprintf(sql, sizeof(sql), "INSERT INTO sqlite_sequence(name, seq) VALUES('sync_journal', %lld)", base);
        sqlite3_exec(conn, sql, NULL, NULL, NULL);
    }
}

bool flywire_cluster_mirror_row(sqlite3 *conn, long long seq, const char *entity, int entity_id,
                                const char *op, const char *payload) {
    if (!flywire_cluster_active() || !conn) return true;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn,
                           "INSERT OR REPLACE INTO sync_journal (seq, entity, entity_id, op, payload) "
                           "VALUES (?,?,?,?,?)", -1, &st, NULL) != SQLITE_OK) {
        CWIST_LOG_ERROR("flywire: mirror prepare failed: %s", sqlite3_errmsg(conn));
        return false;
    }
    sqlite3_bind_int64(st, 1, seq);
    sqlite3_bind_text(st, 2, entity ? entity : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, entity_id);
    sqlite3_bind_text(st, 4, op ? op : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 5, payload ? payload : "", -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        CWIST_LOG_ERROR("flywire: mirror of seq %lld failed: %s", seq, sqlite3_errmsg(conn));
        return false;
    }
    return true;
}

/* Rows past @p base were written by a primary that lost its role before they
 * reached the new one: drop them so this node's log matches the new primary,
 * and resume applying from @p base. */
static void journal_truncate_after(sqlite3 *conn, long long base) {
    long long had = journal_max(conn);
    if (had <= base) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, "DELETE FROM sync_journal WHERE seq > ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, base);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    CWIST_LOG_WARN("flywire: journal rows %lld..%lld never reached the new primary and were dropped; "
                   "their changes stay only in this node's tables", base + 1, had);
}

/* ---- Status endpoint (served by workers) ---- */

void handler_flywire_status(cwist_http_request *req, cwist_http_response *res) {
    const char *token = cwist_http_header_get(req->headers, "X-FlyWire-Token");
    if (!flywire_cluster_active() || !token ||
        strlen(token) != strlen(flywire_token()) ||
        CRYPTO_memcmp(token, flywire_token(), strlen(token)) != 0) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "self", g_flywire.self_idx);
    cJSON_AddNumberToObject(o, "epoch", (double)atomic_load(&g_fs->epoch));
    cJSON_AddNumberToObject(o, "base_seq", (double)atomic_load(&g_fs->base_seq));
    cJSON_AddNumberToObject(o, "primary", atomic_load(&g_fs->primary_idx));
    bool primary = atomic_load(&g_fs->primary_idx) == g_flywire.self_idx;
    cJSON_AddNumberToObject(o, "journal_max", (double)applied_seq(fly_db_conn(req->db), primary));
    cJSON_AddNumberToObject(o, "primary_down", atomic_load(&g_fs->primary_down));
    cJSON_AddNumberToObject(o, "candidate", atomic_load(&g_fs->candidate_idx));
    cJSON_AddNumberToObject(o, "handover_to", atomic_load(&g_fs->handover_to));
    cJSON_AddNumberToObject(o, "fenced", atomic_load(&g_fs->fenced));
    cJSON_AddNumberToObject(o, "mean_rtt_ms", atomic_load(&g_fs->mean_rtt_ms));
    cJSON_AddStringToObject(o, "version", flywire_site_version());
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    cwist_sstring_assign(res->body, body ? body : "{}");
    free(body);
}

/* ---- Peer probing ---- */

typedef struct {
    char *data;
    size_t len;
} probe_buf;

static size_t probe_write(void *ptr, size_t size, size_t nmemb, void *ud) {
    probe_buf *b = ud;
    size_t n = size * nmemb;
    if (b->len + n > 16384) return 0;
    char *g = realloc(b->data, b->len + n + 1);
    if (!g) return 0;
    b->data = g;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static double num(cJSON *o, const char *k, double def) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void probe_peer(int idx, peer_view_t *out) {
    memset(out, 0, sizeof(*out));
    out->primary_idx = -1;
    out->candidate_idx = -1;
    out->handover_to = -1;
    CURL *curl = curl_easy_init();
    if (!curl) return;
    probe_buf buf = {0};
    char hdr[192];
    snprintf(hdr, sizeof(hdr), "X-FlyWire-Token: %s", flywire_token());
    struct curl_slist *headers = curl_slist_append(NULL, hdr);
    struct curl_slist *to = flywire_curl_target(curl, idx, "/flywire/status");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, probe_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    long long t0 = now_ms();
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_slist_free_all(to);
    curl_easy_cleanup(curl);
    if (rc == CURLE_OK && status == 200 && buf.data) {
        cJSON *o = cJSON_Parse(buf.data);
        if (o && (int)num(o, "self", -1) == idx) {
            out->ok = true;
            out->rtt_ms = (int)(now_ms() - t0);
            out->epoch = (long long)num(o, "epoch", 0);
            out->base_seq = (long long)num(o, "base_seq", 0);
            out->journal_max = (long long)num(o, "journal_max", 0);
            out->primary_idx = (int)num(o, "primary", -1);
            out->primary_down = (int)num(o, "primary_down", 0);
            out->candidate_idx = (int)num(o, "candidate", -1);
            out->handover_to = (int)num(o, "handover_to", -1);
            out->mean_rtt_ms = (int)num(o, "mean_rtt_ms", 0);
        }
        if (o) cJSON_Delete(o);
    }
    free(buf.data);
}

typedef struct {
    int idx;
    peer_view_t *out;
} probe_job_t;

static void *probe_thread(void *arg) {
    probe_job_t *j = arg;
    probe_peer(j->idx, j->out);
    cwist_gc_scope_flush();
    return NULL;
}

/* Probe every peer at once so a silent node costs one timeout per tick, not
 * one per peer (fencing and failover timing depend on the tick length). */
static void probe_all(peer_view_t *peers) {
    int n = g_flywire.n_nodes, self = g_flywire.self_idx;
    pthread_t th[FLYWIRE_MAX_NODES];
    probe_job_t jobs[FLYWIRE_MAX_NODES];
    bool started[FLYWIRE_MAX_NODES] = {false};
    for (int i = 0; i < n; i++) {
        memset(&peers[i], 0, sizeof(peers[i]));
        peers[i].primary_idx = peers[i].candidate_idx = peers[i].handover_to = -1;
        if (i == self) continue;
        jobs[i].idx = i;
        jobs[i].out = &peers[i];
        started[i] = pthread_create(&th[i], NULL, probe_thread, &jobs[i]) == 0;
        if (!started[i]) probe_peer(i, &peers[i]);
    }
    for (int i = 0; i < n; i++) {
        if (started[i]) pthread_join(th[i], NULL);
    }
}

/* ---- Decisions ---- */

/* failover=auto ordering: more of the log first, then lower mean RTT, then
 * nodes order. Uses only values every node can see (each peer's own journal
 * max and mean RTT from its status), so all nodes pick the same candidate. */
static bool auto_better(long long jm_a, int rtt_a, int idx_a, long long jm_b, int rtt_b, int idx_b) {
    if (jm_a != jm_b) return jm_a > jm_b;
    if (rtt_a != rtt_b) return rtt_a < rtt_b;
    return idx_a < idx_b;
}

static int pick_candidate(const peer_view_t *peers, int primary, long long my_jm, int my_rtt) {
    int self = g_flywire.self_idx;
    int best = -1;
    long long best_jm = 0;
    int best_rtt = 0;
    for (int i = 0; i < g_flywire.n_nodes; i++) {
        if (i == primary) continue;
        bool alive = (i == self) || peers[i].ok;
        if (!alive) continue;
        long long jm = (i == self) ? my_jm : peers[i].journal_max;
        int rtt = (i == self) ? my_rtt : peers[i].mean_rtt_ms;
        if (best < 0) {
            best = i; best_jm = jm; best_rtt = rtt;
            if (!g_flywire.failover_auto) break; /* priority: first live node */
            continue;
        }
        if (auto_better(jm, rtt, i, best_jm, best_rtt, best)) {
            best = i; best_jm = jm; best_rtt = rtt;
        }
    }
    return best;
}

/* Become the primary for a new epoch whose log starts after @p base. */
static void promote(sqlite3 *conn, long long base, const char *why) {
    journal_continue_after(conn, base);
    long long epoch = atomic_load(&g_fs->epoch) + 1;
    atomic_store(&g_fs->base_seq, base);
    atomic_store(&g_fs->epoch, epoch);
    atomic_store(&g_fs->primary_idx, g_flywire.self_idx);
    atomic_store(&g_fs->handover_to, -1);
    atomic_store(&g_fs->primary_down, 0);
    atomic_store(&g_fs->candidate_idx, -1);
    atomic_store(&g_fs->fenced, 0);
    state_save();
    engine_bdr_bump();
    CWIST_LOG_WARN("flywire: %s - this node (%s) is now primary, epoch %lld, journal from seq %lld",
                   why, g_flywire.nodes[g_flywire.self_idx], epoch, base);
}

typedef struct {
    long long primary_silent_since;  /* replica: first failed primary probe */
    long long majority_lost_since;   /* primary: first tick without a majority */
    long long handover_since;
    bool confirmed;                  /* primary: a tick has confirmed the role */
} cluster_local_t;

static void cluster_tick(cluster_local_t *L) {
    int n = g_flywire.n_nodes;
    int self = g_flywire.self_idx;
    peer_view_t peers[FLYWIRE_MAX_NODES];
    int rtt_sum = 0, rtt_n = 0, reachable = 0;
    long long probe_started = now_ms();
    probe_all(peers);
    for (int i = 0; i < n; i++) {
        if (i == self) continue;
        if (peers[i].ok) {
            reachable++;
            rtt_sum += peers[i].rtt_ms;
            rtt_n++;
        }
    }
    atomic_store(&g_fs->mean_rtt_ms, rtt_n ? rtt_sum / rtt_n : 0);

    fly_forkgate_enter();
    sqlite3 *conn = NULL;
    if (sqlite3_open(FLY_DB_MAIN_PATH, &conn) != SQLITE_OK || !db_configure_connection(conn)) {
        CWIST_LOG_ERROR("flywire: cluster tick cannot open %s", FLY_DB_MAIN_PATH);
        if (conn) sqlite3_close(conn);
        fly_forkgate_leave();
        return;
    }
    long long now = probe_started;
    long long my_epoch = atomic_load(&g_fs->epoch);
    int primary = atomic_load(&g_fs->primary_idx);
    long long my_jm = applied_seq(conn, primary == self);
    long long fo_ms = (long long)g_flywire.failover_seconds * 1000;

    /* 1. Adopt the newest epoch any peer knows. */
    int newest = -1;
    for (int i = 0; i < n; i++) {
        if (i == self || !peers[i].ok) continue;
        if (peers[i].epoch > my_epoch && (newest < 0 || peers[i].epoch > peers[newest].epoch)) newest = i;
    }
    if (newest >= 0) {
        int was_primary = (primary == self);
        long long base = peers[newest].base_seq;
        atomic_store(&g_fs->epoch, peers[newest].epoch);
        atomic_store(&g_fs->primary_idx, peers[newest].primary_idx);
        atomic_store(&g_fs->base_seq, base);
        atomic_store(&g_fs->handover_to, -1);
        atomic_store(&g_fs->fenced, 0);
        if (peers[newest].primary_idx != self) {
            if (my_jm > base) {
                journal_truncate_after(conn, base);
                flywire_checkpoint_write(base);
                my_jm = base;
            } else if (flywire_checkpoint_read() != my_jm) {
                /* A former primary has no checkpoint of its own: its log is
                 * everything it has applied. */
                flywire_checkpoint_write(my_jm);
            }
        }
        state_save();
        engine_bdr_bump();
        CWIST_LOG_WARN("flywire: adopted epoch %lld from %s: primary is %s%s", peers[newest].epoch,
                       g_flywire.nodes[newest], g_flywire.nodes[peers[newest].primary_idx],
                       was_primary && peers[newest].primary_idx != self ? " (this node stepped down)" : "");
        L->primary_silent_since = 0;
        L->majority_lost_since = 0;
        L->confirmed = false;
        sqlite3_close(conn);
        fly_forkgate_leave();
        return;
    }

    if (primary == self) {
        /* 2. Primary: keep a majority or fence; hand back to a higher-priority
         *    node once it has the whole log. */
        bool majority = (1 + reachable) * 2 > n;
        int handover = atomic_load(&g_fs->handover_to);
        if (majority) {
            L->majority_lost_since = 0;
            if (!L->confirmed) {
                L->confirmed = true;
                CWIST_LOG_INFO("flywire: primary role confirmed by a majority (epoch %lld)", my_epoch);
            }
        } else {
            if (!L->majority_lost_since) L->majority_lost_since = now;
            if (now - L->majority_lost_since >= fo_ms && L->confirmed) {
                L->confirmed = false;
                CWIST_LOG_WARN("flywire: primary cannot reach a majority of %d sites; writes fenced", n);
            }
        }
        if (handover >= 0) {
            bool target_ok = peers[handover].ok && peers[handover].epoch == my_epoch;
            if (!target_ok || now - L->handover_since > 3 * fo_ms) {
                CWIST_LOG_WARN("flywire: handover to %s abandoned", g_flywire.nodes[handover]);
                atomic_store(&g_fs->handover_to, -1);
                handover = -1;
            }
        } else if (!g_flywire.failover_auto && L->confirmed) {
            for (int i = 0; i < self; i++) {
                if (peers[i].ok && peers[i].epoch == my_epoch && peers[i].primary_idx == self &&
                    peers[i].journal_max == my_jm) {
                    atomic_store(&g_fs->handover_to, i);
                    L->handover_since = now;
                    handover = i;
                    CWIST_LOG_WARN("flywire: %s has caught up and ranks higher; fencing writes and handing "
                                   "the primary role back", g_flywire.nodes[i]);
                    break;
                }
            }
        }
        atomic_store(&g_fs->fenced, (!L->confirmed || handover >= 0) ? 1 : 0);
        atomic_store(&g_fs->primary_down, 0);
        atomic_store(&g_fs->candidate_idx, -1);
    } else {
        /* 3. Replica: watch the primary, take over by majority, or accept a
         *    handover. */
        bool primary_ok = primary >= 0 && primary < n && peers[primary].ok;
        if (primary_ok) L->primary_silent_since = 0;
        else if (!L->primary_silent_since) L->primary_silent_since = now;
        bool down = !primary_ok && now - L->primary_silent_since >= fo_ms;
        int cand = down ? pick_candidate(peers, primary, my_jm, atomic_load(&g_fs->mean_rtt_ms)) : -1;
        atomic_store(&g_fs->primary_down, down ? 1 : 0);
        atomic_store(&g_fs->candidate_idx, cand);

        if (primary_ok && peers[primary].handover_to == self && peers[primary].epoch == my_epoch &&
            peers[primary].journal_max == my_jm) {
            promote(conn, my_jm, "the primary handed the role back");
        } else if (down && cand == self) {
            int agree = 1;
            for (int i = 0; i < n; i++) {
                if (i == self || i == primary || !peers[i].ok) continue;
                if (peers[i].epoch == my_epoch && peers[i].primary_down && peers[i].candidate_idx == self) agree++;
            }
            if (agree * 2 > n) {
                char why[160];
                snprintf(why, sizeof(why), "primary %s silent for %ds, %d of %d sites agree",
                         g_flywire.nodes[primary], g_flywire.failover_seconds, agree, n);
                promote(conn, my_jm, why);
            }
        }
    }
    sqlite3_close(conn);
    fly_forkgate_leave();
}

static void *cluster_main(void *arg) {
    (void)arg;
    cluster_local_t L = {0};
    long long next_poll = 0;
    while (atomic_load(&g_cluster_running)) {
        cluster_tick(&L);
        /* Replicas apply the feed from whichever node is primary now. A
         * journal purged past our checkpoint needs a reseed: keep making role
         * decisions but stop polling for 5 minutes at a time. */
        if (atomic_load(&g_fs->primary_idx) != g_flywire.self_idx && now_ms() >= next_poll) {
            if (flywire_replica_iteration() == 1) next_poll = now_ms() + 300000;
        }
        cwist_gc_scope_flush();
        for (int i = 0; i < flywire_poll_seconds() * 10 && atomic_load(&g_cluster_running); i++) usleep(100000);
    }
    return NULL;
}

bool flywire_cluster_start(void) {
    if (!flywire_cluster_configured()) return false;
    void *mem = mmap(NULL, sizeof(flywire_shared_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        CWIST_LOG_ERROR("flywire: cannot map the shared cluster state");
        return false;
    }
    g_fs = mem;
    memset(g_fs, 0, sizeof(*g_fs));
    atomic_store(&g_fs->handover_to, -1);
    atomic_store(&g_fs->candidate_idx, -1);
    state_load();
    /* Until a tick confirms a majority and no newer epoch, a node that
     * believes it is primary refuses writes. */
    atomic_store(&g_fs->fenced, atomic_load(&g_fs->primary_idx) == g_flywire.self_idx ? 1 : 0);
    atomic_store(&g_cluster_running, true);
    if (pthread_create(&g_cluster_thread, NULL, cluster_main, NULL) != 0) {
        atomic_store(&g_cluster_running, false);
        CWIST_LOG_ERROR("flywire: failed to start the cluster thread");
        return false;
    }
    g_cluster_thread_started = true;
    g_cluster_owner = getpid();
    CWIST_LOG_INFO("flywire: failover cluster of %d sites, this is %s; epoch %lld, primary %s (%s)",
                   g_flywire.n_nodes, g_flywire.nodes[g_flywire.self_idx], (long long)atomic_load(&g_fs->epoch),
                   g_flywire.nodes[atomic_load(&g_fs->primary_idx)],
                   g_flywire.failover_auto ? "failover=auto" : "failover=priority");
    return true;
}

void flywire_cluster_stop(void) {
    if (!g_cluster_thread_started || g_cluster_owner != getpid()) return;
    atomic_store(&g_cluster_running, false);
    /* Detached-equivalent: the supervisor is exiting; the thread wakes within
     * 100 ms and returns. Joining here could wait on a 5 s peer probe. */
    pthread_detach(g_cluster_thread);
    g_cluster_thread_started = false;
}
