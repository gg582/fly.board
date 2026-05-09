#ifndef HANDLERS_INTERNAL_H
#define HANDLERS_INTERNAL_H

#include "handlers.h"
#include "../auth/auth.h"
#include "../crypto/fly_crypto.h"
#include "../db/db.h"
#include "../nats/fly_nats.h"
#include "../render/render.h"
#include "../render/theme.h"
#include "../utils/utils.h"
#include "../config/config.h"
#include <cwist/core/sstring/sstring.h>
#include <unistd.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <cwist/net/http/query.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sqlite3.h>

bool is_dark(cwist_http_request *req);
void redirect(cwist_http_response *res, const char *url);
char *get_profile_pic(cwist_db *db, int uid, const char *role);
void send_html_res(cwist_http_response *res, cwist_sstring *html);
int json_int(cJSON *obj, const char *key, int def);
bool is_author_or_admin(cJSON *post, int uid, const char *role);
cJSON *board_by_route_key(cwist_db *db, const char *key);
char *sql_esc(const char *src);

#endif
