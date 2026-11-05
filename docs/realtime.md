# Realtime post presence and comments

Realtime enhancements are disabled by default. They use the existing optional
NATS fabric and a small browser script; no WebSocket service or JavaScript
framework is required.

## Enable

Set the feature in `blog.settings` and restart fly.board:

```ini
realtime_enabled=true
# Optional, independently controlled:
realtime_typing=false
```

Set `NATS_URL=nats://127.0.0.1:4222` on every node for cross-process and
cross-node presence/typing fanout. The existing NATS leaf-node topology can be
used between sites. Without `NATS_URL`, realtime still runs in local mode in
each node (shared by its worker processes), but its ephemeral counts are not cluster-wide.

`realtime_typing=true` has no effect unless `realtime_enabled=true`. Typing is
available only where the current user is allowed to comment.

## Behavior and privacy

- A browser tab generates a random 128-bit session id with Web Crypto and keeps
  it in `sessionStorage`. No account name, IP address, or comment text is put in
  a realtime event.
- Presence heartbeats have a 45-second lease. Viewer output is aggregated into
  `1 viewer`, `2-5 viewers`, `6-20 viewers`, or `21+ viewers`; identities and
  exact larger counts are not exposed.
- Typing indicators say only “Someone is typing...”. They have a five-second
  lease, are hidden from the typing tab itself, and active updates are limited
  to one per 2.5 seconds per session and post. The browser also throttles input
  events. Stop messages are accepted immediately.
- New comments remain authoritative in SQLite. A comment event tells the open
  page to fetch the server-rendered comment list, preserving escaping, reply
  structure, and permissions.
- Without JavaScript, the original server-rendered comments and normal POST
  forms are unchanged. Presence and typing indicators stay hidden.

## Delivery and reconnection

`GET /realtime/events` returns a finite Server-Sent Events response containing
a `state` event and any comments after the supplied id. The response ends and
the browser's native `EventSource` reconnects after two seconds. `Last-Event-ID`
is honored, so reconnects resume without replaying comments. Each response can
return at most 50 comments; subsequent reconnects drain the rest.

This short-response SSE design works through the existing CWIST HTTP/TLS
stack and does not pin a request worker to a permanent socket. It is near-
realtime rather than an instantaneous persistent stream.

NATS subjects:

| Subject | Payload | Purpose |
|---|---|---|
| `flyboard.realtime.presence` | `post_id`, opaque `session`, `active` | Join, heartbeat, and leave |
| `flyboard.realtime.typing` | `post_id`, opaque `session`, `active` | Short typing leases |
| `flyboard.realtime.comments` | `post_id`, `comment_id` | New-comment notification |

Ephemeral NATS messages are not signed or persisted. Treat the NATS network as
trusted and private, as with the existing fabric. Presence and typing are
best-effort and can be approximate during network partitions. SQLite replay
through the id cursor prevents a missed NATS notification from losing a
comment update.

If NATS is absent or reconnecting, posting and reading comments continue
normally, SSE reconnects continue, and local ephemeral state remains usable.
The `state` event reports `transport: "local"`; the browser does not turn this
into a disruptive error. When the broker returns, cnats reconnects and fanout
resumes. Queued messages are bounded and may be dropped, so expired presence
or typing should never be treated as authoritative.

Each node keeps at most 2048 ephemeral sessions in shared memory initialized
before workers fork, so all local workers enforce the same rate limits. Excess
updates receive a degraded response rather than growing memory without limit. These leases and
counts are operational hints, not analytics or an audit trail.

## Tests

```sh
make check-realtime
# Requires Docker; runs two processes against a real NATS broker, then stops
# and restarts the broker to verify recovery.
make check-realtime-nats
```

The browser test covers event handling, comment refreshes, throttled typing,
and leave beacons. The C tests cover leases, rate limits, privacy buckets,
cross-process delivery, external comment subscribers, and broker reconnects.
