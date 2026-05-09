#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool exec_sql(cwist_db *db, const char *sql) {
    cwist_error_t err = cwist_db_exec(db, sql);
    return err.errtype == CWIST_ERR_INT16 && err.error.err_i16 == 0;
}

bool db_init(cwist_db *db) {
    if (!db || !db->conn) return false;
    return db_migrate(db);
}

bool db_migrate(cwist_db *db) {
    const char *schema =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT UNIQUE NOT NULL,"
        "  email TEXT UNIQUE NOT NULL,"
        "  password_hash TEXT NOT NULL,"
        "  role TEXT DEFAULT 'user',"
        "  profile_pic TEXT,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  active INTEGER DEFAULT 1"
        ");"
        "CREATE TABLE IF NOT EXISTS boards ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT UNIQUE NOT NULL,"
        "  slug TEXT UNIQUE NOT NULL,"
        "  description TEXT,"
        "  admin_only INTEGER DEFAULT 0,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS board_permissions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  board_id INTEGER NOT NULL,"
        "  user_id INTEGER NOT NULL,"
        "  UNIQUE(board_id, user_id),"
        "  FOREIGN KEY(board_id) REFERENCES boards(id) ON DELETE CASCADE,"
        "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS posts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  board_id INTEGER,"
        "  user_id INTEGER NOT NULL,"
        "  title TEXT NOT NULL,"
        "  slug TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  summary TEXT,"
        "  pqc_signature TEXT,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY(board_id) REFERENCES boards(id) ON DELETE SET NULL,"
        "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS files ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  post_id INTEGER,"
        "  user_id INTEGER NOT NULL,"
        "  filename TEXT NOT NULL,"
        "  mime_type TEXT,"
        "  file_path TEXT NOT NULL,"
        "  size INTEGER DEFAULT 0,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY(post_id) REFERENCES posts(id) ON DELETE SET NULL,"
        "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS comments ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  target_type TEXT NOT NULL,"
        "  target_id INTEGER NOT NULL,"
        "  user_id INTEGER NOT NULL,"
        "  parent_id INTEGER DEFAULT NULL,"
        "  content TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  deleted INTEGER DEFAULT 0,"
        "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
        "  FOREIGN KEY(parent_id) REFERENCES comments(id) ON DELETE CASCADE"
        ");";
    if (!exec_sql(db, schema)) return false;
    exec_sql(db, "ALTER TABLE users ADD COLUMN nickname TEXT");
    exec_sql(db, "ALTER TABLE users ADD COLUMN bio TEXT");
    exec_sql(db, "ALTER TABLE users ADD COLUMN profile_pic TEXT");
    exec_sql(db, "ALTER TABLE posts ADD COLUMN view_count INTEGER DEFAULT 0");
    exec_sql(db, "ALTER TABLE posts ADD COLUMN is_notice INTEGER DEFAULT 0");
    exec_sql(db, "ALTER TABLE posts ADD COLUMN is_secret INTEGER DEFAULT 0");
    exec_sql(db, "ALTER TABLE posts ADD COLUMN category TEXT DEFAULT ''");
    exec_sql(db, "ALTER TABLE boards ADD COLUMN read_perm INTEGER DEFAULT 0");
    exec_sql(db, "ALTER TABLE boards ADD COLUMN write_perm INTEGER DEFAULT 0");
    exec_sql(db, "ALTER TABLE boards ADD COLUMN comment_perm INTEGER DEFAULT 0");
    exec_sql(db, "ALTER TABLE files ADD COLUMN download_count INTEGER DEFAULT 0");
    exec_sql(db, "CREATE TABLE IF NOT EXISTS post_votes (id INTEGER PRIMARY KEY AUTOINCREMENT, post_id INTEGER NOT NULL, user_id INTEGER NOT NULL, vote_type INTEGER DEFAULT 0, created_at DATETIME DEFAULT CURRENT_TIMESTAMP, UNIQUE(post_id, user_id), FOREIGN KEY(post_id) REFERENCES posts(id) ON DELETE CASCADE, FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE)");
    exec_sql(db, "CREATE TABLE IF NOT EXISTS tags (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE NOT NULL)");
    exec_sql(db, "CREATE TABLE IF NOT EXISTS post_tags (id INTEGER PRIMARY KEY AUTOINCREMENT, post_id INTEGER NOT NULL, tag_id INTEGER NOT NULL, UNIQUE(post_id, tag_id), FOREIGN KEY(post_id) REFERENCES posts(id) ON DELETE CASCADE, FOREIGN KEY(tag_id) REFERENCES tags(id) ON DELETE CASCADE)");
    return true;
}

/* ---- Users ---- */
cJSON *db_user_get_by_username(cwist_db *db, const char *username) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE username='%s' LIMIT 1", username);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

cJSON *db_user_get_by_id(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE id=%d LIMIT 1", id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

bool db_user_create(cwist_db *db, const char *username, const char *email, const char *password_hash) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO users (username, email, password_hash) VALUES ('%s','%s','%s')",
        username, email, password_hash);
    return exec_sql(db, sql);
}

bool db_user_delete(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM users WHERE id=%d", id);
    return exec_sql(db, sql);
}

bool db_user_update_role(cwist_db *db, int id, const char *role) {
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE users SET role='%s' WHERE id=%d", role, id);
    return exec_sql(db, sql);
}

bool db_user_update_profile_pic(cwist_db *db, int id, const char *profile_pic) {
    char sql[512];
    snprintf(sql, sizeof(sql), "UPDATE users SET profile_pic='%s' WHERE id=%d", profile_pic, id);
    return exec_sql(db, sql);
}

bool db_user_update_profile(cwist_db *db, int id, const char *nickname, const char *bio, const char *profile_pic) {
    char sql[2048];
    snprintf(sql, sizeof(sql),
        "UPDATE users SET nickname='%s', bio='%s', profile_pic='%s' WHERE id=%d",
        nickname ? nickname : "", bio ? bio : "", profile_pic ? profile_pic : "", id);
    return exec_sql(db, sql);
}

bool db_user_update_password(cwist_db *db, int id, const char *password_hash) {
    char sql[1024];
    snprintf(sql, sizeof(sql), "UPDATE users SET password_hash='%s' WHERE id=%d", password_hash, id);
    return exec_sql(db, sql);
}

cJSON *db_user_list(cwist_db *db) {
    const char *sql = "SELECT id, username, email, role, profile_pic, created_at, active FROM users ORDER BY id";
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

/* ---- Boards ---- */
bool db_board_create(cwist_db *db, const char *name, const char *slug, const char *description, bool admin_only, int read_perm, int write_perm, int comment_perm) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO boards (name, slug, description, admin_only, read_perm, write_perm, comment_perm) VALUES ('%s','%s','%s',%d,%d,%d,%d)",
        name, slug, description ? description : "", admin_only ? 1 : 0, read_perm, write_perm, comment_perm);
    return exec_sql(db, sql);
}

bool db_board_delete(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM boards WHERE id=%d", id);
    return exec_sql(db, sql);
}

bool db_board_update(cwist_db *db, int id, const char *name, const char *slug, const char *description, bool admin_only, int read_perm, int write_perm, int comment_perm) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "UPDATE boards SET name='%s', slug='%s', description='%s', admin_only=%d, read_perm=%d, write_perm=%d, comment_perm=%d WHERE id=%d",
        name, slug, description ? description : "", admin_only ? 1 : 0, read_perm, write_perm, comment_perm, id);
    return exec_sql(db, sql);
}

cJSON *db_board_list(cwist_db *db) {
    const char *sql = "SELECT * FROM boards ORDER BY created_at DESC";
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

cJSON *db_board_get_by_slug(cwist_db *db, const char *slug) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT * FROM boards WHERE slug='%s' LIMIT 1", slug);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

cJSON *db_board_get_by_id(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM boards WHERE id=%d LIMIT 1", id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

bool db_board_can_user_access(cwist_db *db, int board_id, int user_id, bool is_admin) {
    cJSON *b = db_board_get_by_id(db, board_id);
    if (!b) return false;
    cJSON *ao = cJSON_GetObjectItem(b, "admin_only");
    int admin_only = ao ? ao->valueint : 0;
    cJSON_Delete(b);
    if (!admin_only) return true;
    if (is_admin) return true;
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT 1 FROM board_permissions WHERE board_id=%d AND user_id=%d LIMIT 1",
        board_id, user_id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    bool ok = false;
    if (res && cJSON_GetArraySize(res) > 0) ok = true;
    if (res) cJSON_Delete(res);
    return ok;
}

/* ---- Board permissions ---- */
bool db_board_perm_grant(cwist_db *db, int board_id, int user_id) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT OR IGNORE INTO board_permissions (board_id, user_id) VALUES (%d,%d)",
        board_id, user_id);
    return exec_sql(db, sql);
}

bool db_board_perm_revoke(cwist_db *db, int board_id, int user_id) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "DELETE FROM board_permissions WHERE board_id=%d AND user_id=%d",
        board_id, user_id);
    return exec_sql(db, sql);
}

cJSON *db_board_perm_list(cwist_db *db, int board_id) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT u.id, u.username, u.email FROM board_permissions bp JOIN users u ON bp.user_id=u.id WHERE bp.board_id=%d",
        board_id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

/* ---- Posts ---- */
bool db_post_create(cwist_db *db, int board_id, int user_id, const char *title, const char *slug, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category) {
    char sql[8192];
    snprintf(sql, sizeof(sql),
        "INSERT INTO posts (board_id, user_id, title, slug, content, summary, pqc_signature, is_notice, is_secret, category) VALUES (%d,%d,'%s','%s','%s','%s','%s',%d,%d,'%s')",
        board_id, user_id, title, slug, content, summary ? summary : "", pqc_signature ? pqc_signature : "", is_notice, is_secret, category ? category : "");
    return exec_sql(db, sql);
}

bool db_post_update(cwist_db *db, int id, const char *title, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category) {
    char sql[8192];
    snprintf(sql, sizeof(sql),
        "UPDATE posts SET title='%s', content='%s', summary='%s', pqc_signature='%s', is_notice=%d, is_secret=%d, category='%s', updated_at=CURRENT_TIMESTAMP WHERE id=%d",
        title, content, summary ? summary : "", pqc_signature ? pqc_signature : "", is_notice, is_secret, category ? category : "", id);
    return exec_sql(db, sql);
}

bool db_post_delete(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM posts WHERE id=%d", id);
    return exec_sql(db, sql);
}

cJSON *db_post_get_by_slug(cwist_db *db, const char *slug) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.slug='%s' LIMIT 1",
        slug);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

cJSON *db_post_get_by_id(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.id=%d LIMIT 1",
        id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

cJSON *db_post_list(cwist_db *db, int board_id, int limit, int offset) {
    char sql[1024];
    if (board_id > 0) {
        snprintf(sql, sizeof(sql),
            "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.board_id=%d ORDER BY p.created_at DESC LIMIT %d OFFSET %d",
            board_id, limit, offset);
    } else {
        snprintf(sql, sizeof(sql),
            "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id ORDER BY p.created_at DESC LIMIT %d OFFSET %d",
            limit, offset);
    }
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

cJSON *db_post_recent(cwist_db *db, int limit) {
    return db_post_list(db, 0, limit, 0);
}

cJSON *db_post_recent_by_board(cwist_db *db, int board_id, int limit) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.board_id=%d ORDER BY p.created_at DESC LIMIT %d",
        board_id, limit);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

int db_post_count(cwist_db *db, int board_id) {
    char sql[512];
    if (board_id > 0) {
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts WHERE board_id=%d", board_id);
    } else {
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts");
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

/* ---- Files ---- */
bool db_file_create_volume(cwist_db *db, int post_id, int user_id, const char *filename, const char *mime_type, const char *file_path, size_t len) {
    char sql[2048];
    snprintf(sql, sizeof(sql),
        "INSERT INTO files (post_id, user_id, filename, mime_type, file_path, size) VALUES (%d,%d,'%s','%s','%s',%lu)",
        post_id, user_id, filename, mime_type ? mime_type : "application/octet-stream", file_path, (unsigned long)len);
    return exec_sql(db, sql);
}

cJSON *db_file_get(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM files WHERE id=%d LIMIT 1", id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

cJSON *db_file_list_by_post(cwist_db *db, int post_id) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT id, filename, mime_type, file_path, size, created_at FROM files WHERE post_id=%d", post_id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

bool db_file_delete(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM files WHERE id=%d", id);
    return exec_sql(db, sql);
}

/* ---- Comments ---- */
bool db_comment_create(cwist_db *db, const char *target_type, int target_id, int user_id, int parent_id, const char *content) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO comments (target_type, target_id, user_id, parent_id, content) VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, target_type, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, target_id);
    sqlite3_bind_int(stmt, 3, user_id);
    sqlite3_bind_int(stmt, 4, parent_id);
    sqlite3_bind_text(stmt, 5, content, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_comment_update(cwist_db *db, int id, int user_id, const char *content) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE comments SET content=?, updated_at=CURRENT_TIMESTAMP WHERE id=? AND user_id=? AND deleted=0";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, content, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, id);
    sqlite3_bind_int(stmt, 3, user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_comment_delete(cwist_db *db, int id, int user_id) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE comments SET deleted=1, content='', updated_at=CURRENT_TIMESTAMP WHERE id=? AND user_id=? AND deleted=0";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_int(stmt, 2, user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_comment_get_by_id(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM comments WHERE id=%d LIMIT 1", id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

cJSON *db_comment_list_by_target(cwist_db *db, const char *target_type, int target_id) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT c.*, u.username FROM comments c "
        "JOIN users u ON c.user_id = u.id "
        "WHERE c.target_type='%s' AND c.target_id=%d "
        "ORDER BY c.created_at ASC",
        target_type, target_id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

/* ---- User delete with cascade ---- */
bool db_user_delete_with_cascade(cwist_db *db, int id, bool delete_replies) {
    if (delete_replies) {
        char sql[256];
        snprintf(sql, sizeof(sql), "DELETE FROM comments WHERE user_id=%d OR parent_id IN (SELECT id FROM comments WHERE user_id=%d)", id, id);
        exec_sql(db, sql);
    } else {
        char sql[256];
        snprintf(sql, sizeof(sql), "UPDATE comments SET content='', deleted=1 WHERE user_id=%d", id);
        exec_sql(db, sql);
    }
    char sql_posts[256];
    snprintf(sql_posts, sizeof(sql_posts), "DELETE FROM posts WHERE user_id=%d", id);
    exec_sql(db, sql_posts);
    char sql_files[256];
    snprintf(sql_files, sizeof(sql_files), "DELETE FROM files WHERE user_id=%d", id);
    exec_sql(db, sql_files);
    char sql_user[256];
    snprintf(sql_user, sizeof(sql_user), "DELETE FROM users WHERE id=%d", id);
    return exec_sql(db, sql_user);
}

/* ---- Post views ---- */
bool db_post_increment_view(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE posts SET view_count = view_count + 1 WHERE id=%d", id);
    return exec_sql(db, sql);
}

/* ---- Post list with search & notice ordering ---- */
cJSON *db_post_list_search(cwist_db *db, int board_id, const char *search, int limit, int offset) {
    char sql[2048];
    char esc[512] = {0};
    if (search && search[0]) {
        size_t j = 0;
        for (size_t i = 0; i < strlen(search) && j < sizeof(esc)-2; i++) {
            if (search[i] == '\'') esc[j++] = '\'';
            esc[j++] = search[i];
        }
        esc[j] = '\0';
    }
    if (board_id > 0) {
        if (search && search[0]) {
            snprintf(sql, sizeof(sql),
                "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.board_id=%d AND (p.title LIKE '%%%s%%' OR p.content LIKE '%%%s%%') ORDER BY p.is_notice DESC, p.created_at DESC LIMIT %d OFFSET %d",
                board_id, esc, esc, limit, offset);
        } else {
            snprintf(sql, sizeof(sql),
                "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.board_id=%d ORDER BY p.is_notice DESC, p.created_at DESC LIMIT %d OFFSET %d",
                board_id, limit, offset);
        }
    } else {
        if (search && search[0]) {
            snprintf(sql, sizeof(sql),
                "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.title LIKE '%%%s%%' OR p.content LIKE '%%%s%%' ORDER BY p.is_notice DESC, p.created_at DESC LIMIT %d OFFSET %d",
                esc, esc, limit, offset);
        } else {
            snprintf(sql, sizeof(sql),
                "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id ORDER BY p.is_notice DESC, p.created_at DESC LIMIT %d OFFSET %d",
                limit, offset);
        }
    }
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

int db_post_count_search(cwist_db *db, int board_id, const char *search) {
    char sql[1024];
    char esc[512] = {0};
    if (search && search[0]) {
        size_t j = 0;
        for (size_t i = 0; i < strlen(search) && j < sizeof(esc)-2; i++) {
            if (search[i] == '\'') esc[j++] = '\'';
            esc[j++] = search[i];
        }
        esc[j] = '\0';
    }
    if (board_id > 0) {
        if (search && search[0]) {
            snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts WHERE board_id=%d AND (title LIKE '%%%s%%' OR content LIKE '%%%s%%')", board_id, esc, esc);
        } else {
            snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts WHERE board_id=%d", board_id);
        }
    } else {
        if (search && search[0]) {
            snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts WHERE title LIKE '%%%s%%' OR content LIKE '%%%s%%'", esc, esc);
        } else {
            snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts");
        }
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

/* ---- Post votes ---- */
bool db_post_vote(cwist_db *db, int post_id, int user_id, int vote_type) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO post_votes (post_id, user_id, vote_type) VALUES (?,?,?) ON CONFLICT(post_id, user_id) DO UPDATE SET vote_type=excluded.vote_type";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_bind_int(stmt, 3, vote_type);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_post_vote_remove(cwist_db *db, int post_id, int user_id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM post_votes WHERE post_id=%d AND user_id=%d", post_id, user_id);
    return exec_sql(db, sql);
}

cJSON *db_post_vote_counts(cwist_db *db, int post_id) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT SUM(CASE WHEN vote_type=1 THEN 1 ELSE 0 END) as up, SUM(CASE WHEN vote_type=-1 THEN 1 ELSE 0 END) as down FROM post_votes WHERE post_id=%d", post_id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

int db_post_user_vote(cwist_db *db, int post_id, int user_id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT vote_type FROM post_votes WHERE post_id=%d AND user_id=%d LIMIT 1", post_id, user_id);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int vote = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        vote = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return vote;
}

/* ---- Tags ---- */
int db_tag_get_or_create(cwist_db *db, const char *name) {
    char sql[512];
    char esc[256] = {0};
    size_t j = 0;
    for (size_t i = 0; i < strlen(name) && j < sizeof(esc)-2; i++) {
        if (name[i] == '\'') esc[j++] = '\'';
        esc[j++] = name[i];
    }
    esc[j] = '\0';
    snprintf(sql, sizeof(sql), "SELECT id FROM tags WHERE name='%s' LIMIT 1", esc);
    sqlite3_stmt *stmt = NULL;
    int tag_id = 0;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            tag_id = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    if (tag_id > 0) return tag_id;
    snprintf(sql, sizeof(sql), "INSERT INTO tags (name) VALUES ('%s')", esc);
    if (exec_sql(db, sql)) {
        tag_id = (int)sqlite3_last_insert_rowid(db->conn);
    }
    return tag_id;
}

bool db_tag_link(cwist_db *db, int post_id, int tag_id) {
    char sql[512];
    snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO post_tags (post_id, tag_id) VALUES (%d,%d)", post_id, tag_id);
    return exec_sql(db, sql);
}

cJSON *db_tag_list_by_post(cwist_db *db, int post_id) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT t.name FROM tags t JOIN post_tags pt ON t.id=pt.tag_id WHERE pt.post_id=%d ORDER BY t.name", post_id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

bool db_tag_clear_by_post(cwist_db *db, int post_id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM post_tags WHERE post_id=%d", post_id);
    return exec_sql(db, sql);
}

/* ---- File downloads ---- */
bool db_file_increment_download(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE files SET download_count = download_count + 1 WHERE id=%d", id);
    return exec_sql(db, sql);
}
