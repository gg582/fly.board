#define _POSIX_C_SOURCE 200809L
#include "media_preview.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
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
        "ffmpeg -hide_banner -loglevel error -i '%s' -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' -q:v 3 -y '%s'",
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

bool generate_audio_preview(const char *src, const char *dst, int bitrate_kbps) {
    if (!src || !dst || bitrate_kbps <= 0) return false;
    dir_ensure("public/uploads/.previews");
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -hide_banner -loglevel error -i '%s' -b:a %dk -f mp3 -y '%s'",
        src, bitrate_kbps, dst);
    return run_ffmpeg(cmd);
}
