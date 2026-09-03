#include "engine/settings.h"
#include "auth/auth.h"
#include "config/config.h"
#include <cwist/core/log.h>

bool engine_settings_load(void) {
    if (!auth_admin_load("admin.settings")) {
        FLY_LOG_ERROR("Failed to load admin.settings");
        return false;
    }
    CWIST_LOG_INFO("Admin settings loaded");
    blog_config_load("blog.settings");
    CWIST_LOG_INFO("Blog config loaded");
    font_settings_load("fonts.settings");
    CWIST_LOG_INFO("Font settings loaded");
    s3_config_load("s3.settings");
    if (s3_config_enabled()) CWIST_LOG_INFO("S3 storage enabled (bucket=%s)", g_s3_config.bucket);
    robots_config_load("robots.settings");
    CWIST_LOG_INFO("Robots policy loaded (robots=%s, llms=%s)", robots_level(), llms_level());
    return true;
}
