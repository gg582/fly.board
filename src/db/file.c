#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include "utils/utils.h"
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>

bool db_file_create_volume(cwist_db *db, int post_id, int user_id, const char *filename, const char *mime_type, const char *file_path, size_t len) {
    const char *sql = "INSERT INTO files (post_id, user_id, filename, mime_type, file_path, size) VALUES (?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_bind_text(stmt, 3, filename, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, mime_type ? mime_type : "application/octet-stream", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, file_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)len);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

int db_file_create_volume_get_id(cwist_db *db, int post_id, int user_id, const char *filename, const char *mime_type, const char *file_path, size_t len) {
    const char *sql = "INSERT INTO files (post_id, user_id, filename, mime_type, file_path, size) VALUES (?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_bind_text(stmt, 3, filename, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, mime_type ? mime_type : "application/octet-stream", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, file_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)len);
    int rc = sqlite3_step(stmt);
    int id = 0;
    if (rc == SQLITE_DONE) {
        id = (int)sqlite3_last_insert_rowid(db->conn);
    } else if (rc == SQLITE_CONSTRAINT) {
        id = -1;
    }
    sqlite3_finalize(stmt);
    return id;
}

static bool filename_exists_for_post(cwist_db *db, int post_id, const char *filename) {
    const char *sql = "SELECT 1 FROM files WHERE post_id=? AND filename=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_text(stmt, 2, filename, -1, SQLITE_STATIC);
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

static char *copy_string(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *out = (char *)cwist_alloc(len + 1);
    if (!out) return NULL;
    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

char *db_file_unique_filename(cwist_db *db, int post_id, const char *filename) {
    if (!filename || !filename[0]) return NULL;
    if (!filename_exists_for_post(db, post_id, filename)) {
        return copy_string(filename);
    }

    const char *dot = strrchr(filename, '.');
    size_t base_len = dot ? (size_t)(dot - filename) : strlen(filename);
    const char *ext = dot ? dot : "";

    for (int n = 1; n <= 10000; n++) {
        char candidate[512];
        snprintf(candidate, sizeof(candidate), "%.*s (%d)%s", (int)base_len, filename, n, ext);
        if (!filename_exists_for_post(db, post_id, candidate)) {
            return copy_string(candidate);
        }
    }
    return copy_string(filename);
}

cJSON *db_file_get(cwist_db *db, int id) {
    const char *sql = "SELECT * FROM files WHERE id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, id);
    return db_sqlite3_row_to_json(stmt);
}

cJSON *db_file_list_all(cwist_db *db) {
    const char *sql =
        "SELECT id, filename, mime_type, file_path, size, created_at, thumb_path, preview_path "
        "FROM files "
        "WHERE file_path IS NOT NULL AND file_path != '' AND file_path NOT LIKE '%/.thumbs/%' AND file_path NOT LIKE '%/.previews/%' "
        "ORDER BY id ASC";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    return db_sqlite3_rows_to_json(stmt);
}

cJSON *db_file_list_by_post(cwist_db *db, int post_id) {
    const char *sql =
        "SELECT a.id, a.filename, a.mime_type, a.file_path, a.size, a.created_at, a.thumb_path, a.preview_path "
        "FROM files a "
        "LEFT JOIN files b ON a.post_id = b.post_id AND a.filename = b.filename AND a.id < b.id "
        "WHERE a.post_id = ? AND b.id IS NULL AND (a.file_path IS NULL OR a.file_path NOT LIKE '%/.thumbs/%') "
        "ORDER BY a.id DESC";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, post_id);
    return db_sqlite3_rows_to_json(stmt);
}

cJSON *db_file_list_by_user(cwist_db *db, int user_id, int limit) {
    const char *sql =
        "SELECT id, filename, mime_type, file_path, size, created_at, thumb_path, preview_path "
        "FROM files "
        "WHERE user_id = ? AND (file_path IS NULL OR file_path NOT LIKE '%/.thumbs/%') "
        "ORDER BY id DESC "
        "LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, limit > 0 ? limit : 200);
    return db_sqlite3_rows_to_json(stmt);
}

bool db_file_delete(cwist_db *db, int id) {
    const char *sql = "DELETE FROM files WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

int db_file_drop_all(cwist_db *db) {
    const char *sql_select = "SELECT file_path FROM files";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql_select, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(stmt, 0);
            if (path && path[0] && is_safe_public_path(path)) unlink(path);
            else if (path && path[0]) CWIST_LOG_WARN("Refusing to drop unsafe file path: %s", path);
        }
        sqlite3_finalize(stmt);
    }
    const char *sql_count = "SELECT COUNT(*) FROM files";
    int count = 0;
    if (sqlite3_prepare_v2(db->conn, sql_count, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(db->conn, "DELETE FROM files", NULL, NULL, NULL);
    return count;
}

bool db_file_increment_download(cwist_db *db, int id) {
    const char *sql = "UPDATE files SET download_count = download_count + 1 WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_file_set_delete_pin_hash(cwist_db *db, int id, const char *delete_pin_hash) {
    const char *sql = "UPDATE files SET delete_pin_hash=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, delete_pin_hash ? delete_pin_hash : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_file_set_preview_paths(cwist_db *db, int id, const char *thumb_path, const char *preview_path) {
    const char *sql = "UPDATE files SET thumb_path=?, preview_path=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, thumb_path ? thumb_path : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, preview_path ? preview_path : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_file_attach_to_post(cwist_db *db, int id, int post_id, int is_inline) {
    const char *sql = "UPDATE files SET post_id=?, is_inline=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, is_inline ? 1 : 0);
    sqlite3_bind_int(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

void db_file_replace_for_post(cwist_db *db, int post_id, const char *filename) {
    const char *sql = "SELECT id, file_path FROM files WHERE post_id=? AND filename=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_text(stmt, 2, filename, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int existing_id = sqlite3_column_int(stmt, 0);
        const char *existing_path = (const char *)sqlite3_column_text(stmt, 1);
        if (existing_path && existing_path[0]) unlink(existing_path);
        sqlite3_finalize(stmt);
        db_file_delete(db, existing_id);
    } else {
        sqlite3_finalize(stmt);
    }
}

void db_file_cleanup_duplicates(cwist_db *db) {
    while (1) {
        const char *sql =
            "SELECT a.id, a.file_path FROM files a "
            "JOIN files b ON (a.post_id = b.post_id OR (a.post_id IS NULL AND b.post_id IS NULL)) "
            "AND a.filename = b.filename AND a.id < b.id "
            "LIMIT 1";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) break;
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            break;
        }
        int id = sqlite3_column_int(stmt, 0);
        const char *path = (const char *)sqlite3_column_text(stmt, 1);
        sqlite3_finalize(stmt);
        if (path && path[0]) unlink(path);
        db_file_delete(db, id);
    }
}

#include <dirent.h>

static bool path_in_db(cwist_db *db, const char *path) {
    if (!path || !path[0]) return false;
    const char *sql = "SELECT 1 FROM files WHERE file_path=? OR thumb_path=? OR preview_path=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, path, -1, SQLITE_STATIC);
    bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

static void cleanup_upload_dir(cwist_db *db, const char *dir_path, bool skip_hidden) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (skip_hidden && ent->d_name[0] == '.') continue;
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) continue;
        if (!path_in_db(db, full_path)) {
            CWIST_LOG_INFO("Removing orphaned file: %s", full_path);
            unlink(full_path);
        }
    }
    closedir(dir);
}

void db_cleanup_orphaned_files(cwist_db *db) {
    cleanup_upload_dir(db, "public/uploads", true);
    cleanup_upload_dir(db, "public/uploads/.thumbs", false);
    cleanup_upload_dir(db, "public/uploads/.previews", false);
}

void db_file_delete_by_post(cwist_db *db, int post_id) {
    const char *sql = "SELECT id, file_path, thumb_path, preview_path FROM files WHERE post_id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, post_id);

    int ids[256];
    char paths[256][PATH_MAX];
    char thumbs[256][PATH_MAX];
    char previews[256][PATH_MAX];
    int count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && count < 256) {
        ids[count] = sqlite3_column_int(stmt, 0);
        const char *p1 = (const char *)sqlite3_column_text(stmt, 1);
        const char *p2 = (const char *)sqlite3_column_text(stmt, 2);
        const char *p3 = (const char *)sqlite3_column_text(stmt, 3);
        paths[count][0] = '\0';
        thumbs[count][0] = '\0';
        previews[count][0] = '\0';
        if (p1) snprintf(paths[count], PATH_MAX, "%s", p1);
        if (p2) snprintf(thumbs[count], PATH_MAX, "%s", p2);
        if (p3) snprintf(previews[count], PATH_MAX, "%s", p3);
        count++;
    }
    sqlite3_finalize(stmt);

    for (int i = 0; i < count; i++) {
        if (paths[i][0] && is_safe_public_path(paths[i])) unlink(paths[i]);
        if (thumbs[i][0] && is_safe_public_path(thumbs[i])) unlink(thumbs[i]);
        if (previews[i][0]) {
            if ((paths[i][0] == '\0' || strcmp(previews[i], paths[i]) != 0) && is_safe_public_path(previews[i])) {
                unlink(previews[i]);
            }
        }

        DIR *dir = opendir("public/uploads/.thumbs");
        if (dir) {
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "%d_", ids[i]);
            size_t prefix_len = strlen(prefix);
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (strncmp(ent->d_name, prefix, prefix_len) == 0) {
                    char full_path[PATH_MAX];
                    snprintf(full_path, sizeof(full_path), "public/uploads/.thumbs/%s", ent->d_name);
                    unlink(full_path);
                }
            }
            closedir(dir);
        }

        db_file_delete(db, ids[i]);
    }
}
