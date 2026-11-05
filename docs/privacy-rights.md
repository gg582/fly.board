# Personal data export and account ghosting

fly.board provides self-service privacy controls under **Account Settings**.
They are enabled by default and require an authenticated, active account.

## Personal data export

**Create personal data export** queues a background job. The settings page
polls job status and shows a download link when the archive is ready. The
HTTP endpoints are:

- `POST /account/export`
- `GET /account/export/<id>/status`
- `GET /account/export/<id>/download`

The download is a streaming-created `tar.zst` archive containing:

- `profile.json`: profile and account metadata, but no password hash, TOTP
  secret/recovery codes, email tokens, or other credentials;
- `posts/*.json`: all of the account's posts, including drafts, scheduled and
  secret posts, but no deletion PIN hashes;
- `comments/*.json`: the account's comments with their thread relationships;
- `uploads/*.json` and `uploads/<id>-<filename>`: metadata and locally stored
  bytes for uploads owned by the account.

Every query is scoped to the authenticated user id. A job id is random, but it
is not a capability: status and download also require a session belonging to
the recorded owner. Other accounts, logged-out clients, expired jobs, and
ghosted accounts receive no archive data. Responses use `private, no-store`.
Downloads accept single HTTP byte ranges (`Range` / 206) so interrupted clients
can resume.

Archives and state files are mode `0600` below a mode `0700` export directory.
The default lifetime is 15 minutes. Expired files are removed when their job
is accessed. Deleting the account revokes all of its jobs. Operators should
also remove old export directories during routine maintenance if the service
has not been running; queued/running jobs interrupted by a restart are not
resumed, although completed unexpired downloads remain usable.

| Environment variable | Default | Effect |
|---|---|---|
| `FLY_EXPORT_DIR` | `data/exports` | Export state/archive directory |
| `FLY_EXPORT_TTL_SECONDS` | `900` | Lifetime, clamped to 60–3600 seconds |

Uploads stored only in S3 offload mode cannot be embedded by the local archive
worker. Their owned metadata is still exported; keep the local mirror enabled
when complete byte-for-byte self-service export is required. Extract archives
with a Zstd-compatible tar implementation, for example `tar --zstd -xf
flyboard-personal-data.tar.zst`.

## Account deletion and ghosting

Deletion requires the current password, the literal confirmation `DELETE`,
and a current authenticator/recovery code when TOTP is enabled. The
`admin.settings` site administrator cannot use this endpoint.

A successful `POST /account/delete` converts the existing users row into a
non-login ghost instead of deleting its numeric id. This preserves posts,
comments, and comment parent/child structure under **Deleted user**. The
original username, email address, nickname, bio, profile picture, password,
TOTP secrets/recovery codes, email verification tokens, webmail,
notifications, votes, board permissions, guestbook entries, deletion PIN
hashes, and owned upload records are removed. Locally stored upload and
profile-image bytes are unlinked, and all exports are revoked.

All JWTs are checked against the current user row on every request. Setting
`active=0` and `ghosted=1` therefore invalidates existing sessions immediately;
clients also receive cleared session cookies after deletion. Other users'
content and private rows are not changed.

Ghosting updates the main and comments databases in coordinated transactions.
As with any service that stores data in multiple databases, take consistent
backups before maintenance and test restoration. Object bytes in S3 offload
mode are not deleted by this workflow, but their database records and download
routes are removed. Operators with an erasure requirement for offloaded object
bytes must also apply their storage retention/deletion procedure.
