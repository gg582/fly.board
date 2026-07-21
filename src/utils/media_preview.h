#ifndef MEDIA_PREVIEW_H
#define MEDIA_PREVIEW_H

#include <stdbool.h>
#include <cwist/core/db/sql.h>

int media_quality_score_from_link(const char *score_str, const char *effective_type, const char *downlink_str,
                                  const char *rtt_str, const char *retry_str, const char *timeout_str,
                                  const char *save_data_str);
/* Compute max preview dimensions based on quality score and source size while preserving aspect ratio */
void media_preview_dimensions_from_score(int score, int src_w, int src_h,
                                         int *image_max_w, int *image_max_h,
                                         int *video_max_h);

bool get_media_dimensions(const char *src, bool is_video, int *w, int *h);
bool generate_image_thumb(const char *src, const char *dst, int max_w, int max_h);
bool generate_gif_thumb(const char *src, const char *dst, int max_w, int max_h, int fps);
bool generate_static_asset_webp(const char *src, const char *dst, int max_w, int max_h);
bool generate_video_thumb(const char *src, const char *dst, int max_w, int max_h);
bool generate_video_preview(const char *src, const char *dst, int max_h);
bool generate_audio_preview(const char *src, const char *dst, int bitrate_kbps);
void media_preview_backfill(cwist_db *db);
void media_preview_backfill_static_assets(void);

#endif
