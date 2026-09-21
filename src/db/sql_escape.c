#include "sql_escape.h"

#include <cwist/core/mem/alloc.h>

#define FB_STRUTIL_ALLOC cwist_alloc
#define FB_STRUTIL_FREE cwist_free
#include "../utils/strutil_pure.h"

char *sql_escape(const char *src) {
    return fb_sql_escape(src);
}

char *sql_unescape(const char *src) {
    return fb_sql_unescape(src);
}
