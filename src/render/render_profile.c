#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
#include "config/config.h"
#include "utils/utils.h"
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <string.h>

cwist_sstring *render_profile(cJSON *user, bool dark, const char *user_role, const char *profile_pic, bool is_own_profile) {
    cwist_sstring *b = cwist_sstring_create();
    const char *username = cJSON_GetObjectItem(user, "username")->valuestring;
    const char *email = cJSON_GetObjectItem(user, "email")->valuestring;
    cJSON *pp = cJSON_GetObjectItem(user, "profile_pic");
    const char *user_profile_pic = (pp && pp->type == cJSON_String) ? pp->valuestring : "";
    cJSON *nickname_obj = cJSON_GetObjectItem(user, "nickname");
    const char *nickname = (nickname_obj && nickname_obj->type == cJSON_String) ? nickname_obj->valuestring : "";
    cJSON *bio_obj = cJSON_GetObjectItem(user, "bio");
    const char *bio = (bio_obj && bio_obj->type == cJSON_String) ? bio_obj->valuestring : "";
    cJSON *created_obj = cJSON_GetObjectItem(user, "created_at");
    const char *created_at = (created_obj && created_obj->type == cJSON_String) ? created_obj->valuestring : "";
    cJSON *uid_obj = cJSON_GetObjectItem(user, "id");
    int user_id = uid_obj ? uid_obj->valueint : 0;

    cJSON *owner_role_obj = cJSON_GetObjectItem(user, "role");
    const char *owner_role = (owner_role_obj && owner_role_obj->type == cJSON_String) ? owner_role_obj->valuestring : "";

    static char admin_pic_buf[512];
    const char *display_pic = user_profile_pic;
    if ((!display_pic || !display_pic[0]) && owner_role && strcmp(owner_role, "admin") == 0) {
        if (g_config.blog_logo[0]) {
            snprintf(admin_pic_buf, sizeof(admin_pic_buf), "/assets/img/%s", g_config.blog_logo);
            display_pic = admin_pic_buf;
        } else {
            display_pic = "/assets/img/logo.png";
        }
    }

    cwist_sstring_append(b, "<div class='card' style='max-width:600px;margin:20px auto;text-align:center'>");
    cwist_sstring_append(b, "<h2>User Profile</h2>");

    if (display_pic && display_pic[0]) {
        cwist_sstring_append(b, "<img src='");
        cwist_sstring_append(b, display_pic);
        cwist_sstring_append(b, "' class='profile-pic' style='margin:20px 0'><br>");
    } else {
        cwist_sstring_append(b, "<div style='width:100px;height:100px;background:#eee;border-radius:50%;margin:20px auto;display:flex;align-items:center;justify-content:center;color:#999'>No Pic</div>");
    }

    if (nickname && nickname[0]) {
        cwist_sstring_append(b, "<h3 style='margin-bottom:4px'>");
        cwist_sstring_append_escaped(b, nickname);
     
     
        cwist_sstring_append(b, "</h3>");
        cwist_sstring_append(b, "<p style='color:var(--muted);margin-top:0'>@");
        cwist_sstring_append_escaped(b, username);
        cwist_sstring_append(b, "</p>");
    } else {
        cwist_sstring_append(b, "<h3>");
        cwist_sstring_append_escaped(b, username);
        cwist_sstring_append(b, "</h3>");
    }

    cwist_sstring_append(b, "<p style='color:var(--muted)'>");
    cwist_sstring_append_escaped(b, email);
    cwist_sstring_append(b, "</p>");

    cwist_sstring_append(b, "<p style='color:var(--muted);font-size:13px'>");
    cwist_sstring_append(b, format_join_date(created_at));
    cwist_sstring_append(b, "</p>");

    if (bio && bio[0]) {
        cwist_sstring_append(b, "<div style='margin:16px 0;padding:12px;background:var(--bg-alt);border-radius:8px;text-align:left'>");
        cwist_sstring_append(b, "<p style='margin:0;color:var(--fg)'>");
        cwist_sstring_append_escaped(b, bio);
     
     
        cwist_sstring_append(b, "</p>");
        cwist_sstring_append(b, "</div>");
    }

    if (is_own_profile || (user_role && strcmp(user_role, "admin") == 0)) {
        cwist_sstring_append(b, "<hr style='margin:20px 0;border:0;border-top:1px solid var(--border)'>");
        if (is_own_profile) {
            cwist_sstring_append(b, "<a href='/account/settings' class='btn btn-outline'>Account Settings</a>");
        } else {
            char uid_buf[32];
            snprintf(uid_buf, sizeof(uid_buf), "%d", user_id);
            cwist_sstring_append(b, "<a href='/account/settings?id=");
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "' class='btn btn-outline'>Edit User Profile (Admin)</a>");
        }
    } else if (user_id > 0) {
        cwist_sstring_append(b, "<hr style='margin:20px 0;border:0;border-top:1px solid var(--border)'>");
        char uid_buf[32];
        snprintf(uid_buf, sizeof(uid_buf), "%d", user_id);
        cwist_sstring_append(b, "<a href='/user/");
        cwist_sstring_append(b, uid_buf);
        cwist_sstring_append(b, "' class='btn btn-outline'>View Public Profile</a>");
    }

    cwist_sstring_append(b, "</div>");

    cwist_sstring *res = render_page("Profile", b->data, dark, user_role, profile_pic);
    cwist_sstring_destroy(b);
    return res;
}

cwist_sstring *render_account_settings(cJSON *user, bool dark, const char *viewer_role, const char *profile_pic, const char *error) {
    cwist_sstring *b = cwist_sstring_create();
    const char *username = cJSON_GetObjectItem(user, "username")->valuestring;
    const char *email = cJSON_GetObjectItem(user, "email")->valuestring;
    cJSON *pp = cJSON_GetObjectItem(user, "profile_pic");
    const char *current_pic = (pp && pp->type == cJSON_String) ? pp->valuestring : "";
    cJSON *nickname_obj = cJSON_GetObjectItem(user, "nickname");
    const char *nickname = (nickname_obj && nickname_obj->type == cJSON_String) ? nickname_obj->valuestring : "";
    cJSON *bio_obj = cJSON_GetObjectItem(user, "bio");
    const char *bio = (bio_obj && bio_obj->type == cJSON_String) ? bio_obj->valuestring : "";

    cwist_sstring_append(b, "<div class='card' style='max-width:600px;margin:20px auto'>");
    cwist_sstring_append(b, "<h2>Account Settings</h2>");

    if (current_pic && current_pic[0]) {
        cwist_sstring_append(b, "<div style='text-align:center;margin-bottom:16px'><img src='");
        cwist_sstring_append(b, current_pic);
        cwist_sstring_append(b, "' class='profile-pic' style='width:80px;height:80px'></div>");
    }

    if (error && error[0]) {
        cwist_sstring_append(b, "<div class='alert'>");
        cwist_sstring_append_escaped(b, error);
        cwist_sstring_append(b, "</div>");
    }

    cwist_sstring_append(b, "<form action='/account/settings' method='POST' enctype='multipart/form-data'>");

    cJSON *id_obj = cJSON_GetObjectItem(user, "id");
    if (id_obj) {
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "%d", id_obj->valueint);
        cwist_sstring_append(b, "<input type='hidden' name='id' value='");
        cwist_sstring_append(b, id_buf);
        cwist_sstring_append(b, "'>");
    }

    cwist_sstring_append(b, "<label>Nickname</label><input name='nickname' value='");
    cwist_sstring_append_escaped(b, nickname);
    cwist_sstring_append(b, "' placeholder='Your display name'>");

    cwist_sstring_append(b, "<label>Bio</label><textarea name='bio' rows='4' placeholder='Tell us about yourself'>");
    cwist_sstring_append_escaped(b, bio);
    cwist_sstring_append(b, "</textarea>");

    cwist_sstring_append(b, "<label>Profile Picture</label><input type='file' name='profile_pic' accept='image/*'>");
    cwist_sstring_append(b, "<small style='color:var(--muted)'>Leave empty to keep current picture.</small>");

    cwist_sstring_append(b, "<div style='margin-top:16px;display:flex;gap:10px'>");
    cwist_sstring_append(b, "<button type='submit' class='btn'>Save Changes</button>");
    cwist_sstring_append(b, "<a href='/profile' class='btn btn-outline'>Cancel</a>");
    cwist_sstring_append(b, "</div>");

    cwist_sstring_append(b, "</form>");

    cwist_sstring_append(b, "<hr style='margin:24px 0;border:0;border-top:1px solid var(--border)'>");
    cwist_sstring_append(b, "<a href='/account/password' class='btn btn-outline'>Change Password</a>");

    cwist_sstring_append(b, "<hr style='margin:24px 0;border:0;border-top:1px solid var(--border)'>");
    cwist_sstring_append(b, "<p style='color:var(--muted);font-size:13px'>Username: ");
    cwist_sstring_append_escaped(b, username);
    cwist_sstring_append(b, "<br>Email: ");
    cwist_sstring_append_escaped(b, email);
    cwist_sstring_append(b, "</p>");

    cwist_sstring_append(b, "</div>");

    cwist_sstring *res = render_page("Account Settings", b->data, dark, viewer_role, profile_pic);
    cwist_sstring_destroy(b);
    return res;
}
