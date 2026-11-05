# Stale-while-revalidate page refresh

fly.board can serve a recently expired rendered page while one background job
refreshes it. This avoids making every request in a hot-page burst wait for the
same database reads and HTML render.

The feature is **off by default**. Set `FLY_PAGE_SWR_SECONDS` to a positive
number of seconds and restart the service to enable it. The normal page TTL is
unchanged (60 seconds for the supported pages).

## Request behavior

The home page, board directory, board post lists, and public post detail pages
use stale-while-revalidate. A request receives:

1. the fresh page-cache entry when one exists;
2. otherwise, the expired entry while it is within the configured stale window;
3. otherwise, the normal single-flight synchronous render.

A stale response has `X-Fly-Cache-Status: stale`. These responses are not stored
in the route BDR, so refreshing the page cache cannot leave a second stale copy
in the route-level cache. Browser cache policy remains private/no-store.

The first stale request for a cache key elects the only background leader for
that key. Other stale requests return immediately without joining a refresh
queue. Leader election uses the existing request coalescer and expires a stuck
leader after five seconds, allowing a replacement.

## Bounds and failure handling

Each serving process has a dedicated lazy refresh pool. Its threads start only
in the serving process, after CWIST's worker fork. Jobs contain copied scalar
and string values, never a request, response, connection, or renderer-owned
pointer. Each job opens and closes its own main SQLite connection and uses the
existing fork gate around connection setup and teardown.

| Variable | Default | Range | Effect |
|---|---:|---:|---|
| `FLY_PAGE_SWR_SECONDS` | `0` | 0–86400 | Stale retention window; zero disables the feature |
| `FLY_PAGE_REFRESH_WORKERS` | `2` | 1–16 | Background render threads per serving process |
| `FLY_PAGE_REFRESH_QUEUE_MAX` | `64` | 1–4096 | Maximum waiting jobs, excluding jobs already running |

If allocation, thread creation, or queue admission fails, the current stale
response is still returned and a later request may retry the refresh. A refresh
failure leaves the old value in place until its stale window ends. Cache memory
limits still include fresh and retained stale values; oversized or over-budget
refreshes are not cached.

## Mutation safety

Every page-cache invalidation advances a cache generation. A background job
captures that generation before reading the database and can install its result
only if the generation is unchanged. Thus a render that overlaps a post, board,
listing, policy, or full-cache invalidation cannot repopulate an invalidated
entry. A mutation also marks pinned invalidated entries as unusable for stale
lookups; existing readers retain valid memory until they release their pins.

Cache entries, leader elections, generations, and refresh queues are local to
each serving process. The route BDR's existing cross-process content versions
remain the cross-process invalidation mechanism. Consequently, different worker
processes may refresh independently.

Background post-detail renders do not increment view counts and do not reproduce
request-only actions such as delete-confirmation links. Only public post pages
are eligible for refresh caching.

## Verification

`make check-utils` runs the page-cache, request-coalescing, and SWR tests. The
SWR suite covers concurrent stale hits with exactly one leader, stuck-leader
timeout, queue overflow, mutation during refresh, pinned-entry replacement, and
concurrent set/get/invalidate stress.
