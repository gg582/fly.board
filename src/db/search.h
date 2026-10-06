#ifndef FLYBOARD_DB_SEARCH_H
#define FLYBOARD_DB_SEARCH_H

#include <stdbool.h>
#include <sqlite3.h>
#include <cwist/core/sstring/sstring.h>

#define SEARCH_MAX_TERMS 8
#define SEARCH_TERM_MAX_BYTES 64

/* Split a query on whitespace into at most SEARCH_MAX_TERMS terms. */
int search_split_terms(const char *query, char terms[SEARCH_MAX_TERMS][SEARCH_TERM_MAX_BYTES]);

/* SQL fragments for a post search over table aliases p (posts) and b
 * (boards). Every term must match (AND); terms of three or more code points
 * are narrowed through the trigram index first. */
typedef struct {
    cwist_sstring *where;      /* " AND ..." clauses, empty when no terms */
    cwist_sstring *title_rank; /* expression counting terms found in the title; empty when unranked */
    char **binds;
    int nbinds;
    int cap;
    int rank_first_bind;       /* binds[0..rank_first_bind) belong to where */
    int terms;
    char term[SEARCH_MAX_TERMS][SEARCH_TERM_MAX_BYTES];
} search_query;

void search_query_build(search_query *q, const char *query, const char *search_type);
void search_query_add_bind(search_query *q, const char *value);
/* Bind the WHERE values (and the rank values after them when @p with_rank)
 * starting at *idx, advancing it. */
void search_query_bind(const search_query *q, sqlite3_stmt *stmt, int *idx, bool with_rank);
void search_query_free(search_query *q);

#endif
