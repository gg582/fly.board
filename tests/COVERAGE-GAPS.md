# Coverage gaps

Features that could not be unit-tested without modifying `src/` (forbidden by
task rules), or that crash/depend on infrastructure unavailable in a unit test.

- `render_markdown_to_html("")` (empty string input): segfaults in
  `protect_math` at src/render/render_md.c:1194 — `strdup(NULL)` when the
  protected buffer is empty. Needs a src fix (e.g. guard `out->data`); tested
  around it with whitespace-only input in `test_render_md.c`.
- `src/render` full page renderers (`render_page`, `render_post_detail`,
  etc.): require the full CWIST app context (engine globals, i18n loader,
  inlined asset registry, legal doc store). Only `render_md` and
  `theme_build_json`/`theme_build_css` are covered.
- `auth_jwt_verify_from_request` / cookie handling: needs a populated
  `cwist_http_request`; covered indirectly via `cwist_jwt_verify` in
  `test_auth_jwt.c`.
- Anonymous rate-limit keying in `spam_guard` (`getpeername` path): covered
  only for the uid-keyed writer path and one socketpair peer; exotic address
  families untested.
- `src/db` search, votes, comments, notifications, files, board_tree,
  pqc_keys, report, sync replication: need either large surrounding
  subsystems or are thin SQL wrappers already exercised via migrations;
  search in particular was stubbed out in the db tests.
