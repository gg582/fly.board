#!/usr/bin/env bash
# Backup recovery drill: prove the newest archive (or a given one) actually
# restores — not just that it exists. A backup that has never been restored
# is a hypothesis; this script turns it into a checked fact, without touching
# the live site:
#
#   1. fly_board --verify   manifest signature + every blob hash
#   2. fly_board --restore  into a throwaway directory (never the site root)
#   3. sanity checks on the restored data/blog.db: quick_check, core tables,
#      row counts, one FTS5 query, and settings readability
#
# Usage:
#   tools/backup_drill.sh [--archive PATH] [--keep-temp] [--fly-board PATH]
#
# Archive selection: --archive, else $BACKUP_DRILL_ARCHIVE, else the newest
# *.fbk under the dir-target path from backup.settings (target=dir), else
# under ./backups. Passphrase: $FLY_BACKUP_PASSPHRASE or --passphrase-file
# via $BACKUP_DRILL_PASSPHRASE_FILE is forwarded to --verify/--restore.
#
# Cron/systemd example (weekly, Sunday 04:30 local):
#   30 4 * * 0 /path/to/fly.board/tools/backup_drill.sh >> /var/log/flyboard-drill.log 2>&1
# Exit status is 0 only when every stage passes.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FLY_BOARD="$ROOT/fly_board"
ARCHIVE=""
KEEP_TEMP=0
LOG_LINE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --archive) ARCHIVE="${2:-}"; shift 2 ;;
        --fly-board) FLY_BOARD="${2:-}"; shift 2 ;;
        --keep-temp) KEEP_TEMP=1; shift ;;
        -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
        *) echo "backup_drill: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

fail() { echo "DRILL FAIL: $*" >&2; exit 1; }

[ -x "$FLY_BOARD" ] || fail "fly_board binary not found at $FLY_BOARD (build it first)"

# ---- Pick the archive ------------------------------------------------------
if [ -z "$ARCHIVE" ]; then
    ARCHIVE="${BACKUP_DRILL_ARCHIVE:-}"
fi
if [ -z "$ARCHIVE" ]; then
    DIR=""
    if [ -f "$ROOT/backup.settings" ]; then
        DIR="$(awk -F= '$1=="path" && $2 ~ /^\// {print $2}' "$ROOT/backup.settings" | head -n1)"
        TGT="$(awk -F= '$1=="target" {print $2}' "$ROOT/backup.settings" | head -n1)"
        [ "${TGT:-dir}" = "dir" ] || DIR=""   # S3 targets keep no local copy
    fi
    [ -n "$DIR" ] || DIR="$ROOT/backups"
    if [ -d "$DIR" ]; then
        ARCHIVE="$(find "$DIR" -maxdepth 1 -name '*.fbk' -type f -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -n1 | cut -d' ' -f2-)"
    fi
fi
[ -n "$ARCHIVE" ] && [ -f "$ARCHIVE" ] || fail "no archive found (pass --archive or set BACKUP_DRILL_ARCHIVE)"
echo "drill: archive $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"

PASS_ARGS=()
[ -n "${BACKUP_DRILL_PASSPHRASE_FILE:-}" ] && PASS_ARGS+=(--passphrase-file "$BACKUP_DRILL_PASSPHRASE_FILE")

# ---- Stage 1: cryptographic verify ----------------------------------------
echo "drill: stage 1/3 manifest + blob verify"
OUT1="$(cd "$ROOT" && "$FLY_BOARD" --verify "$ARCHIVE" "${PASS_ARGS[@]}" 2>&1)" \
    || { echo "$OUT1" >&2; fail "stage 1 (--verify) failed"; }
echo "$OUT1" | tail -n 3

# ---- Stage 2: restore into a throwaway site root ----------------------------
TMP="$(mktemp -d /tmp/flyboard-drill.XXXXXX)"
trap '[ "$KEEP_TEMP" = "1" ] || rm -rf "$TMP"' EXIT
echo "drill: stage 2/3 restore into $TMP"
# fly_board resolves site files relative to its own directory at startup, so
# the throwaway root needs its own copy of the binary -- exactly like the
# "fresh install, then --restore" procedure in docs/backup.md. The read-only
# asset trees are linked in (same trick as tools/smoke_test.sh); only data/
# is restored content.
cp "$FLY_BOARD" "$TMP/fly_board"
for d in public posts legal img; do
    [ -e "$ROOT/$d" ] && ln -s "$ROOT/$d" "$TMP/$d"
done
OUT2="$(cd "$TMP" && ./fly_board --restore "$ARCHIVE" "${PASS_ARGS[@]}" 2>&1)" \
    || { echo "$OUT2" >&2; fail "stage 2 (--restore) failed"; }
echo "$OUT2" | tail -n 3
[ -f "$TMP/data/blog.db" ] || fail "restored data/blog.db missing"

# ---- Stage 3: the restored database must be a working blog ------------------
echo "drill: stage 3/3 restored DB sanity checks"
python3 - "$TMP/data/blog.db" <<'EOF' || fail "stage 3 (sanity checks) failed"
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
def one(sql):
    return conn.execute(sql).fetchone()[0]
problems = []
notes = []
def tokenizer_skippable(exc):
    return "no such tokenizer" in str(exc)  # host sqlite lacks mecab; the server binary has it
try:
    if one("PRAGMA quick_check(1)") != "ok":
        problems.append("quick_check")
except Exception as e:
    if tokenizer_skippable(e):
        notes.append("quick_check skipped (host sqlite lacks the mecab tokenizer)")
    else:
        problems.append(f"quick_check error: {e}")
for t in ("users", "posts", "boards", "comments"):
    try:
        one(f"SELECT COUNT(*) FROM {t}")
    except Exception:
        problems.append(f"table {t} missing/unreadable")
users = one("SELECT COUNT(*) FROM users")
posts = one("SELECT COUNT(*) FROM posts")
if users < 1:
    problems.append("no users in restored DB")
try:
    conn.execute("SELECT rowid FROM posts_fts WHERE posts_fts MATCH '\"the\"' LIMIT 1").fetchall()
except Exception as e:
    if tokenizer_skippable(e):
        notes.append("FTS5 query skipped (host sqlite lacks the mecab tokenizer)")
    else:
        problems.append(f"FTS5 unreadable: {e}")
try:
    one("SELECT value FROM site_settings LIMIT 1")
except Exception:
    problems.append("site_settings unreadable")
print(f"  counts ok; users={users} posts={posts}")
for n in notes:
    print("  note: " + n)
if problems:
    print("  PROBLEMS: " + "; ".join(problems), file=sys.stderr)
    sys.exit(1)
EOF

echo "DRILL OK: $ARCHIVE restores and passes sanity checks ($TMP)"
echo "$(date -Is) OK $ARCHIVE" >> "${BACKUP_DRILL_LOG:-$ROOT/backup-drill.log}"
