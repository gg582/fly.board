# NATS Integration Guide for fly.board

## Purpose

`fly.board` can broadcast newly created post metadata over NATS so other nodes, bots, or sidecar services can react without polling the HTTP app.

The current integration is intentionally narrow:

- One subject: `flyboard.posts`
- One publisher path: post creation
- One subscriber path: background dispatch thread
- One PQC primitive: `ML-DSA-65` message signing

There is no PQC encryption in the current codebase. Message confidentiality still depends on network isolation and TLS around the NATS transport.

## Runtime Enablement

NATS is optional. It turns on only when `NATS_URL` is set at process start.

```bash
export NATS_URL=nats://localhost:4222
./fly_board
```

If `NATS_URL` is unset, the server runs normally without any messaging side effects.

## Startup Flow

The initialization sequence in [src/main.c](/home/yjlee/fly.board/src/main.c:68) is:

1. Verify working directory and public assets.
2. Initialize PQC signing via `fly_crypto_init()`.
3. Load admin and blog configuration.
4. Read `NATS_URL`.
5. Call `fly_nats_init(NATS_URL)`.
6. If that succeeds, start a dedicated `pthread` that loops on `fly_nats_dispatch()`.

This matters because NATS payload signing depends on the global PQC keypair already being live before the first post publish.

## Subject Model

| Subject | Producer | Consumer | Purpose |
|---------|----------|----------|---------|
| `flyboard.posts` | `handler_post_new_post` | `on_post_msg` callback and any external subscribers | Broadcast new post metadata |

The publish call originates from the post creation handler after the database write succeeds. The post itself remains authoritative in SQLite; NATS is a fanout channel, not the source of truth.

## Message Schema

Current payloads are emitted by [src/nats/fly_nats.c](/home/yjlee/fly.board/src/nats/fly_nats.c:66) as compact JSON:

```json
{
  "title": "Post Title",
  "slug": "post-title",
  "summary": "Short description",
  "sig_alg": "ML-DSA-65",
  "sig_b64": "<base64 signature>",
  "pubkey_b64": "<base64 public key>"
}
```

### Signed Region

The signature is computed over the canonical base object only:

```json
{
  "title": "Post Title",
  "slug": "post-title",
  "summary": "Short description"
}
```

Then `sig_alg`, `sig_b64`, and `pubkey_b64` are attached as an envelope. Consumers should verify the signature over the three-field canonical object, not the final envelope string.

## Publisher Behavior

Publishing happens inside `fly_nats_publish_post()`:

1. Build the canonical `{title, slug, summary}` object.
2. Serialize it with `cJSON_PrintUnformatted`.
3. Sign the serialized bytes with `fly_crypto_sign(...)`.
4. Export the active public key with `fly_crypto_pubkey_export(...)`.
5. Attach `sig_alg`, `sig_b64`, and `pubkey_b64`.
6. Publish the final envelope to `flyboard.posts`.

If signing fails, publishing fails. That is intentional: externally exposed messages should not silently drop back to unsigned mode.

## Subscriber Behavior

The built-in subscriber is minimal by design. It currently:

- subscribes to `flyboard.posts` in `fly_nats_init()`
- receives messages in `on_post_msg(...)`
- parses the payload
- logs the `slug`
- logs whether a signature field is present

It does not yet perform full signature verification in-process. That is left open for a stricter downstream worker or a future hardening patch.

## Threading Model

NATS dispatch runs outside the HTTP request path.

- Main thread: boot, configure CWIST, serve HTTP/HTTPS.
- NATS worker thread: tight loop calling `fly_nats_dispatch()` while `g_nats_running` is true.

Operationally, this means:

- request latency is not blocked on inbound subscriber callbacks
- outbound publish still occurs on the request path
- inbound fanout handling is isolated to one thread today

The current design is single-dispatch-thread, not a parallel consumer pool.

## Security Model

### What is PQC-protected now

- Post authenticity in the main application path through `fly_crypto_sign` and `fly_crypto_verify`
- NATS broadcast authenticity through `sig_b64` and `pubkey_b64`

### What is not PQC-protected

- NATS transport confidentiality
- File upload/download confidentiality
- Any key exchange with external clients

That limitation exists because the codebase currently exposes PQC signatures, not a PQC KEM or encryption primitive.

### Practical hardening guidance

- Run NATS on a private network segment.
- Prefer TLS-enabled NATS transport in front of or inside the broker deployment.
- Pin or distribute trusted public keys out of band if consumers should reject unknown publishers.
- Treat unsigned or malformed envelopes as hostile input.

## Relationship to TASFA and Other External Surfaces

The repository also has external file-transfer paths such as TASFA upload/download handshakes. Those paths currently use integrity/authenticity mechanisms based on shared-secret HMAC, not PQC encryption. They should be described separately from NATS so operators do not assume the entire external surface is already post-quantum hardened.

If broader PQC coverage is required, the next honest step is to add a PQC KEM or hybrid TLS/KEM layer first, not to relabel signatures as encryption.

## Local Testing

Example local run:

```bash
docker run --rm -p 4222:4222 nats:latest
export NATS_URL=nats://127.0.0.1:4222
./fly_board
```

Then create a post and subscribe from another shell:

```bash
nats sub flyboard.posts
```

You should see a JSON envelope containing `title`, `slug`, `summary`, `sig_alg`, `sig_b64`, and `pubkey_b64`.

## Extension Points

If you add more subjects, keep the same pattern:

- define a minimal canonical body
- sign that body
- attach algorithm and public-key metadata
- keep the authoritative state in the database, not in NATS

That keeps attack surface smaller than introducing mutable event-only state or unsigned cross-node control traffic.
