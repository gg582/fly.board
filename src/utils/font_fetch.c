/**
 * @file font_fetch.c
 * @brief Startup downloader for admin-configured font files.
 *
 * fonts.settings may declare font_file=<filename> <url> entries.  At startup
 * each declared file is fetched into public/fonts/ unless it already exists,
 * so deployments only need the settings file — the server pulls the fonts
 * itself.  Files present in the repo (or previously downloaded) are left
 * untouched.  Uses libcurl, already linked for the translation API.
 */

#define _POSIX_C_SOURCE 200809L
#include "utils/font_fetch.h"
#include "config/config.h"
#include <cwist/core/log.h>
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define FONT_DIR "public/fonts"

static size_t write_file_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

static bool font_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

static bool download_one(const char *url, const char *tmp_path, const char *final_path) {
    FILE *out = fopen(tmp_path, "wb");
    if (!out) return false;

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(out);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_cb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fly_board-font-fetch/1.0");

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(out);

    if (rc != CURLE_OK || http_code >= 400) {
        remove(tmp_path);
        CWIST_LOG_WARN("Font download failed (%s): %s", url, curl_easy_strerror(rc));
        return false;
    }
    if (rename(tmp_path, final_path) != 0) {
        remove(tmp_path);
        return false;
    }
    return true;
}

void font_files_ensure_downloaded(void) {
    for (size_t i = 0; i < g_font_settings.download_count; i++) {
        const char *name = g_font_settings.download_name[i];
        char final_path[256];
        char tmp_path[256 + 8];
        snprintf(final_path, sizeof(final_path), "%s/%s", FONT_DIR, name);
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
        if (font_file_exists(final_path)) continue;
        CWIST_LOG_INFO("Downloading font %s from %s", name, g_font_settings.download_url[i]);
        if (download_one(g_font_settings.download_url[i], tmp_path, final_path)) {
            CWIST_LOG_INFO("Font %s downloaded to %s", name, final_path);
        }
    }
}
