#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

cJSON *cwist_orm_board_list_popular(cwist_db *db) {
    const char *sql =
        "SELECT b.*,"
        " COALESCE(AVG(p.view_count), 0) AS avg_views,"
        " COALESCE(MAX(p.view_count), 0) AS peak_views,"
        " (COALESCE(AVG(p.view_count), 0) * 0.3 + COALESCE(MAX(p.view_count), 0) * 0.7) AS score"
        " FROM boards b"
        " LEFT JOIN posts p ON p.board_id = b.id"
        " GROUP BY b.id"
        " ORDER BY score DESC, b.created_at DESC";
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}
