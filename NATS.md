# NATS Integration Guide for fly.board

## Overview

fly.board uses NATS as an optional messaging backbone for real-time post broadcasting across distributed instances. When enabled, every new post is published to the `flyboard.posts` subject, allowing subscribers (other fly.board nodes, microservices, or bots) to react immediately.

## Enabling NATS

Set the `NATS_URL` environment variable before starting the server:

```bash
export NATS_URL=nats://localhost:4222
./fly_board
```

If `NATS_URL` is unset or empty, NATS is disabled and the blog runs as a standalone instance without messaging.

## Docker Compose

The provided `docker-compose.yml` already forwards `NATS_URL`:

```yaml
environment:
  - NATS_URL=${NATS_URL:-}
```

To connect to an existing NATS server:

```bash
NATS_URL=nats://nats.example.com:4222 docker compose up --build
```

Or start a local NATS container alongside fly.board:

```yaml
services:
  nats:
    image: nats:latest
    ports:
      - "4222:4222"
  flyboard:
    build: .
    environment:
      - NATS_URL=nats://nats:4222
    depends_on:
      - nats
```

## Topic Structure

| Subject | Direction | Description |
|---------|-----------|-------------|
| `flyboard.posts` | Publish | Broadcasts metadata of a newly created post |

## Message Format

When a post is created, fly.board publishes a JSON message:

```json
{
  "title": "Post Title",
  "slug": "post-title",
  "summary": "Short description",
  "timestamp": "2026-05-09T13:45:00Z"
}
```

Consumers can use `slug` to construct the full URL: `https://<host>/post/<slug>`.

## Internal Architecture

### Publisher
- **Trigger**: `handler_post_new_post` after successful database insert
- **Function**: `fly_nats_publish_post(title, slug, summary)`
- **Non-blocking**: The HTTP handler does not wait for NATS ACK; failures are logged at DEBUG level

### Subscriber
- **Trigger**: Background `pthread` created in `main()` if `NATS_URL` is set
- **Function**: `fly_nats_dispatch()` loops and calls the subscription callback
- **Current behavior**: Logs received messages via `FLY_LOG_DEBUG` (custom handlers can be added in `fly_nats.c`)

## Extending NATS Usage

To add new subjects (e.g., comments, file uploads, user events), extend `src/nats/fly_nats.c`:

1. Add a publish helper:
   ```c
   void fly_nats_publish_comment(int post_id, const char *author, const char *content) {
       if (!g_nats.conn) return;
       // Build JSON and publish to "flyboard.comments"
   }
   ```

2. Subscribe in `fly_nats_init`:
   ```c
   natsSubscription *sub;
   natsConnection_Subscribe(&sub, conn, "flyboard.comments", on_comment_msg, NULL);
   ```

3. Invoke the publisher from the appropriate handler in `src/handlers/handlers.c`.

## Security Considerations

- NATS connections are currently plaintext (`nats://`). For production, use TLS (`nats://` with `natsOptions_SetSecure`) or place NATS inside a private network.
- No authentication tokens are passed in fly.board messages. If the consumer needs to verify the sender, sign the JSON payload with the existing PQC key (`fly_crypto_sign`) and attach the signature to the message.

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `NATS init failed, continuing without messaging` | Server unreachable or wrong URL | Check `NATS_URL` and network connectivity |
| No messages received | Subscriber not running | Ensure `fly_nats_dispatch()` thread is spawned in `main()` |
| Messages lost on restart | NATS running in memory-only mode | Run NATS with JetStream or a persistent store |
