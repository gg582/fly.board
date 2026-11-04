# Database portability

The application is implemented against SQLite 3. The default main database is
unchanged: `data/blog.db`. Comments and board-tree data remain separate local
SQLite databases (`data/comments.db` and `data/board_tree.db`).

## Selecting another SQLite location

Set `FLY_DB_LOCATION` to an absolute/relative SQLite path or a SQLite `file:`
URI. Set `FLY_DB_BACKEND=sqlite` explicitly if desired. Any other backend name
is rejected at startup rather than being treated as a working server driver.

```sh
FLY_DB_BACKEND=sqlite FLY_DB_LOCATION=/srv/flyboard/blog.db ./fly_board
FLY_DB_LOCATION='file:/srv/flyboard/blog.db?mode=rwc' ./fly_board
```

A path on a network filesystem is only as safe as that filesystem's SQLite
locking and WAL support. SQLite WAL generally requires shared-memory support
on one host; do not put a WAL database on NFS or another network filesystem
that does not provide correct POSIX locks. A SQLite URI is not a PostgreSQL,
MySQL, or network-database connection string.

Backup, restore, auxiliary databases, and operational tools may still use
their documented paths. Verify those workflows before relocating a production
database.

## Standard-SQL adapter contract (not a runtime driver)

There is no PostgreSQL or MySQL runtime support. A future server adapter must
be wired end-to-end through connection creation, per-thread/fork handling,
schema migration, FlyWire journal and anti-entropy queries, backup/restore,
and tests before it can claim support. At minimum its contract must provide:

- transactional execution and prepared statements with typed bind parameters;
- deterministic `INSERT ... ON CONFLICT` behavior and unique-key enforcement;
- an equivalent of stable integer primary keys and explicit tombstone storage;
- connection-local foreign-key enforcement and an explicit busy/retry policy;
- migrations for SQLite-specific `PRAGMA`, `sqlite_sequence`, `AUTOINCREMENT`,
  `datetime('now')`, FTS5 trigram search, and WAL checkpoint operations;
- crash-safe, mutually consistent backup and restore across every database;
- integration tests against the actual server, including concurrent writers.

The current `src/db/db_portability.c` interface intentionally opens SQLite
only; the contract above documents the remaining work, not an abstraction that
already exists at runtime.
