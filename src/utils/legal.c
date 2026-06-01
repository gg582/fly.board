#define _POSIX_C_SOURCE 200809L
#include <cwist/legal.h>
#include "render/render.h"
#include <cwist/core/sstring/sstring.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void make_title(const char *src, char *out, size_t out_len) {
    size_t ti = 0;
    for (size_t i = 0; src[i] && ti < out_len - 1; i++) {
        if (i == 0 || src[i - 1] == '_') {
            char c = src[i];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            out[ti++] = c;
        } else if (src[i] == '_') {
            out[ti++] = ' ';
        } else {
            out[ti++] = src[i];
        }
    }
    out[ti] = '\0';
}

cJSON *legal_load_docs(void) {
    cJSON *arr = cJSON_CreateArray();
    DIR *d = opendir("legal");
    if (!d) {
        cJSON_Delete(arr);
        return NULL;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len <= 3 || strcmp(ent->d_name + len - 3, ".md") != 0)
            continue;

        char path[512];
        snprintf(path, sizeof(path), "legal/%s", ent->d_name);
        char *md = read_file(path);
        if (!md) continue;

        cwist_sstring *html = render_markdown_to_html(md);
        free(md);
        if (!html) continue;

        char *name = strdup(ent->d_name);
        name[len - 3] = '\0';

        bool required = (strncmp(name, "must_", 5) == 0);
        const char *title_src = required ? name + 5 : name;
        char title[256];
        make_title(title_src, title, sizeof(title));

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", name);
        cJSON_AddStringToObject(obj, "title", title);
        cJSON_AddStringToObject(obj, "html", html->data);
        cJSON_AddBoolToObject(obj, "required", required);
        cJSON_AddItemToArray(arr, obj);

        free(name);
        cwist_sstring_destroy(html);
    }
    closedir(d);
    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(arr);
        return NULL;
    }
    return arr;
}
