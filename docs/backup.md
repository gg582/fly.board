# Backups and restore

fly.board has a built-in, signed backup format (`.fbk`). The same CLI is
used for manual archives and scheduled backups; the implementation lives in
`src/tools/backup.c` (archive format) and `src/tools/backup_schedule.c`
(scheduled target + shipping).

## Manual backups

Run from the site root (where `public/` and `data/` live):

```sh
./fly_board --backup  /srv/backups/site.fbk                 # content + settings
./fly_board --backup  /srv/backups/site.fbk --with-secrets  # also sealed secrets
./fly_board --verify  /srv/backups/site.fbk
./fly_board --restore /srv/backups/site.fbk                 # fresh install; --force to replace an existing site
```

Options:

- `--with-secrets` — additionally seals the secret files into the archive
  (see below). Requires a passphrase of at least 12 characters, from
  `--passphrase-file <file>`, the `FLY_BACKUP_PASSPHRASE` environment
  variable, or an interactive prompt (entered twice).
- `--passphrase-file <file>` — non-interactive passphrase for `--backup`,
  `--verify` and `--restore`.
- `--force` (restore only) — replace an existing site. Without it, restore
  refuses to touch a site whose `data/blog.db` already exists.

Other content tools: `--export-markdown <dir> [--rewrite-links]`,
`--verify-markdown <dir>` (see README), and `--sign-posts` (re-sign posts
whose signature is missing or invalid).

## Archive contents and format

A `.fbk` file is a zstd-compressed tar stream:

```
b/000001 ... b/NNNNNN   file contents (blobs carry numbers, not paths)
secrets.enc             optional, encrypted
manifest.json           path, size, SHA-256 of every blob, key info
manifest.sig            ML-DSA-65 signature over manifest.json
```

What is included:

- **Databases** (consistent SQLite snapshots taken while the server runs):
  `data/blog.db`, `data/comments.db`, `data/board_tree.db`.
- **Uploaded/user files**, recursively: `public/uploads`,
  `public/profile`, `public/img`.
- **Settings**: `blog.settings`, `fonts.settings`, `robots.settings`,
  `upload.settings`.
- **Secrets** (only with `--with-secrets`): the post-signing seed
  (`data/.pqc_mldsa65_seed`), `data/.jwt_secret`, `admin.settings`,
  `s3.settings`, `backup.settings` — sealed as JSON with AES-256-GCM under a
  key derived with scrypt (N=2^17, r=8, p=1, ≤256 MiB) from the passphrase.

Integrity: `--verify` and `--restore` refuse an archive whose ML-DSA-65
manifest signature does not verify or whose blob hashes do not match. Only
paths on an internal allow-list are written during restore; each replaced
file is kept as `<name>.pre-restore-<time>` with `--force`. Backups are
written mode `0600` and are never placed under `public/`.

A site restored **without** its secrets starts with a fresh signing key and
still verifies every earlier post, because `pqc_keys` keeps every public key
that has ever signed on the site.

## Scheduled backups (`backup_schedule`)

Register a target in **Dashboard → Scheduled Backups** (or write
`backup.settings` by hand; mode 0600, keep private — it can hold S3
credentials and the archive passphrase):

```
target=s3                    # or "dir"
enabled=true
endpoint=                    # S3: e.g. https://s3.eu-central-1.amazonaws.com
region=
bucket=
access_key=
secret_key=
prefix=
use_path_style=false
path=/mnt/backups/flyboard   # dir target: must be an absolute path
keep=14
passphrase=
```

Semantics (`src/tools/backup_schedule.c`):

- `keep` defaults to 14. `enabled` defaults to true; `enabled=false` or `0`
  disables the schedule.
- Readiness: `target=s3` requires endpoint+bucket+access_key+secret_key;
  `target=dir` requires an absolute `path`. Nothing runs until a target is
  registered and ready.
- **When**: every day at 03:00 local time the server's cleanup worker spawns
  `fly_board --scheduled-backup` as a separate process (an flock on
  `data/.backup.lock` prevents overlapping runs). The archive is staged in
  `data/.backup-out`, shipped to the target, pruned to the newest `keep`
  archives **made by this site**, and the local copy removed. Outcome, size
  and error (if any) are recorded in the `site_settings` table and shown on
  the admin dashboard.
- With `passphrase` set, each scheduled archive is written `--with-secrets`;
  with it empty, archives carry no secrets.

## Restore procedure

Full-site recovery onto a fresh machine:

```sh
# 1. Install fly.board (docs/install.md) but do not start it.

# 2. Bring the archive in and restore:
./fly_board --restore /path/to/site.fbk
#    add --passphrase-file /root/backup.pass if the archive has sealed secrets
#    add --force only when restoring over an existing site

# 3. Start:
systemctl start flyboard
```

Notes:

- Restore works without the signing seed; the first startup mints a new one
  (which is why `--restore` runs before crypto init in `main()`).
- If the archive carries secrets, give the passphrase — otherwise restore
  proceeds without them and the site gets a new signing key.
- After restoring a **replica**, see [flywire.md](flywire.md) for resuming
  or reseeding sync.

## Related maintenance

- Abandoned TASFA transfer sessions are swept at startup and daily at 03:00;
  `./fly_board --sweep-uploads --dry-run` previews what would be removed.
- Unverified accounts older than 24 h and orphaned files are also cleaned in
  the same daily job.
