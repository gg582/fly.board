#define _POSIX_C_SOURCE 200809L
#include "engine/warmup.h"
#include "utils/cache.h"
#include "render/render.h"
#include "db/db.h"
#include "utils/media_preview.h"
#include <cwist/core/log.h>
#include <cwist/core/sstring/sstring.h>
#include <cjson/cJSON.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

static void *gif_warmup_thread_func(void *arg) {
    (void)arg;
    sqlite3 *conn = NULL;
    if (sqlite3_open("data/blog.db", &conn) != SQLITE_OK) {
        if (conn) sqlite3_close(conn);
        return NULL;
    }
    sqlite3_busy_timeout(conn, 5000);

    const char *sql_select = "SELECT id, file_path, preview_path FROM files WHERE mime_type = 'image/gif'";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql_select, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(conn);
        return NULL;
    }

    typedef struct {
        int id;
        char file_path[PATH_MAX];
        char preview_path[PATH_MAX];
    } gif_task_t;

    gif_task_t *tasks = NULL;
    int task_count = 0;
    int task_cap = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const char *fpath = (const char *)sqlite3_column_text(stmt, 1);
        const char *ppath = (const char *)sqlite3_column_text(stmt, 2);

        if (!fpath || fpath[0] == '\0') continue;

        bool need_convert = false;
        if (!ppath || ppath[0] == '\0') {
            need_convert = true;
        } else {
            struct stat st;
            if (stat(ppath, &st) != 0 || st.st_size <= 0) {
                need_convert = true;
            }
        }

        if (need_convert) {
            if (task_count >= task_cap) {
                task_cap = task_cap == 0 ? 16 : task_cap * 2;
                tasks = realloc(tasks, task_cap * sizeof(gif_task_t));
            }
            tasks[task_count].id = id;
            snprintf(tasks[task_count].file_path, sizeof(tasks[task_count].file_path), "%s", fpath);
            if (ppath) {
                snprintf(tasks[task_count].preview_path, sizeof(tasks[task_count].preview_path), "%s", ppath);
            } else {
                tasks[task_count].preview_path[0] = '\0';
            }
            task_count++;
        }
    }
    sqlite3_finalize(stmt);

    if (task_count > 0) {
        CWIST_LOG_INFO("GIF Warmup: Found %d GIFs needing MP4 conversion.", task_count);
    }

    for (int i = 0; i < task_count; i++) {
        int id = tasks[i].id;
        const char *orig_path = tasks[i].file_path;
        
        char webm_path[PATH_MAX];
        snprintf(webm_path, sizeof(webm_path), "public/uploads/.previews/%d.webm", id);

        struct stat st;
        if (stat(webm_path, &st) != 0 || st.st_size <= 0) {
            CWIST_LOG_INFO("GIF Warmup: Converting file %d (%s) to WebM...", id, orig_path);
            if (generate_webm_preview(orig_path, webm_path, 720)) {
                const char *sql_update = "UPDATE files SET preview_path = ? WHERE id = ?";
                sqlite3_stmt *up_stmt = NULL;
                if (sqlite3_prepare_v2(conn, sql_update, -1, &up_stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(up_stmt, 1, webm_path, -1, SQLITE_STATIC);
                    sqlite3_bind_int(up_stmt, 2, id);
                    if (sqlite3_step(up_stmt) == SQLITE_DONE) {
                        CWIST_LOG_INFO("GIF Warmup: Updated DB preview_path for file %d to %s", id, webm_path);
                    } else {
                        CWIST_LOG_ERROR("GIF Warmup: Failed to update DB preview_path for file %d", id);
                    }
                    sqlite3_finalize(up_stmt);
                }
            } else {
                CWIST_LOG_ERROR("GIF Warmup: Failed to convert file %d to WebM.", id);
            }
        }
    }

    free(tasks);
    sqlite3_close(conn);
    CWIST_LOG_INFO("GIF Warmup thread finished.");
    return NULL;
}

static void gif_warmup_start(void) {
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, gif_warmup_thread_func, NULL) == 0) {
        CWIST_LOG_INFO("Started GIF to MP4 warmup thread successfully.");
    } else {
        CWIST_LOG_ERROR("Failed to start GIF to MP4 warmup thread.");
    }
    pthread_attr_destroy(&attr);
}

void page_cache_warmup(cwist_db *db) {
    if (!db) return;
    CWIST_LOG_INFO("Warming up page cache...");

    struct { bool dark; bool mobile; } variants[] = {
        {false, false}, {true, false}, {false, true}, {true, true}
    };

    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        bool dark = variants[i].dark;
        bool mobile = variants[i].mobile;
        char key[256];
        page_cache_key_home(key, sizeof(key), dark, mobile, "", 0);
        if (page_cache_get(key, NULL, NULL, NULL)) {
            page_cache_release(key);
            continue;
        }

        cJSON *posts = db_post_recent(db, 12);
        cwist_sstring *page = render_post_list(posts, NULL, dark, "", 1, 1, "", NULL, NULL, NULL, 0, mobile, NULL);
        if (page) {
            page_cache_set(key, page->data, page->size, 300);
            cwist_sstring_destroy(page);
        }
        if (posts) cJSON_Delete(posts);
    }

    CWIST_LOG_INFO("Page cache warmup complete (%zu bytes)", page_cache_total_bytes());

    gif_warmup_start();
}
