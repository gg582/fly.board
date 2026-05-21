#ifndef MEDIA_PREVIEW_H
#define MEDIA_PREVIEW_H

#include <stdbool.h>

bool generate_image_thumb(const char *src, const char *dst, int max_w, int max_h);
bool generate_video_thumb(const char *src, const char *dst, int max_w, int max_h);
bool generate_audio_preview(const char *src, const char *dst, int bitrate_kbps);

#endif
