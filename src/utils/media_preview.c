#define _POSIX_C_SOURCE 200809L
#include "media_preview.h"
#include "utils.h"
#include "db/db.h"
#include <cjson/cJSON.h>
#include <cwist/core/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>

#include <pthread.h>
#include <dirent.h>

static char *escape_shell_arg(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *escaped = malloc(len * 4 + 1);
    if (!escaped) return NULL;
    char *dst = escaped;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\'') {
            *dst++ = '\'';
            *dst++ = '\\';
            *dst++ = '\'';
            *dst++ = '\'';
        } else {
            *dst++ = src[i];
        }
    }
    *dst = '\0';
    return escaped;
}

static bool validate_media_path(const char *path) {
    if (!path || !path[0]) return false;
    if (!is_safe_public_path(path)) return false;
    const char *unsafe = ";'\"`|&<>(){}[]$\\\n\r";
    if (strpbrk(path, unsafe)) return false;
    return true;
}

static bool run_ffmpeg(const char *cmd) {
    char timeout_cmd[8192];
    snprintf(timeout_cmd, sizeof(timeout_cmd), "timeout 15 %s", cmd);
    FILE *fp = popen(timeout_cmd, "r");
    if (!fp) {
        return false;
    }
    int status = pclose(fp);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int media_quality_score_from_link(const char *score_str, const char *effective_type, const char *downlink_str,
                                  const char *rtt_str, const char *retry_str, const char *timeout_str,
                                  const char *save_data_str) {
    int explicit_score = score_str ? atoi(score_str) : 0;
    if (explicit_score > 0) {
        if (explicit_score < 10) explicit_score = 10;
        if (explicit_score > 100) explicit_score = 100;
        return explicit_score;
    }

    double downlink = downlink_str ? atof(downlink_str) : 0.0;
    double rtt = rtt_str ? atof(rtt_str) : 0.0;
    int retries = retry_str ? atoi(retry_str) : 0;
    int timeouts = timeout_str ? atoi(timeout_str) : 0;
    int score = 55;

    if (effective_type && strcmp(effective_type, "4g") == 0) score += 24;
    else if (effective_type && strcmp(effective_type, "3g") == 0) score += 10;
    else if (effective_type && (!strcmp(effective_type, "2g") || !strcmp(effective_type, "slow-2g"))) score -= 10;

    if (downlink >= 30.0) score += 18;
    else if (downlink >= 10.0) score += 12;
    else if (downlink >= 3.0) score += 6;
    else if (downlink > 0.0 && downlink < 1.5) score -= 10;

    if (rtt > 0.0) {
        if (rtt <= 60.0) score += 16;
        else if (rtt <= 120.0) score += 8;
        else if (rtt <= 220.0) score += 2;
        else if (rtt <= 450.0) score -= 10;
        else score -= 18;
    }

    (void)retries;
    (void)timeouts;

    if (save_data_str && (!strcmp(save_data_str, "1") || !strcasecmp(save_data_str, "true"))) score -= 10;

    if (score < 10) score = 10;
    if (score > 100) score = 100;
    return score;
}

void media_preview_dimensions_from_score(int score, int src_w, int src_h,
                                         int *image_max_w, int *image_max_h,
                                         int *video_max_h) {
    /* Base max dimensions based on quality score */
    int base_w = 1280;
    int base_h = 1280;
    int base_video_h = 1080;

    if (score < 25) {
        base_w = 1080;  base_h = 1080;  base_video_h = 720;
    } else if (score < 45) {
        base_w = 1280;  base_h = 1280; base_video_h = 900;
    }

    /* Preserve aspect ratio for images */
    if (src_w > 0 && src_h > 0) {
        double ratio = (double)src_w / (double)src_h;
        int w = base_w;
        int h = (int)(base_w / ratio);
        if (h > base_h) { h = base_h; w = (int)(base_h * ratio); }
        if (image_max_w) *image_max_w = w;
        if (image_max_h) *image_max_h = h;
    } else {
        if (image_max_w) *image_max_w = base_w;
        if (image_max_h) *image_max_h = base_h;
    }

    /* Video height: keep width scaling implicit, only limit height */
    if (video_max_h) *video_max_h = base_video_h;
}


bool generate_image_thumb(const char *src, const char *dst, int max_w, int max_h) {
    if (!src || !dst || max_w <= 0 || max_h <= 0) return false;
    if (!validate_media_path(src) || !validate_media_path(dst)) return false;
    dir_ensure("public/uploads/.thumbs");

    char *esc_src = escape_shell_arg(src);
    char *esc_dst = escape_shell_arg(dst);
    if (!esc_src || !esc_dst) {
        free(esc_src);
        free(esc_dst);
        return false;
    }

    int quality = 60;
    int compression = 5;
    if (strstr(src, "uploads") != NULL) {
        quality = 82;
        compression = 5;
    }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel error -threads 1 -i '%s' -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' -frames:v 1 -c:v libwebp -quality %d -compression_level %d -y '%s'",
        esc_src, max_w, max_h, quality, compression, esc_dst);
    bool ok = run_ffmpeg(cmd);
    free(esc_src);
    free(esc_dst);
    return ok;
}

bool generate_gif_thumb(const char *src, const char *dst, int max_w, int max_h, int fps) {
    if (!src || !dst || max_w <= 0 || max_h <= 0 || fps <= 0) return false;
    if (!validate_media_path(src) || !validate_media_path(dst)) return false;
    dir_ensure("public/uploads/.thumbs");

    char *esc_src = escape_shell_arg(src);
    char *esc_dst = escape_shell_arg(dst);
    if (!esc_src || !esc_dst) {
        free(esc_src);
        free(esc_dst);
        return false;
    }

    int needed = snprintf(NULL, 0,
        "ffmpeg -hide_banner -loglevel error -threads 1 -i '%s' "
        "-filter_complex '[0:v]fps=%d,scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,palettegen=stats_mode=diff[p];"
        "[0:v]fps=%d,scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos[x];"
        "[x][p]paletteuse=dither=bayer:bayer_scale=3' -loop 0 -y '%s'",
        esc_src, fps, max_w, max_h, fps, max_w, max_h, esc_dst);
    if (needed <= 0) {
        free(esc_src);
        free(esc_dst);
        return false;
    }
    char *cmd = (char *)malloc((size_t)needed + 1);
    if (!cmd) {
        free(esc_src);
        free(esc_dst);
        return false;
    }
    snprintf(cmd, (size_t)needed + 1,
        "ffmpeg -hide_banner -loglevel error -threads 1 -i '%s' "
        "-filter_complex '[0:v]fps=%d,scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,palettegen=stats_mode=diff[p];"
        "[0:v]fps=%d,scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos[x];"
        "[x][p]paletteuse=dither=bayer:bayer_scale=3' -loop 0 -y '%s'",
        esc_src, fps, max_w, max_h, fps, max_w, max_h, esc_dst);
    bool ok = run_ffmpeg(cmd);
    free(cmd);
    free(esc_src);
    free(esc_dst);
    return ok;
}

bool generate_static_asset_webp(const char *src, const char *dst, int max_w, int max_h) {
    if (!src || !dst || max_w <= 0 || max_h <= 0) return false;
    if (!validate_media_path(src) || !validate_media_path(dst)) return false;
    dir_ensure("public/uploads/.thumbs");

    char *esc_src = escape_shell_arg(src);
    char *esc_dst = escape_shell_arg(dst);
    if (!esc_src || !esc_dst) {
        free(esc_src);
        free(esc_dst);
        return false;
    }

    int quality = 55;
    int compression = 6;
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel error -threads 1 -i '%s' -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' -frames:v 1 -c:v libwebp -quality %d -compression_level %d -y '%s'",
        esc_src, max_w, max_h, quality, compression, esc_dst);
    bool ok = run_ffmpeg(cmd);
    free(esc_src);
    free(esc_dst);
    return ok;
}

bool generate_video_thumb(const char *src, const char *dst, int max_w, int max_h) {
    if (!src || !dst || max_w <= 0 || max_h <= 0) return false;
    if (!validate_media_path(src) || !validate_media_path(dst)) return false;
    dir_ensure("public/uploads/.thumbs");

    char *esc_src = escape_shell_arg(src);
    char *esc_dst = escape_shell_arg(dst);
    if (!esc_src || !esc_dst) {
        free(esc_src);
        free(esc_dst);
        return false;
    }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel error -threads 1 -i '%s' -ss 00:00:01 -vframes 1 -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' -q:v 3 -y '%s'",
        esc_src, max_w, max_h, esc_dst);
    bool ok = run_ffmpeg(cmd);
    free(esc_src);
    free(esc_dst);
    return ok;
}

bool generate_video_preview(const char *src, const char *dst, int max_h) {
    if (!src || !dst || max_h <= 0) return false;
    if (!validate_media_path(src) || !validate_media_path(dst)) return false;
    dir_ensure("public/uploads/.previews");

    char *esc_src = escape_shell_arg(src);
    char *esc_dst = escape_shell_arg(dst);
    if (!esc_src || !esc_dst) {
        free(esc_src);
        free(esc_dst);
        return false;
    }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel error -threads 1 -i '%s' -vf 'scale=-2:min(%d\\,ih)' -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p -c:a aac -b:a 160k -movflags +faststart -y '%s'",
        esc_src, max_h, esc_dst);
    bool ok = run_ffmpeg(cmd);
    free(esc_src);
    free(esc_dst);
    return ok;
}

bool generate_webm_preview(const char *src, const char *dst, int max_h) {
    if (!src || !dst || max_h <= 0) return false;
    if (!validate_media_path(src) || !validate_media_path(dst)) return false;
    dir_ensure("public/uploads/.previews");

    char *esc_src = escape_shell_arg(src);
    char *esc_dst = escape_shell_arg(dst);
    if (!esc_src || !esc_dst) {
        free(esc_src);
        free(esc_dst);
        return false;
    }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel error -threads 1 -i '%s' -vf 'scale=-2:min(%d\\,ih)' -c:v libvpx -crf 10 -b:v 1M -an -y '%s'",
        esc_src, max_h, esc_dst);
    bool ok = run_ffmpeg(cmd);
    free(esc_src);
    free(esc_dst);
    return ok;
}

bool generate_audio_preview(const char *src, const char *dst, int bitrate_kbps) {
    if (!src || !dst || bitrate_kbps <= 0) return false;
    if (!validate_media_path(src) || !validate_media_path(dst)) return false;
    dir_ensure("public/uploads/.previews");

    char *esc_src = escape_shell_arg(src);
    char *esc_dst = escape_shell_arg(dst);
    if (!esc_src || !esc_dst) {
        free(esc_src);
        free(esc_dst);
        return false;
    }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel error -threads 1 -i '%s' -b:a %dk -f mp3 -y '%s'",
        esc_src, bitrate_kbps, esc_dst);
    bool ok = run_ffmpeg(cmd);
    free(esc_src);
    free(esc_dst);
    return ok;
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

        if (strcmp(mime, "image/gif") == 0) {
            snprintf(next_thumb, sizeof(next_thumb), "public/uploads/.thumbs/%d_animated_v2.gif", id);
            snprintf(next_preview, sizeof(next_preview), "public/uploads/.previews/%d.mp4", id);
            if (!regular_file_exists(next_thumb)) {
                ok = generate_gif_thumb(path, next_thumb, 1024, 1024, 12);
                if (ok) generated++;
            }
            if (ok && !regular_file_exists(next_preview)) {
                ok = generate_video_preview(path, next_preview, 720);
                if (ok) generated++;
            }
            changed = ok && strcmp(thumb_path, next_thumb) != 0;
        } else if (strncmp(mime, "image/", 6) == 0) {
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
                ok = generate_video_thumb(path, next_thumb, 1280, 720);
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

typedef struct {
    int w;
    int h;
} thumb_size_t;

static void generate_static_asset_thumbs(const char *scope_prefix, const char *src_path,
                                         const char *filename,
                                         const thumb_size_t *sizes, size_t count,
                                         int *generated, int *failed) {
    char scope_fname[512] = {0};
    snprintf(scope_fname, sizeof(scope_fname), "%s_", scope_prefix);
    char *p = scope_fname + strlen(scope_fname);
    for (size_t i = 0; filename[i] && (size_t)(p - scope_fname) < sizeof(scope_fname) - 1; i++) {
        *p++ = (filename[i] == '/') ? '_' : filename[i];
    }
    *p = '\0';

    for (size_t i = 0; i < count; i++) {
        char dst[PATH_MAX];
        snprintf(dst, sizeof(dst), "public/uploads/.thumbs/asset_%s_%dx%d.webp",
                 scope_fname, sizes[i].w, sizes[i].h);
        if (regular_file_exists(dst)) continue;
        if (generate_image_thumb(src_path, dst, sizes[i].w, sizes[i].h)) {
            (*generated)++;
        } else {
            (*failed)++;
            CWIST_LOG_WARN("Static asset thumb failed: %s -> %s", src_path, dst);
        }
    }
}

static void backfill_static_dir(const char *scope_prefix, const char *dir_path,
                                const thumb_size_t *sizes, size_t count,
                                int *generated, int *failed, int *scanned) {
    DIR *d = opendir(dir_path);
    if (!d) {
        CWIST_LOG_WARN("Static asset pre-generation skipped: %s not found", dir_path);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char src_path[PATH_MAX];
        snprintf(src_path, sizeof(src_path), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (stat(src_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        const char *mt = mime_type(ent->d_name);
        if (!mt || strncmp(mt, "image/", 6) != 0) continue;
        (*scanned)++;
        if (strcmp(mt, "image/gif") == 0) {
            char scope_fname[512] = {0};
            snprintf(scope_fname, sizeof(scope_fname), "%s_", scope_prefix);
            char *p = scope_fname + strlen(scope_fname);
            for (size_t i = 0; ent->d_name[i] && (size_t)(p - scope_fname) < sizeof(scope_fname) - 1; i++) {
                *p++ = (ent->d_name[i] == '/') ? '_' : ent->d_name[i];
            }
            *p = '\0';
            for (size_t i = 0; i < count; i++) {
                char dst[PATH_MAX];
                snprintf(dst, sizeof(dst), "public/uploads/.thumbs/asset_%s_%dx%d.gif",
                         scope_fname, sizes[i].w, sizes[i].h);
                if (regular_file_exists(dst)) continue;
                if (generate_gif_thumb(src_path, dst, sizes[i].w, sizes[i].h, 12)) {
                    (*generated)++;
                } else {
                    (*failed)++;
                    CWIST_LOG_WARN("Static asset gif thumb failed: %s -> %s", src_path, dst);
                }
            }
            continue;
        }
        generate_static_asset_thumbs(scope_prefix, src_path, ent->d_name, sizes, count, generated, failed);
    }
    closedir(d);
}

void media_preview_backfill_static_assets(void) {
    dir_ensure("public/uploads/.thumbs");

    static const thumb_size_t img_sizes[] = {
        {128, 128},      /* favicon */
        {256, 256},      /* favicon retina */
        {512, 512},      /* logo */
        {1920, 1920},    /* default hero-bg / direct asset response (aspect preserved, no upscale) */
    };

    static const thumb_size_t profile_sizes[] = {
        {128, 128},      /* small profile pic */
        {256, 256},      /* standard profile pic */
        {512, 512},      /* large profile pic / default direct asset response */
    };

    int generated = 0;
    int failed = 0;
    int scanned = 0;
    backfill_static_dir("img", "public/img", img_sizes,
                        sizeof(img_sizes) / sizeof(img_sizes[0]), &generated, &failed, &scanned);
    backfill_static_dir("profile", "public/profile", profile_sizes,
                        sizeof(profile_sizes) / sizeof(profile_sizes[0]), &generated, &failed, &scanned);
    CWIST_LOG_INFO("Static asset thumb pre-generation completed: scanned=%d generated=%d failed=%d",
                   scanned, generated, failed);
}
