# FlyWire: primary/replica site synchronization

FlyWire keeps a read-only replica of a fly.board site in sync with its
primary. The primary journals every content mutation into a `sync_journal`
table (`src/db/db_sync.c`) and serves it as a delta feed; replicas poll the
feed and apply rows to their own databases. Implemented in
`src/engine/flywire.c`, configured in `flywire.settings`.

## Configuration (flywire.settings)

| Key | Default | Effect |
|---|---|---|
| `mode` | *(empty = off)* | `primary`: serve `/flywire/feed`, journal mutations. `replica`: poll the primary, apply deltas locally. Unknown values disable FlyWire with a warning |
| `primary_url` | — | Base URL of the primary for a replica, e.g. `https://example.com` (no trailing slash needed) |
| `token` | — | Shared secret. Replicas send it as the `X-FlyWire-Token` header; the primary refuses the feed without a matching token. Primary mode is only active when the token is non-empty |
| `poll_seconds` | `2` | Replica poll interval, clamped to 1–60 |
| `auto_upgrade` | *(off)* | `1`: when the primary advertises a different version, the replica spawns `./flywire-upgrade.sh` (git fetch/reset to origin/main, preserving instance settings, rebuild, `systemctl restart`). The script must exist and be executable; `FLYWIRE_SERVICE` selects the unit name (default `flyboard`) |

## How it works

**Primary.** Every content mutation (posts, boards, comments, files, users,
votes, …) appends a row to `sync_journal` with a monotonically increasing
`seq`. `GET /flywire/feed?since=<seq>&limit=<n>` returns the rows after
`<seq>` as JSON, plus the site's version string (read from
`data/.flywire_version`, falling back to the git HEAD). The endpoint requires
the `X-FlyWire-Token` header and is only registered when the token is set.

**Replica.** A supervisor thread polls `primary_url/flywire/feed?since=<seq>`
every `poll_seconds`, where `<seq>` is the local checkpoint
(`data/.flywire_seq`, written after every applied batch). Each row is applied
as an upsert or delete against the local SQLite databases. Missing file
payloads are re-queued for a later pass. The feed page also carries the
primary's version; a mismatch triggers `flywire-upgrade.sh` when
`auto_upgrade=1`.

**Journal retention.** `sync_journal` rows older than 7 days are purged at
startup and hourly. A replica that falls more than 7 days behind can no
longer catch up incrementally.

## Setting up a replica

1. On the primary: set `mode=primary` and a strong `token` in
   `flywire.settings`, restart.
2. Create a backup archive on the primary (see [backup.md](backup.md)):
   ```
   ./fly_board --backup /path/to/site.fbk --with-secrets
   ```
3. On the replica host: install fly.board (see [install.md](install.md)),
   copy the archive over, and seed the database from it:
   ```
   ./fly_board --restore /path/to/site.fbk
   ```
   Restore writes the primary's checkpoint into `data/.flywire_seq`, so the
   replica resumes exactly where the archive was taken.
4. On the replica: set `mode=replica`, `primary_url=https://<primary>` and
   the same `token` in `flywire.settings`, restart. The apply loop starts
   polling and the site serves the replicated content read-only.

## Writes on a replica (write-through proxy)

A replica is read-only locally: every non-GET/HEAD request that is not under
`/flywire/` is forwarded to the primary by `flywire_proxy_write()`
(`src/handlers/handlers.c`), including Cookie/Authorization credentials, and
the primary's status, `Location`, `Set-Cookie` and `Content-Type` are relayed
back. This makes the replica behave like the primary behind a shared
hostname. If the primary is unreachable the request fails with 503 — logins
and logouts on a replica therefore require the primary to be up.

## Reseeding after `journal_purged`

If the replica logs that the journal was purged past its checkpoint
(`flywire: the primary no longer has journal rows for our checkpoint`), the
replica stops applying and waits. Recovery is a manual reseed:

1. Take a fresh archive on the primary:
   `./fly_board --backup /path/to/site.fbk --with-secrets`
2. On the replica, with the service stopped:
   `./fly_board --restore /path/to/site.fbk --force`
3. Restart the replica. The restored `data/.flywire_seq` points at the new
   checkpoint and incremental sync resumes.

The replica never auto-restores: a feed error (including `journal_purged`)
always requires an operator decision.

## Related endpoints

- `POST /flywire/request-admin` (replica): file a request for admin rights;
  delivered to the primary's site admin.
- `POST /flywire/promote-request` (primary): approve/deny such requests.

## Version sync notes

- The advertised version comes from `data/.flywire_version` if present,
  otherwise from git. Writing that file is how a deployment pins a version
  string that differs from the git HEAD.
- `flywire-upgrade.sh` preserves tracked instance files
  (`blog.settings`, `admin.settings`, `fonts.settings`, `robots.settings`,
  `backup.settings`) across `git reset --hard`, rebuilds with
  `make -j2 fly_board mail-tools`, and restarts the service. Untracked state
  (`data/`, `public/uploads/`, secrets, certs) is never touched by git.
