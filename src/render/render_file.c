#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>

cwist_sstring *render_file_detail(cJSON *file, cJSON *comments, bool dark, const char *user_role, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    if (!file) {
        cwist_sstring_assign(b, "<h1>File Not Found</h1>");
        cwist_sstring *page = render_page("File Detail", b->data, dark, user_role, profile_pic);
        cwist_sstring_destroy(b);
        return page;
    }

    cJSON *fid = cJSON_GetObjectItem(file, "id");
    cJSON *fname = cJSON_GetObjectItem(file, "filename");
    cJSON *stype = cJSON_GetObjectItem(file, "mime_type");
    cJSON *sz = cJSON_GetObjectItem(file, "size");
    char fid_buf[32];
    if (fid) snprintf(fid_buf, sizeof(fid_buf), "%d", fid->valueint);
    char sz_buf[32];
    snprintf(sz_buf, sizeof(sz_buf), "%lld", (long long)(sz ? sz->valueint : 0));

    cwist_sstring_assign(b, "<div class='hero'><h1>");
    cwist_sstring_append_escaped(b, fname ? fname->valuestring : "Unknown File");
    cwist_sstring_append(b, "</h1><p>File details</p></div>");

    cwist_sstring_append(b, "<div class='card'>");
    cwist_sstring_append(b, "<p><strong>MIME Type:</strong> ");
    cwist_sstring_append_escaped(b, stype && stype->valuestring ? stype->valuestring : "unknown");
    cwist_sstring_append(b, "</p><p><strong>Size:</strong> ");
    cwist_sstring_append(b, sz_buf);
    cwist_sstring_append(b, " bytes</p>");

    if (fid) {
        cwist_sstring_append(b, "<a href='/file/");
        cwist_sstring_append(b, fid_buf);
        cwist_sstring_append(b, "' class='btn'>Download File</a>");
    }
    cwist_sstring_append(b, "</div>");

    cwist_sstring *page = render_page(fname && fname->valuestring ? fname->valuestring : "File Detail", b->data, dark, user_role, profile_pic);
    cwist_sstring_destroy(b);
    return page;
}

cwist_sstring *render_file_repo(cJSON *files, bool dark, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>File Repository</h1><p>Shared files and attachments.</p></div>");
    cwist_sstring_append(b, "<div class='card' style='max-width:560px;margin:0 auto'><form action='/file/upload' method='post' enctype='multipart/form-data'>");
    cwist_sstring_append(b, "<label>Upload file</label><input type='file' name='file' required>");
    cwist_sstring_append(b, "<button type='submit' class='btn' style='margin-top:8px'>Upload</button>");
    cwist_sstring_append(b, "<small style='color:var(--muted);display:block;margin-top:6px'>Large files (&gt;1MB) are stored on volume; small files in DB.</small>");
    cwist_sstring_append(b, "</form></div>");

    cwist_sstring_append(b, "<h3 style='margin-top:28px'>Files</h3>");
    if (files && cJSON_GetArraySize(files) > 0) {
        cwist_sstring_append(b, "<div class='post-grid'>");
        int n = cJSON_GetArraySize(files);
        for (int i = 0; i < n; i++) {
            cJSON *f = cJSON_GetArrayItem(files, i);
            cJSON *fid = cJSON_GetObjectItem(f, "id");
            cJSON *fname = cJSON_GetObjectItem(f, "filename");
            cJSON *stype = cJSON_GetObjectItem(f, "mime_type");
            cJSON *sz = cJSON_GetObjectItem(f, "size");
            char fid_buf[32];
            snprintf(fid_buf, sizeof(fid_buf), "%d", fid->valueint);
            char sz_buf[32];
            snprintf(sz_buf, sizeof(sz_buf), "%lld", (long long)(sz ? sz->valueint : 0));
            cwist_sstring_append(b, "<article class='card'>");
            cwist_sstring_append(b, "<h4 style='margin-top:0'>");
            cwist_sstring_append_escaped(b, fname ? fname->valuestring : "Unknown");
            cwist_sstring_append(b, "</h4>");
            cwist_sstring_append(b, "<p style='color:var(--muted);font-size:13px'>");
            cwist_sstring_append(b, stype && stype->valuestring ? stype->valuestring : "unknown/mime");
            cwist_sstring_append(b, " &middot; ");
            cwist_sstring_append(b, sz_buf);
            cwist_sstring_append(b, " bytes</p>");
            cwist_sstring_append(b, "<div style='display:flex;gap:8px'><a href='/file/");
            cwist_sstring_append(b, fid_buf);
            cwist_sstring_append(b, "' class='btn' style='font-size:12px;padding:4px 10px'>Download</a>");
            cwist_sstring_append(b, "<form style='display:inline' action='/file/delete' method='post'>");
            cwist_sstring_append(b, "<input type='hidden' name='id' value='");
            cwist_sstring_append(b, fid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='font-size:12px;padding:4px 10px'>Delete</button></form></div>");
            cwist_sstring_append(b, "</article>");
        }
        cwist_sstring_append(b, "</div>");
    } else {
        cwist_sstring_append(b, "<div class='card' style='text-align:center;padding:40px 20px;color:var(--muted);'>No files uploaded yet.</div>");
    }
    cwist_sstring *page = render_page("Files", b->data, dark, "", profile_pic);
    cwist_sstring_destroy(b);
    return page;
}
