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
    return true;
}
