#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool db_exec_sql(cwist_db *db, const char *sql) {
    cwist_error_t err = cwist_db_exec(db, sql);
    return err.errtype == CWIST_ERR_INT16 && err.error.err_i16 == 0;
}

cJSON *db_sqlite3_rows_to_json(sqlite3_stmt *stmt) {
    cJSON *arr = cJSON_CreateArray();
    int cols = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON *row = cJSON_CreateObject();
        for (int i = 0; i < cols; i++) {
            const char *name = sqlite3_column_name(stmt, i);
            int type = sqlite3_column_type(stmt, i);
            if (type == SQLITE_INTEGER) {
                cJSON_AddNumberToObject(row, name, sqlite3_column_int(stmt, i));
            } else if (type == SQLITE_FLOAT) {
                cJSON_AddNumberToObject(row, name, sqlite3_column_double(stmt, i));
            } else if (type == SQLITE_TEXT) {
                cJSON_AddStringToObject(row, name, (const char *)sqlite3_column_text(stmt, i));
            } else {
                cJSON_AddNullToObject(row, name);
            }
        }
        cJSON_AddItemToArray(arr, row);
    }
    sqlite3_finalize(stmt);
    return arr;
}

cJSON *db_sqlite3_row_to_json(sqlite3_stmt *stmt) {
    cJSON *arr = db_sqlite3_rows_to_json(stmt);
    if (arr && cJSON_GetArraySize(arr) > 0) {
        cJSON *row = cJSON_DetachItemFromArray(arr, 0);
        cJSON_Delete(arr);
        return row;
    }
    if (arr) cJSON_Delete(arr);
    return NULL;
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
    if (!db_exec_sql(db, schema)) return false;
    db_exec_sql(db, "ALTER TABLE users ADD COLUMN nickname TEXT");
    db_exec_sql(db, "ALTER TABLE users ADD COLUMN bio TEXT");
    db_exec_sql(db, "ALTER TABLE users ADD COLUMN profile_pic TEXT");
    db_exec_sql(db, "ALTER TABLE posts ADD COLUMN view_count INTEGER DEFAULT 0");
    db_exec_sql(db, "ALTER TABLE posts ADD COLUMN is_notice INTEGER DEFAULT 0");
    db_exec_sql(db, "ALTER TABLE posts ADD COLUMN is_secret INTEGER DEFAULT 0");
    db_exec_sql(db, "ALTER TABLE posts ADD COLUMN category TEXT DEFAULT ''");
    db_exec_sql(db, "ALTER TABLE posts ADD COLUMN delete_pin_hash TEXT DEFAULT ''");
    db_exec_sql(db, "ALTER TABLE boards ADD COLUMN read_perm INTEGER DEFAULT 0");
    db_exec_sql(db, "ALTER TABLE boards ADD COLUMN write_perm INTEGER DEFAULT 0");
    db_exec_sql(db, "ALTER TABLE boards ADD COLUMN comment_perm INTEGER DEFAULT 0");
    db_exec_sql(db, "ALTER TABLE files ADD COLUMN download_count INTEGER DEFAULT 0");
    db_exec_sql(db, "ALTER TABLE files ADD COLUMN delete_pin_hash TEXT DEFAULT ''");
    db_exec_sql(db, "ALTER TABLE files ADD COLUMN thumb_path TEXT DEFAULT ''");
    db_exec_sql(db, "ALTER TABLE files ADD COLUMN preview_path TEXT DEFAULT ''");
    db_exec_sql(db, "CREATE TABLE IF NOT EXISTS post_votes (id INTEGER PRIMARY KEY AUTOINCREMENT, post_id INTEGER NOT NULL, user_id INTEGER NOT NULL, vote_type INTEGER DEFAULT 0, created_at DATETIME DEFAULT CURRENT_TIMESTAMP, UNIQUE(post_id, user_id), FOREIGN KEY(post_id) REFERENCES posts(id) ON DELETE CASCADE, FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE)");
    db_exec_sql(db, "CREATE TABLE IF NOT EXISTS tags (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE NOT NULL)");
    db_exec_sql(db, "CREATE TABLE IF NOT EXISTS post_tags (id INTEGER PRIMARY KEY AUTOINCREMENT, post_id INTEGER NOT NULL, tag_id INTEGER NOT NULL, UNIQUE(post_id, tag_id), FOREIGN KEY(post_id) REFERENCES posts(id) ON DELETE CASCADE, FOREIGN KEY(tag_id) REFERENCES tags(id) ON DELETE CASCADE)");
    return true;
}
