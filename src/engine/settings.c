#include "engine/settings.h"
#include "engine/community.h"
#include "engine/bdr.h"
#include "auth/auth.h"
#include "audit/audit.h"
#include "config/config.h"
#include "render/theme.h"
#include "utils/font_fetch.h"
#include "utils/cache.h"
#include <cwist/core/log.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>

void engine_settings_appearance_refresh(void) {
    static pthread_mutex_t reload_mtx = PTHREAD_MUTEX_INITIALIZER;
    static _Atomic uint64_t loaded_version;
    uint64_t version = engine_bdr_appearance_version();
    if (!version || version == loaded_version) return;
    pthread_mutex_lock(&reload_mtx);
    if (version != loaded_version) {
        /* Every worker process keeps its own copy of these; the admin save
         * (or a replica pulling the primary's appearance) only bumps the
         * shared version, so each worker re-reads the files here. */
        theme_modes_load("blog.settings");
        theme_custom_load("data/custom-theme.json");
        layout_settings_load("layout.settings");
        font_settings_load("fonts.settings");
        page_cache_clear();
        loaded_version = version;
    }
    pthread_mutex_unlock(&reload_mtx);
}

bool engine_settings_load(void) {
    if (!auth_admin_load("admin.settings")) {
        FLY_LOG_ERROR("Failed to load admin.settings");
        return false;
    }
    CWIST_LOG_INFO("Admin settings loaded");
    blog_config_load("blog.settings");
    CWIST_LOG_INFO("Blog config loaded");
    font_settings_load("fonts.settings");
    layout_settings_load("layout.settings");
    theme_custom_load("data/custom-theme.json");
    CWIST_LOG_INFO("Font, layout, and custom theme settings loaded");
    font_files_ensure_downloaded();
    s3_config_load("s3.settings");
    if (s3_config_enabled()) CWIST_LOG_INFO("S3 storage enabled (bucket=%s)", g_s3_config.bucket);
    robots_config_load("robots.settings");
    CWIST_LOG_INFO("Robots policy loaded (robots=%s, llms=%s)", robots_level(), llms_level());
    upload_policy_load("upload.settings");
    CWIST_LOG_INFO("Upload policy loaded (mode=%s)", upload_policy_mode());
    community_config_load("community.settings");
    if (!audit_init("audit.settings")) return false;
    flywire_config_load("flywire.settings");
    if (flywire_is_primary()) CWIST_LOG_INFO("FlyWire: primary mode (serving /flywire/feed)");
    else if (flywire_is_replica()) CWIST_LOG_INFO("FlyWire: replica of %s", flywire_primary_url());
    return true;
}
