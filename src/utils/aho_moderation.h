#ifndef FLYBOARD_AHO_MODERATION_H
#define FLYBOARD_AHO_MODERATION_H

#include <stdbool.h>
#include <stddef.h>

#define AHO_MODERATION_MAX_RULES 256
#define AHO_MODERATION_MAX_STATES 2048
#define AHO_MODERATION_RULES_MAX_BYTES 16384

typedef struct aho_moderation aho_moderation_t;

typedef struct {
    size_t count;
    bool limit_reached;
} aho_match_result_t;

aho_moderation_t *aho_moderation_build(const char *rules, bool case_fold);
void aho_moderation_destroy(aho_moderation_t *automaton);
aho_match_result_t aho_moderation_search(const aho_moderation_t *automaton,
                                         const char *text, size_t match_limit);

#endif
