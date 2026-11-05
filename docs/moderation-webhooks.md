# Asynchronous External Moderation Protocol

fly.board v1.1 can optionally send local moderation queue items to an external review service over NATS. The integration is a signed message contract, not a bundled reviewer or AI service. Operators must provide and operate their own reviewer.

The local SQLite moderation queue remains authoritative. Until a valid decision arrives, content remains pending and invisible to the public. A local moderator can always approve or reject it through the existing moderation interface.

## Enablement

External review is disabled by default. Enable it with all of the following settings:

```sh
export NATS_URL=nats://127.0.0.1:4222
export FLY_MODERATION_WEBHOOKS=1
export FLY_MODERATION_REVIEWER_PUBKEY_B64='<base64 ML-DSA-65 reviewer public key>'
export FLY_MODERATION_TIMEOUT_SECONDS=300
```

`FLY_MODERATION_TIMEOUT_SECONDS` defaults to 300 and accepts 30 through 86400. Startup fails if external review is enabled without NATS, a valid reviewer public key, or a valid timeout. The reviewer private key must never be configured on the fly.board host. Exchange public keys out of band and pin the review request public key in the external service.

Only submissions already classified as `pending` by the local moderation rules generate review requests. Enabling this integration does not itself enable the approval queue or send locally allowed content to a third party.

## Subjects and delivery

- fly.board publishes `moderation.review` envelopes to `flyboard.moderation.review`.
- The reviewer publishes `moderation.decision` envelopes to `flyboard.moderation.decision`.
- fly.board subscribes to decisions only while external review is enabled.

Publishing is asynchronous and bounded by the existing per-process NATS queue. NATS connection, signing, queue overflow, and publish failures do not change the pending item. Ordinary NATS does not provide durable delivery: unsent requests are dropped on shutdown and a disconnected reviewer can miss requests. The fallback in every such case is local review; there is no automatic allow or reject.

Every worker may receive the same decision. Database idempotency makes this safe. Decisions are not a substitute for NATS authentication or transport encryption; use a private NATS deployment with TLS and appropriate subject permissions.

## Data minimization

A review request contains only the opaque correlation ID, target type, content requiring review, local rule reason, issue time, and expiry time. It does not contain the local target ID, user ID, username, author display name, email address, IP address, post title, slug, or session data. The content itself is necessary for review and may contain user-supplied personal data, so operators remain responsible for reviewer selection, retention policy, and transport confidentiality.

## Review request

Example envelope:

```json
{
  "version": 1,
  "type": "moderation.review",
  "correlation_id": "0123456789abcdef0123456789abcdef",
  "target_type": "comment",
  "content": "Content requiring review",
  "reason": "approval_queue",
  "issued_at": 1762300000,
  "expires_at": 1762300300,
  "sig_alg": "ML-DSA-65",
  "sig_b64": "<key-id>:<base64 signature>",
  "pubkey_b64": "<base64 fly.board public key>"
}
```

`correlation_id` is 32 lowercase hexadecimal characters representing 128 random bits. `target_type` is `post` or `comment`. Times are integer Unix seconds. Content is limited to 1 MiB; the local rule reason is limited to 128 bytes.

The signature uses ML-DSA-65 with context `fly.board/moderation-request/v1`. Its signed bytes are the compact JSON serialization of exactly these fields, in this order, with no whitespace added by the serializer:

```json
{"version":1,"type":"moderation.review","correlation_id":"0123456789abcdef0123456789abcdef","target_type":"comment","content":"Content requiring review","reason":"approval_queue","issued_at":1762300000,"expires_at":1762300300}
```

JSON string escaping follows cJSON compact serialization. The `sig_alg`, `sig_b64`, and `pubkey_b64` envelope fields are not signed as fields; their values authenticate the canonical body. Reviewers must pin the fly.board public key rather than trusting an arbitrary supplied key.

## Decision

The reviewer answers with one of `allow`, `pending`, or `reject`:

```json
{
  "version": 1,
  "type": "moderation.decision",
  "correlation_id": "0123456789abcdef0123456789abcdef",
  "decision_id": "reviewer-01JEXAMPLE000000000000000",
  "action": "allow",
  "issued_at": 1762300010,
  "reason": "acceptable",
  "sig_alg": "ML-DSA-65",
  "sig_b64": "<reviewer-key-id>:<base64 signature>",
  "pubkey_b64": "<base64 reviewer public key>"
}
```

The signed compact body contains exactly `version`, `type`, `correlation_id`, `decision_id`, `action`, `issued_at`, and `reason`, in that order. The ML-DSA-65 context is `fly.board/moderation-decision/v1`. A request signature cannot be replayed as a decision signature.

`decision_id` is reviewer-generated, 1 through 64 ASCII letters, digits, hyphens, or underscores, and must be globally unique. `reason` is required but may be empty and is limited to 256 bytes. The decision public key and signature key ID must match the pinned reviewer key.

## Validation and state transitions

fly.board rejects malformed envelopes, unsupported versions or actions, extra or missing fields, unknown correlations, invalid signatures, wrong keys, expired requests, decisions issued more than five minutes in the future, and decisions issued more than five minutes before the request.

- `pending` records an acknowledgement but leaves the item pending.
- `allow` performs the existing local approval transition.
- `reject` performs the existing local rejection transition.
- A locally resolved or manually reviewed item cannot be changed later by an external decision.

The first valid `decision_id` is stored with its complete decision fields. An exact replay is an idempotent duplicate. Reusing that ID with different fields is rejected. Receipts are retained so old decisions cannot be replayed after a restart; deployments should include the database in normal backup and capacity planning.

## Loopback verification

`tests/test_nats_moderation.c` uses the real ML-DSA-65 implementation and real moderation database transitions. It constructs requests and signed decisions, then delivers decisions directly to the same processing function used by the NATS callback. It covers metadata minimization, pending, allow, reject, replay, conflicting IDs, tampering, expiry, unknown correlations, and local fallback. It does not claim to test an external reviewer or require a running NATS broker.
