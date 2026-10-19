#!/usr/bin/env python3
"""Link orphaned files (post_id NULL/0) to the post whose content references them.

Uploads from stale cached editors can miss the post_id wiring, leaving the
file invisible in the post's attachment list and in the editor's existing
attachments view. This script scans every post body for /file/download/<id>
and /file/preview/<id> references and attaches matching orphan files to that
post, but only when the file owner and the post author are the same user, so
no cross-user claims are possible.

Usage: python3 tools/link_orphan_files.py [path/to/blog.db]
Defaults to data/blog.db relative to the repo root. Read-write: run it while
the server is stopped, or accept a possible SQLITE_BUSY retry loop.
"""
import re
import sqlite3
import sys
from pathlib import Path

REF_RE = re.compile(r"/file/(?:download|preview)/(\d+)")


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def main() -> int:
    db_path = Path(sys.argv[1]) if len(sys.argv) > 1 else repo_root() / "data" / "blog.db"
    conn = sqlite3.connect(str(db_path), timeout=30)
    conn.row_factory = sqlite3.Row

    posts = conn.execute("SELECT id, user_id, content FROM posts").fetchall()
    refs: dict[int, list[int]] = {}
    for post in posts:
        if not post["content"]:
            continue
        for fid in {int(m) for m in REF_RE.findall(post["content"])}:
            refs.setdefault(fid, []).append(post["id"])

    orphans = conn.execute(
        "SELECT id, user_id, filename FROM files WHERE post_id IS NULL OR post_id = 0"
    ).fetchall()

    linked = skipped = 0
    for f in orphans:
        fid, f_uid, fname = f["id"], f["user_id"], f["filename"]
        candidates = refs.get(fid, [])
        # Same-owner posts only, newest first: an edit usually re-saves the
        # latest post that uses the file.
        rows = conn.execute(
            "SELECT id FROM posts WHERE id IN (%s) AND user_id = ? ORDER BY id DESC"
            % ",".join("?" * len(candidates)),
            [*candidates, f_uid],
        ).fetchall() if candidates else []
        if not rows:
            print(f"skip file {fid} ({fname}): no same-owner post references it")
            skipped += 1
            continue
        target = rows[0]["id"]
        conn.execute("UPDATE files SET post_id = ? WHERE id = ?", (target, fid))
        linked += 1
        note = ""
        if len(rows) > 1:
            note = f" (also referenced by posts {', '.join(str(r['id']) for r in rows[1:])})"
        print(f"linked file {fid} ({fname}) -> post {target}{note}")

    conn.commit()
    print(f"done: {linked} linked, {skipped} skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
