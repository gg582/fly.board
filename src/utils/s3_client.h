#ifndef FLY_S3_CLIENT_H
#define FLY_S3_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

/* S3 object keys are stored in files.file_path with an "s3://" prefix. */
#define S3_PATH_PREFIX "s3://"

bool s3_path_is(const char *path);
/* Extract the object key from an "s3://..." path (pointer into path). */
const char *s3_path_key(const char *path);

/* Upload a local file as an object.  On success the caller may delete the
 * local copy and store "s3://<key>" as the file path. */
bool s3_upload_file(const char *local_path, const char *key, const char *content_type);
bool s3_delete_object(const char *key);
/* Push a freshly uploaded local file to the bucket (key = prefix+basename).
 * On success writes the "s3://<key>" marker into out_marker when provided. */
bool s3_store_upload(const char *local_path, const char *content_type,
                     char *out_marker, size_t out_marker_size);
/* Build a time-limited presigned GET URL (SigV4 query auth). */
bool s3_presign_get(const char *key, char *out, size_t out_size, int expires_sec);

/* Storage-aware delete: S3 objects go to the bucket, everything else is a
 * safe-checked local unlink.  Drop-in replacement at file_path unlink sites. */
void storage_delete_file(const char *path);

#endif
