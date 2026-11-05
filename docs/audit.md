# Optional audit log

fly.board can record selected security-sensitive reads and administrative
mutations in a local, append-only JSON Lines file. Audit storage is **disabled
by default**. It is independent of the content databases and FlyWire journal.

## Enable and configure

Create `audit.settings` in the site root and restart fly.board:

```
enabled=true
path=data/audit.log
sign=true
```

| Key | Default | Effect |
|---|---|---|
| `enabled` | `false` | Enables audit storage only when `true`, `1`, or `yes` |
| `path` | `data/audit.log` | JSONL destination, relative to the site root unless absolute |
| `sign` | `false` | Signs every entry hash with the site's ML-DSA-65 key |

The file is created with mode 0600. Keep its parent directory restricted to
the service account. Enabling `sign` uses the existing
`data/.pqc_mldsa65_seed`; it does not create a second audit key. Audit and post
signatures use separate signing contexts and cannot be substituted for one
another.

The initial instrumentation covers:

- administrator role and account-active changes;
- write-scope and upload-permission policy changes;
- automatic IP bans created after repeated login failures;
- moderation approval/rejection and report resolution/dismissal/deletion;
- successful direct reads of secret posts, including anonymous reads.

Entries describe successful mutations (or a newly created automatic ban), not
every rejected request. Details contain identifiers and fixed outcome values,
not post bodies, credentials, IP addresses, or secret-post contents.

## Entry format and integrity

Each line is one JSON object with `version`, `seq`, `timestamp`, `actor_uid`,
`action`, `target`, `outcome`, `detail`, `previous_hash`, and `hash`. The first
`previous_hash` is empty. `hash` is SHA-256 over the unformatted JSON payload
without `hash` or `signature`. When signing is enabled, `signature` contains
the ML-DSA key id and signature over the hexadecimal entry hash.

Appending takes an exclusive file lock, re-verifies the existing chain, writes
one complete line with `O_APPEND`, and calls `fsync`. This serializes threads
and worker processes. An append failure is logged and does not roll back the
already-completed administrative action. Startup with auditing enabled
verifies the entire existing file and refuses to start if it is malformed,
reordered, changed, truncated, or has an invalid signature.

This is tamper evidence, not operating-system immutability or a write-once
store. A privileged attacker can replace the whole file or remove a valid
suffix. Keep the signing seed separate, restrict file access, and export the
log and verified head hash to an independent system when that threat matters.

## Verify and export

With the site stopped, run:

```
./fly_board --audit-verify
./fly_board --audit-verify /path/to/exported-audit.log
```

A valid file prints `audit log OK: N entries` and exits 0. A missing file is an
empty valid log. The standalone command and startup verification load the
current site seed. Keep that seed available for every signed chain you need to
verify; rotating to a new key requires retaining the old chain and its old
verification environment separately.

Export by copying the JSONL file while the service is stopped, then verify the
copy. There is no public HTTP export endpoint. Copy the verified final `hash`
separately if suffix-deletion detection is required.

## Retention and recovery

There is no automatic purge, rotation, or size-based rewriting. Retention is
an operator policy: export first, verify the export, and only then replace the
active file while fly.board is stopped. Starting a new file resets `seq` to 1
and creates a new chain. Never truncate or edit a live log. A verification
failure requires restoring a known-good exported chain or explicitly starting
a new one; fly.board does not silently repair or re-sign altered entries.

The audit file is not included implicitly in content database backups. Include
it in the site's file-backup/export procedure separately.

## FlyWire behavior

Audit entries are deliberately **not replicated** through `/flywire/feed` and
are not added to `sync_journal`. They belong to the site and signing-key domain
that observed the action. A write-through proxy request is audited once, on
the primary that performs the mutation; the replica does not append a second
entry for the proxy hop. Replicas therefore do not hold a complete audit copy.
Export logs independently from every node on which auditing is enabled.

During failover, a new primary starts appending to its own local chain. It does
not merge or rewrite another node's chain, and entries that existed only on a
failed primary are not reconstructed from content replication. Operators who
need a continuous cross-site record must export each node's chain and preserve
its node identity externally.
