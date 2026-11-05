#include "aho_moderation.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t codepoint;
    size_t state;
} aho_edge_t;

typedef struct {
    aho_edge_t *edges;
    size_t edge_count;
    size_t edge_capacity;
    size_t failure;
    size_t *outputs;
    size_t output_count;
    size_t output_capacity;
} aho_state_t;

struct aho_moderation {
    aho_state_t *states;
    size_t state_count;
    size_t state_capacity;
    bool case_fold;
};

static uint32_t fold_codepoint(uint32_t cp, bool enabled) {
    if (!enabled) return cp;
    if (cp >= 'A' && cp <= 'Z') return cp + 0x20;
    if ((cp >= 0x00c0 && cp <= 0x00d6) || (cp >= 0x00d8 && cp <= 0x00de)) return cp + 0x20;
    if ((cp >= 0x0391 && cp <= 0x03a1) || (cp >= 0x03a3 && cp <= 0x03ab)) return cp + 0x20;
    if (cp >= 0x0400 && cp <= 0x040f) return cp + 0x50;
    if (cp >= 0x0410 && cp <= 0x042f) return cp + 0x20;
    return cp;
}

static bool utf8_next(const unsigned char *s, size_t len, size_t *offset, uint32_t *cp) {
    size_t i = *offset;
    unsigned char a = s[i++];
    if (a < 0x80) {
        *cp = a;
    } else if (a >= 0xc2 && a <= 0xdf && i < len && (s[i] & 0xc0) == 0x80) {
        *cp = ((uint32_t)(a & 0x1f) << 6) | (s[i++] & 0x3f);
    } else if (a >= 0xe0 && a <= 0xef && i + 1 < len &&
               (s[i] & 0xc0) == 0x80 && (s[i + 1] & 0xc0) == 0x80 &&
               !(a == 0xe0 && s[i] < 0xa0) && !(a == 0xed && s[i] >= 0xa0)) {
        *cp = ((uint32_t)(a & 0x0f) << 12) | ((uint32_t)(s[i] & 0x3f) << 6) | (s[i + 1] & 0x3f);
        i += 2;
    } else if (a >= 0xf0 && a <= 0xf4 && i + 2 < len &&
               (s[i] & 0xc0) == 0x80 && (s[i + 1] & 0xc0) == 0x80 && (s[i + 2] & 0xc0) == 0x80 &&
               !(a == 0xf0 && s[i] < 0x90) && !(a == 0xf4 && s[i] >= 0x90)) {
        *cp = ((uint32_t)(a & 0x07) << 18) | ((uint32_t)(s[i] & 0x3f) << 12) |
              ((uint32_t)(s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f);
        i += 3;
    } else {
        *cp = 0x110000u + a;
        i = *offset + 1;
    }
    *offset = i;
    return true;
}

static bool valid_utf8(const char *s, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        uint32_t cp;
        utf8_next((const unsigned char *)s, len, &offset, &cp);
        if (cp >= 0x110000) return false;
    }
    return true;
}

static size_t transition(const aho_moderation_t *a, size_t state, uint32_t cp) {
    const aho_state_t *s = &a->states[state];
    for (size_t i = 0; i < s->edge_count; i++) {
        if (s->edges[i].codepoint == cp) return s->edges[i].state;
    }
    return SIZE_MAX;
}

static bool add_edge(aho_moderation_t *a, size_t state, uint32_t cp, size_t target) {
    aho_state_t *s = &a->states[state];
    if (s->edge_count == s->edge_capacity) {
        size_t capacity = s->edge_capacity ? s->edge_capacity * 2 : 2;
        aho_edge_t *edges = realloc(s->edges, capacity * sizeof(*edges));
        if (!edges) return false;
        s->edges = edges;
        s->edge_capacity = capacity;
    }
    s->edges[s->edge_count++] = (aho_edge_t){cp, target};
    return true;
}

static bool add_output(aho_moderation_t *a, size_t state, size_t rule) {
    aho_state_t *s = &a->states[state];
    if (s->output_count == s->output_capacity) {
        size_t capacity = s->output_capacity ? s->output_capacity * 2 : 2;
        size_t *outputs = realloc(s->outputs, capacity * sizeof(*outputs));
        if (!outputs) return false;
        s->outputs = outputs;
        s->output_capacity = capacity;
    }
    s->outputs[s->output_count++] = rule;
    return true;
}

void aho_moderation_destroy(aho_moderation_t *a) {
    if (!a) return;
    for (size_t i = 0; i < a->state_count; i++) {
        free(a->states[i].edges);
        free(a->states[i].outputs);
    }
    free(a->states);
    free(a);
}

aho_moderation_t *aho_moderation_build(const char *rules, bool case_fold) {
    if (!rules || strlen(rules) > AHO_MODERATION_RULES_MAX_BYTES) return NULL;
    aho_moderation_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->case_fold = case_fold;
    a->state_capacity = 16;
    a->state_count = 1;
    a->states = calloc(a->state_capacity, sizeof(*a->states));
    if (!a->states) goto fail;

    size_t rule_count = 0;
    const char *line = rules;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len && line[len - 1] == '\r') len--;
        if (len) {
            if (++rule_count > AHO_MODERATION_MAX_RULES || !valid_utf8(line, len)) goto fail;
            size_t state = 0, offset = 0;
            while (offset < len) {
                uint32_t cp;
                utf8_next((const unsigned char *)line, len, &offset, &cp);
                cp = fold_codepoint(cp, case_fold);
                size_t next = transition(a, state, cp);
                if (next == SIZE_MAX) {
                    if (a->state_count >= AHO_MODERATION_MAX_STATES) goto fail;
                    if (a->state_count == a->state_capacity) {
                        size_t capacity = a->state_capacity * 2;
                        aho_state_t *states = realloc(a->states, capacity * sizeof(*states));
                        if (!states) goto fail;
                        memset(states + a->state_capacity, 0, (capacity - a->state_capacity) * sizeof(*states));
                        a->states = states;
                        a->state_capacity = capacity;
                    }
                    next = a->state_count++;
                    if (!add_edge(a, state, cp, next)) goto fail;
                }
                state = next;
            }
            if (!add_output(a, state, rule_count - 1)) goto fail;
        }
        if (!end) break;
        line = end + 1;
    }

    size_t *queue = malloc(a->state_count * sizeof(*queue));
    if (!queue) goto fail;
    size_t head = 0, tail = 0;
    for (size_t i = 0; i < a->states[0].edge_count; i++) queue[tail++] = a->states[0].edges[i].state;
    while (head < tail) {
        size_t state = queue[head++];
        size_t edge_count = a->states[state].edge_count;
        for (size_t i = 0; i < edge_count; i++) {
            uint32_t cp = a->states[state].edges[i].codepoint;
            size_t child = a->states[state].edges[i].state;
            size_t failure = a->states[state].failure;
            while (failure && transition(a, failure, cp) == SIZE_MAX) failure = a->states[failure].failure;
            size_t fallback = transition(a, failure, cp);
            a->states[child].failure = fallback == SIZE_MAX ? 0 : fallback;
            for (size_t j = 0; j < a->states[a->states[child].failure].output_count; j++) {
                if (!add_output(a, child, a->states[a->states[child].failure].outputs[j])) {
                    free(queue);
                    goto fail;
                }
            }
            queue[tail++] = child;
        }
    }
    free(queue);
    return a;
fail:
    aho_moderation_destroy(a);
    return NULL;
}

aho_match_result_t aho_moderation_search(const aho_moderation_t *a, const char *text, size_t match_limit) {
    aho_match_result_t result = {0};
    if (!a || !text || !match_limit) return result;
    size_t state = 0, offset = 0, len = strlen(text);
    while (offset < len) {
        uint32_t cp;
        utf8_next((const unsigned char *)text, len, &offset, &cp);
        cp = fold_codepoint(cp, a->case_fold);
        while (state && transition(a, state, cp) == SIZE_MAX) state = a->states[state].failure;
        size_t next = transition(a, state, cp);
        state = next == SIZE_MAX ? 0 : next;
        for (size_t i = 0; i < a->states[state].output_count; i++) {
            result.count++;
            if (result.count >= match_limit) {
                result.limit_reached = true;
                return result;
            }
        }
    }
    return result;
}
