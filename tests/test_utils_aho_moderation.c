#include "utils/aho_moderation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static size_t match(aho_moderation_t *a, const char *text) {
    return aho_moderation_search(a, text, 1024).count;
}

int main(void) {
    aho_moderation_t *a = aho_moderation_build("spam\n금지단어\nплохое", true);
    CHECK(a != NULL, "build multilingual automaton");
    CHECK(match(a, "clean text") == 0, "clean text has no matches");
    CHECK(match(a, "SPAM and spam") == 2, "English simple case folding");
    CHECK(match(a, "이 문장은 금지단어를 포함합니다") == 1, "Korean UTF-8 match");
    CHECK(match(a, "ПЛОХОЕ и плохое") == 2, "Cyrillic simple case folding");
    CHECK(match(a, "Ёлка") == 0, "unrelated Cyrillic does not match");
    aho_moderation_destroy(a);

    a = aho_moderation_build("spam\nСпам", false);
    CHECK(match(a, "SPAM спам СПАМ") == 0, "exact policy does not fold case");
    CHECK(match(a, "spam Спам") == 2, "exact policy matches exact UTF-8");
    aho_moderation_destroy(a);

    a = aho_moderation_build("he\nshe\nhers\nhis", true);
    CHECK(match(a, "ushers") == 3, "overlapping and suffix matches all count");
    aho_match_result_t limited = aho_moderation_search(a, "ushers", 2);
    CHECK(limited.count == 2 && limited.limit_reached, "match limit stops overlapping scan");
    limited = aho_moderation_search(a, "clean", 2);
    CHECK(limited.count == 0 && !limited.limit_reached, "clean scan does not report limit");
    aho_moderation_destroy(a);

    a = aho_moderation_build("spam\n한글", true);
    const char malformed_around[] = {'s', 'p', 'a', 'm', (char)0xff, (char)0xc0, (char)0x80, 0};
    CHECK(match(a, malformed_around) == 1, "malformed bytes after a match are skipped safely");
    const char malformed_inside[] = {'s', 'p', (char)0xe2, (char)0x28, (char)0xa1, 'a', 'm', 0};
    CHECK(match(a, malformed_inside) == 0, "malformed sequence cannot create a false match");
    const char malformed_then_match[] = {(char)0xf4, (char)0x90, (char)0x80, (char)0x80, 's', 'p', 'a', 'm', 0};
    CHECK(match(a, malformed_then_match) == 1, "scanner resynchronizes after malformed UTF-8");
    aho_moderation_destroy(a);

    const char invalid_rule[] = {'b', 'a', 'd', (char)0xff, 0};
    CHECK(aho_moderation_build(invalid_rule, true) == NULL, "malformed UTF-8 rule is rejected");
    a = aho_moderation_build("one\r\n\ntwo\r\n", true);
    CHECK(a != NULL, "CRLF and blank rules are accepted");
    CHECK(match(a, "one two") == 2, "CRLF rules match without carriage returns");
    aho_moderation_destroy(a);

    char *too_long = malloc(AHO_MODERATION_MAX_STATES + 2);
    memset(too_long, 'a', AHO_MODERATION_MAX_STATES + 1);
    too_long[AHO_MODERATION_MAX_STATES + 1] = '\0';
    CHECK(aho_moderation_build(too_long, true) == NULL, "automaton state bound is enforced");
    free(too_long);

    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}
