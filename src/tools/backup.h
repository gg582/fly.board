#ifndef FLY_BACKUP_H
#define FLY_BACKUP_H

#include <stdbool.h>

/* Site backup and migration: posts, comments, boards, signatures, uploads,
 * images and settings in one signed archive.
 *
 *   fly_board --backup  <archive> [--with-secrets] [--passphrase-file <f>]
 *   fly_board --verify  <archive> [--passphrase-file <f>]
 *   fly_board --restore <archive> [--force] [--passphrase-file <f>]
 *   fly_board --export-markdown <dir> [--rewrite-links]
 *   fly_board --verify-markdown <dir>
 *
 * Run from the site root (where public/ and data/ live). The passphrase for
 * --with-secrets comes from --passphrase-file, FLY_BACKUP_PASSPHRASE, or a
 * terminal prompt.
 *
 * Returns the process exit code when argv names one of these commands, or
 * -1 when it does not (normal server start). */
int fly_backup_cli(int argc, char **argv);

/* Write a signed archive to out_path (as --backup does); a non-empty
 * passphrase also seals the secrets. Returns 0 on success. */
int fly_backup_write(const char *out_path, const char *passphrase);

/* ---- Scheduled backups (backup_schedule.c) ----
 * Enabled by registering a target in backup.settings (S3-compatible bucket
 * or a directory such as a mounted disk); then the daily 03:00 job runs
 * `fly_board --scheduled-backup` in a separate process. */
typedef struct {
    char target[8];          /* "s3", "dir" or "" (not registered) */
    bool enabled;
    char endpoint[256], region[64], bucket[128], access_key[128], secret_key[256], prefix[256];
    bool use_path_style;
    char path[512];          /* target=dir */
    int keep;                /* archives to keep; default 14 */
    char passphrase[256];    /* optional: also seal the secrets */
} backup_settings_t;

bool backup_settings_load(backup_settings_t *out);
bool backup_settings_save(const backup_settings_t *in);
bool backup_settings_remove(void);
/* Target fully configured and enabled. */
bool backup_settings_ready(const backup_settings_t *s);
/* Run one scheduled backup now (the --scheduled-backup command). */
int backup_scheduled_run(void);
/* Start --scheduled-backup as a detached process; false if it could not. */
bool backup_spawn(void);

#endif
