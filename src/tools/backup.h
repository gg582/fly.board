#ifndef FLY_BACKUP_H
#define FLY_BACKUP_H

/* Site backup and migration: posts, comments, boards, signatures, uploads,
 * images and settings in one signed archive.
 *
 *   fly_board --backup  <archive> [--with-secrets] [--passphrase-file <f>]
 *   fly_board --verify  <archive> [--passphrase-file <f>]
 *   fly_board --restore <archive> [--force] [--passphrase-file <f>]
 *
 * Run from the site root (where public/ and data/ live). The passphrase for
 * --with-secrets comes from --passphrase-file, FLY_BACKUP_PASSPHRASE, or a
 * terminal prompt.
 *
 * Returns the process exit code when argv names one of these commands, or
 * -1 when it does not (normal server start). */
int fly_backup_cli(int argc, char **argv);

#endif
