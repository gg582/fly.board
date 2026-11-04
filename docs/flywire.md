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
| `primary_url` | - | Base URL of the primary for a replica, e.g. `https://example.com` (no trailing slash needed) |
| `token` | - | Shared secret. Replicas send it as the `X-FlyWire-Token` header; the primary refuses the feed without a matching token. Primary mode is only active when the token is non-empty |
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
hostname. If the primary is unreachable the request fails with 503 - logins
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

## Drift monitoring

The feed advertises the primary's newest journal seq (`latest_seq`), and
each replica compares it against its applied checkpoint after every poll.
When the gap reaches `FLYWIRE_LAG_ALERT_ROWS` (default 200) the replica
logs a WARN - a replica that drifts silently serves stale content, which
no error path used to reveal.

For a one-shot operator view run:

```
./fly_board --flywire-status
```

It prints the mode, site version, replica checkpoint seq and the lag to
the primary's newest seq (exit 1 when the primary cannot be queried).

## Failover cluster

With three or more sites, FlyWire can move the primary role automatically.
Add the same `nodes=` list and token to every site's `flywire.settings`, and
name each site with `self=`:

```
token=<shared secret>
nodes=80.47.226.102,173.231.53.9,59.23.169.247
self=173.231.53.9
site_host=oborona.zip
failover=priority
failover_seconds=20
```

- `nodes` - every site, in failover priority order. Entries are IPs/hosts
  (requests go to `https://<site_host>` routed to that address, so the TLS
  certificate and Host header stay the public ones and no `/etc/hosts` pin
  is needed) or full base URLs (`http://127.0.0.1:18601`, for tests).
- `self` - this site's `nodes` entry. A site behind NAT names its public
  address here (it is how peers reach it).
- `failover=priority` - the first live site in `nodes` order takes over, and
  a higher-priority site takes the role back once it has caught up.
  `failover=auto` - the live site holding the most of the log takes over
  (ties: lowest mean RTT to its peers, then `nodes` order); no fail-back.
- `failover_seconds` - how long the primary must be silent (default 20).
- `mode`/`primary_url` are ignored once `nodes`/`self` are set; on first
  start the first `nodes` entry is primary.

Every site serves `GET /flywire/status` (token) and polls its peers every
`poll_seconds`:

- **Takeover.** When the primary has been silent for `failover_seconds` and
  a majority of the sites agree (this one plus peers that see the same
  outage and name the same candidate), the candidate promotes itself and
  bumps the cluster epoch. Peers adopt the highest epoch they see and route
  feed polls, file fetches and proxied writes to the new primary.
- **Fencing.** A primary that cannot reach a majority for
  `failover_seconds` answers writes with 503 until it can, so a
  partitioned primary and a newly elected one never both take writes. A
  primary also starts fenced after a restart until its first round confirms
  a majority and no newer epoch. With two sites a majority needs both, so
  failover needs at least three.
- **Fail-back** (`priority`). A returning higher-priority site rejoins as a
  replica, catches up, and the current primary then fences itself briefly
  and hands the role over.
- **One log.** Replicas mirror the primary's journal rows under the same
  seq and journal nothing of their own, so any site can serve the feed after
  a takeover and followers resume from their checkpoint. Rows the old
  primary wrote but never replicated (at most the last poll interval) are
  dropped from its journal when it rejoins, with a WARN naming the seqs;
  their table changes stay only on that site.

## Optional anti-entropy union reconciliation

Set `FLYWIRE_ANTI_ENTROPY=1` in the environment of every failover-cluster
node. The feature is off by default and its built-in
`GET /flywire/anti-entropy` route uses the same `X-FlyWire-Token`
authentication as the status endpoint. Each cluster tick exchanges one page with one peer; completed
scans restart from the beginning so randomly identified new mutations are
found regardless of their sort position.

This is a second, independent channel. Ordinary `/flywire/feed` replication,
its sequence checkpoints and gap handling, primary election, epochs, and write
fencing are unchanged. Anti-entropy does not advance `.flywire_seq`, append
ordinary `sync_journal` rows, accept browser writes, or make a fenced node a
writer. It can, however, recover a mutation that the ordinary failover log
would otherwise have dropped; operators enabling it explicitly choose union
retention over discarding that divergence.

Every ordinary primary mutation also gets a random stable `mutation_id`, a
`changed_at` millisecond timestamp, and a SHA-256 hash over entity, numeric id,
operation, timestamp, and payload in `sync_mutation_log`. Sequential mutations
recorded on one node receive strictly increasing `changed_at` values even when
they occur in the same clock millisecond. Existing journal rows are backfilled
with deterministic `legacy-<seq>` identities and use `seq` as their legacy
ordering value when the log is initialized. Peers insert only missing ids and
reject rows whose hash does not match. Deletes are explicit tombstone rows.
For each `(entity, entity_id)`, all peers apply the same total ordering:
descending `changed_at`, then delete over upsert, descending payload hash, and
descending mutation id. Thus sequential latest mutations win, an older upsert
cannot overwrite a newer one, and a delete beats only mutations at the same or
older ordering. A newer upsert after deletion is allowed. Once all
participating nodes have exchanged the same retained mutation set, supported
table rows converge to the same chosen mutation.

Convergence has important limits:

- `changed_at` uses each writer's wall clock. Clock skew can make an edit that
  happened earlier in real time win over a later edit on another node. This is
  timestamp ordering with deterministic tie-breaks, not causal ordering or a
  substitute for synchronized clocks. Equal-timestamp concurrent upserts are
  selected by hash/id, not by arrival order.
- The mutation log is not covered by the seven-day `sync_journal` purge and
  currently grows until an operator implements a safe, coordinated retention
  policy. Purging it independently breaks these guarantees.
- Full retained-log scans are required because mutation ids are random; this
  is intended for optional repair, not high-throughput replication.
- Unknown entities, incompatible schemas, local-admin protections, failed SQL
  applies, file bytes, counters, and files outside the journaled model may not
  converge. File metadata convergence still requires the normal byte-fetch
  mechanism. Hashes provide integrity against accidental mismatch, not
  authentication beyond possession of the shared FlyWire token.

State is kept in `data/.flywire_cluster` (epoch, primary, base seq).
Sessions survive a takeover only if all sites share `data/.jwt_secret`;
copy the primary's file to the others before enabling failover. DNS is out
of scope: a dead site's address stays in round-robin DNS until removed.
