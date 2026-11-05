# Board/year SQLite archive partitioning: architecture and operator plan (PoC)

Status: **design and isolated proof of concept only**. This document does not
describe an enabled fly.board feature. The accompanying test creates its own
throwaway tables and databases; it does not open `data/blog.db`, change a live
schema, or connect a production handler.

## Goals and boundaries

The proposed design keeps one write-active SQLite database and adds immutable,
read-only archives keyed by `(board_id, post_year)`. `post_year` is derived once
from the post's stored creation timestamp in UTC and never changes. Archives use
ordinary SQLite database files and ordinary SQL. Readers mount them with
`ATTACH DATABASE`; no custom VFS, database server, SQL dialect, or file format is
required.

The design must provide:

- one authoritative owner for every `(board_id, post_year)` at each routing
  generation;
- exactly-once reads across active and archive partitions without using `UNION`
  to conceal duplicate ownership;
- no writes to an archived range, including edits, deletes, votes, comments, or
  maintenance jobs that rewrite posts;
- deterministic pagination and search across partitions;
- explicit FlyWire, anti-entropy, failover, backup, and restore behavior; and
- rollback by selecting the previous routing generation while the active
  database still retains its pre-archive rows.

Non-goals for the PoC are automatic archival, online production migration,
cross-database foreign keys, transparent writes to archives, and increasing the
number of simultaneous SQLite writers. The active database remains the only
writer. Existing comment, board-tree, uploaded-file, secret, and auxiliary
database layouts are unchanged.

## Files, ownership, and routing manifest

A future implementation would name archives with an opaque, validated identifier
such as `board-7/year-2023.sqlite`, stored outside the public tree. Each archive
contains the same compatible post and archive-local FTS schema version, plus
archive metadata identifying its board, year, schema, content hash, and manifest
generation. File names are labels, not routing authority.

The active database would hold a transactional routing manifest with:

- a monotonically increasing `generation`;
- exact, non-overlapping `(board_id, post_year)` ranges;
- archive identifier, expected schema version, byte size, and SHA-256 hash;
- state (`staging` or `published`); and
- the active/archive owner selected for each range.

Only a `published` manifest is eligible for serving. A process loads one
manifest generation when it opens or refreshes its connections and uses that
same generation for the whole request. Refresh occurs only between requests.
All connections involved in that request mount the same archive set. A reader
must fail closed if an archive is missing, has the wrong identity/schema/hash,
or the manifest has duplicate or overlapping ownership; it must not silently
fall back to a partial active-only result.

Archive files are opened with a SQLite `file:` URI using `mode=ro` (and
preferably `immutable=1` only after all writers and WAL sidecars are gone). The
connection also enables `PRAGMA query_only=ON` after any active-database work is
complete. Archive aliases and SQL fragments come from a fixed internal
allowlist, never from request text; values remain bound parameters.

### Deterministic reads

1. A request naming both board and year routes to exactly the owning partition
   in its loaded manifest.
2. A board-only or site-wide timeline uses `UNION ALL` over every owner required
   by that manifest. The active branch explicitly excludes ranges owned by an
   archive. `UNION ALL` is valid only because ownership was validated; using
   `UNION`/`DISTINCT` as a substitute is forbidden because it hides manifest
   bugs.
3. Every result carries its stable post id. Sorting includes a unique final tie
   breaker, for example `created_at DESC, id DESC`. Keyset pagination uses the
   complete sort tuple. Offset pagination is not stable while new active rows
   arrive.
4. A post id lookup may probe partitions, but success requires exactly one row
   across the mounted owners. Zero rows is not found; more than one is a
   corruption error.
5. Related data remains active-only unless a future, separately reviewed design
   archives it. Joins must not assume SQLite enforces foreign keys across
   attached databases.

An active write commits before a response is generated, so the active branch
provides normal read-your-write behavior. Archives are immutable and therefore
cannot become stale through subsequent edits: requests for an archived range
are read-only and mutation attempts are rejected before SQL execution.

SQLite's compiled `SQLITE_MAX_ATTACHED` limit (commonly 10) bounds archives per
connection. The implementation must query/enforce its actual limit and use
bounded fan-out or sequential scans for larger manifests. Per-connection attach
setup, file descriptors, query planning, and latency at the intended board/year
counts require benchmarks before any production proposal.

## FTS semantics

Each archive builds and validates its own `posts_fts` index while still offline
and writable. The active database keeps its existing active index. Both sides
must record the same tokenizer, tokenizer build, normalization rules, and search
index version. In particular, a MeCab archive cannot be searched with different
morpheme rules and assumed equivalent.

A search fans out the same parsed query to each required index, confirms matches
against the corresponding base table, and merges complete scored results in the
application. Ordering uses the documented score followed by post id. Applying
`LIMIT` independently to partitions and then merging is incorrect: the merged
sort and limit must happen after all candidates are available, or a proven
top-k merge must retain enough candidates from every partition. Deletes and
updates to archived posts are unsupported; correcting an archive requires a new
archive generation and a reversible manifest switch. The existing active-only
LIKE fallback must not silently omit archive matches when archive FTS is
unavailable.

## Migration and reversible rollout

This is an operator plan for a future implementation, not instructions to run
against the current server. A maintenance window and a single migration process
are required. No HTTP worker may serve a staging manifest.

1. **Prepare and protect.** Stop writes, stop FlyWire apply/election activity,
   checkpoint and then close WAL cleanly, and use rollback-journal mode for the
   offline multi-database operation. Back up active, comments, board-tree, and
   auxiliary state. Keep the original active file unchanged as a recovery copy.
2. **Build a staging archive.** Copy the selected board/year rows and build FTS
   in a new file. Never rename a partially copied file into the archive store.
   Verify schemas, row ids/counts, per-board/year counts, content hashes, and
   `PRAGMA integrity_check` on every file.
3. **Keep a rollback shadow.** Initially leave the copied rows in active. The
   current active-only generation continues to read them. The staging archive
   is not mounted by serving connections, so duplication is impossible.
4. **Publish atomically.** Under the write fence, compare the active post
   versions with the staged copy again, then commit a new manifest generation
   selecting the archive and excluding that range from active reads. Restart or
   refresh workers between requests. Confirm all workers report the same
   generation before resuming reads.
5. **Validate.** Compare global and per-partition id sets, counts, ordering,
   keyset pagination, direct reads, FTS results, and rejection of archived
   writes. Confirm archive files are read-only and active writes still work.
6. **Roll back when needed.** Fence writes, publish the previous active-only
   generation, and refresh all workers. Because active retained its shadow rows,
   no data copy is needed and no archive write has occurred. Remove the bad
   archive only after the previous generation is fully restored.
7. **Clean up only after the rollback window.** Deleting active shadow rows is a
   separate, explicitly irreversible maintenance step. It requires a fresh
   verified backup and acknowledgement that rollback now means restoring the
   pre-cleanup active snapshot and its matching manifest. Never clean up while
   any worker can use an archive-owning generation or FlyWire still needs the
   shadow state.

A crash before manifest publication leaves the old active-only generation. A
crash after publication leaves the complete archive plus active shadow rows.
This avoids relying on atomic commit across multiple independent database files.
The isolated C PoC additionally demonstrates SQLite save/rollback behavior for
an offline attached-database copy; that transaction is not the proposed online
cutover protocol.

## Backup and restore

A partitioned backup is one **recovery set**, not a collection of independently
restorable files. Its manifest records the routing generation, active FlyWire
checkpoint, archive identifiers/hashes, schema versions, and every existing
auxiliary database/file set. With writes fenced, create consistent snapshots
using the SQLite backup API (or a fully stopped file copy), including active and
every referenced archive. Resume writes only after the recovery-set metadata is
complete. Existing `.fbk` tooling does **not** implement this yet and must not be
represented as partition-aware.

Restore the complete set to a new location, verify hashes and integrity checks,
then start with the recorded generation. Never restore active alone while
retaining archives from another recovery set. Drill both directions: restore an
archive-owning generation and restore the preceding active-only generation from
the rollback window. Retain staging and obsolete archive files until no backup
manifest references them.

## FlyWire and anti-entropy

Ordinary post mutations continue to journal only on the active primary. Archive
creation is not a stream of per-row browser mutations. Instead, the primary
ships a verified archive artifact and its recovery-set/generation metadata out
of band, then journals a small generation-publication event at a normal FlyWire
sequence. A replica must fetch and verify every referenced archive before
applying that event. It acknowledges the new generation only after its complete
manifest is mounted. Until then it remains on the previous active-only
shadow-row generation; it must not acknowledge the sequence and then serve a
partial set.

Journal retention must cover the time needed to transfer archives. A replica
that misses the publication event or cannot validate its artifact reseeds from
a complete recovery set. Existing FlyWire does not yet have these event or
artifact mechanisms.

Anti-entropy compares immutable archive id, generation, schema, and content hash;
it does not merge individual archived mutations or run union-winner selection
inside an archive. A mismatch quarantines that archive and blocks the dependent
generation. Repair replaces the whole artifact from an authenticated verified
copy. Active rows and mutation logs retain their existing anti-entropy rules.
Independent deletion of active shadow rows, archived mutation metadata, or
recovery-set members can destroy rollback/convergence and is forbidden.

## Failover

Every failover candidate must advertise its routing generation and archive
hashes in addition to its FlyWire checkpoint/epoch. A candidate is eligible only
when it has the complete artifact set for the generation it would serve. Peers
adopt a generation only through the normal authenticated FlyWire ordering and
fencing rules. Promotion never makes an archive writable.

On failover, all new-primary workers mount the same verified generation before
accepting writes. Archived ranges stay read-only; new posts go to active. A
returning node with an unknown manifest or missing archive remains fenced and
reseeds rather than reconstructing ownership from local files. Split-brain
protection remains FlyWire's majority/epoch mechanism; partitioning adds no
independent leader or commit protocol.

## Production gates

A production design review would require, at minimum:

- integration with real post/search/backup/FlyWire code and versioned schema
  migrations, reviewed separately from this PoC;
- concurrent writer/reader, crash, rollback, restore, replica-lag, failover, and
  corrupted/missing archive tests;
- multi-process manifest refresh proof and observability for generation,
  ownership failures, archive lag, and fallback events;
- archive-count/resource benchmarks and an enforced attach/fan-out limit; and
- an operator drill demonstrating both rollout rollback and complete recovery
  set restoration.

Until those gates are met, the safe default remains the existing monolithic
active database.

## Isolated proof of concept

`tests/test_sqlite_partition_poc.c` uses only the system SQLite C API. It
creates temporary active and archive databases, attaches the archive read-only,
checks exact id-set reads and deterministic board/year routing, rejects an
archive write, and verifies both rollback and commit of an offline migration.
Run it with:

```sh
make check-sqlite-partition-poc
```

The target compiles into a temporary directory and removes its binary and test
databases on exit. Passing this test demonstrates SQL mechanics only; it is not
evidence that production routing, FTS, FlyWire, failover, or backup integration
is implemented.
