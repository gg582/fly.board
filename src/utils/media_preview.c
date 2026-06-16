#define _POSIX_C_SOURCE 200809L
#include "media_preview.h"
#include "utils.h"
#include "db/db.h"
#include <cjson/cJSON.h>
#include <cwist/core/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

static bool run_ffmpeg(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    int status = pclose(fp);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool generate_image_thumb(const char *src, const char *dst, int max_w, int max_h) {
    if (!src || !dst || max_w <= 0 || max_h <= 0) return false;
    dir_ensure("public/uploads/.thumbs");
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel error -i '%s' -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' -frames:v 1 -c:v libwebp -quality 82 -compression_level 5 -y '%s'",
        src, max_w, max_h, dst);
    return run_ffmpeg(cmd);
}

bool generate_video_thumb(const char *src, const char *dst, int max_w, int max_h) {
    if (!src || !dst || max_w <= 0 || max_h <= 0) return false;
    dir_ensure("public/uploads/.thumbs");
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel error -i '%s' -ss 00:00:01 -vframes 1 -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' -q:v 3 -y '%s'",
        src, max_w, max_h, dst);
    return run_ffmpeg(cmd);
}

bool generate_video_preview(const char *src, const char *dst, int max_h) {
    if (!src || !dst || max_h <= 0) return false;
    dir_ensure("public/uploads/.previews");
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel error -i '%s' -vf 'scale=-2:min(%d\\,ih)' -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p -c:a aac -b:a 160k -movflags +faststart -y '%s'",
        src, max_h, dst);
    return run_ffmpeg(cmd);
}

bool generate_audio_preview(const char *src, const char *dst, int bitrate_kbps) {
    if (!src || !dst || bitrate_kbps <= 0) return false;
    dir_ensure("public/uploads/.previews");
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel error -i '%s' -b:a %dk -f mp3 -y '%s'",
        src, bitrate_kbps, dst);
    return run_ffmpeg(cmd);
}

static bool regular_file_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static const char *json_str(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

static int json_id(cJSON *obj) {
    cJSON *v = cJSON_GetObjectItem(obj, "id");
    if (!v) return 0;
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring) return atoi(v->valuestring);
    return 0;
}

void media_preview_backfill(cwist_db *db) {
    if (!db) return;
    dir_ensure("public/uploads/.thumbs");
    dir_ensure("public/uploads/.previews");

    cJSON *files = db_file_list_all(db);
    if (!files) return;

    int generated = 0;
    int failed = 0;
    int n = cJSON_GetArraySize(files);
    for (int i = 0; i < n; i++) {
        cJSON *file = cJSON_GetArrayItem(files, i);
        int id = json_id(file);
        const char *path = json_str(file, "file_path");
        const char *filename = json_str(file, "filename");
        const char *mime = json_str(file, "mime_type");
        const char *thumb_path = json_str(file, "thumb_path");
        const char *preview_path = json_str(file, "preview_path");
        if (id <= 0 || !regular_file_exists(path)) continue;

        char detected[128] = {0};
        if (!mime[0] || strcmp(mime, "application/octet-stream") == 0) {
            if (mime_type_from_data(path, detected, sizeof(detected))) {
                mime = detected;
            } else {
                mime = mime_type(filename);
            }
        }

        char next_thumb[512] = {0};
        char next_preview[512] = {0};
        bool changed = false;
        bool ok = true;

        if (strncmp(mime, "image/", 6) == 0) {
            snprintf(next_thumb, sizeof(next_thumb), "public/uploads/.thumbs/%d.webp", id);
            snprintf(next_preview, sizeof(next_preview), "%s", preview_path);
            if (!regular_file_exists(next_thumb)) {
                ok = generate_image_thumb(path, next_thumb, 1280, 1280);
                if (ok) generated++;
            }
            changed = ok && strcmp(thumb_path, next_thumb) != 0;
        } else if (strncmp(mime, "video/", 6) == 0) {
            snprintf(next_thumb, sizeof(next_thumb), "public/uploads/.thumbs/%d.webp", id);
            snprintf(next_preview, sizeof(next_preview), "public/uploads/.previews/%d.mp4", id);
            if (!regular_file_exists(next_thumb)) {
                ok = generate_video_thumb(path, next_thumb, 480, 270);
                if (ok) generated++;
            }
            if (ok && !regular_file_exists(next_preview)) {
                ok = generate_video_preview(path, next_preview, 1080);
                if (ok) generated++;
            }
            changed = ok && (strcmp(thumb_path, next_thumb) != 0 || strcmp(preview_path, next_preview) != 0);
        } else if (strncmp(mime, "audio/", 6) == 0) {
            snprintf(next_thumb, sizeof(next_thumb), "%s", thumb_path);
            snprintf(next_preview, sizeof(next_preview), "public/uploads/.previews/%d.mp3", id);
            if (!regular_file_exists(next_preview)) {
                ok = generate_audio_preview(path, next_preview, 192);
                if (ok) generated++;
            }
            changed = ok && strcmp(preview_path, next_preview) != 0;
        } else {
            continue;
        }

        if (!ok) {
            failed++;
            CWIST_LOG_WARN("Media preview generation failed: file_id=%d path=%s", id, path);
            continue;
        }
        if (changed) {
            db_file_set_preview_paths(db, id, next_thumb, next_preview);
        }
    }
    CWIST_LOG_INFO("Media preview backfill completed: files=%d generated=%d failed=%d", n, generated, failed);
    cJSON_Delete(files);
}
