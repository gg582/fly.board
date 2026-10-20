#ifndef FLY_NATS_H
#define FLY_NATS_H

#include <stdbool.h>

/**
 * @brief Configure the flyboard NATS subsystem (no connection yet).
 * Each serving process connects on its own, after cwist forks the workers,
 * from a thread started by fly_nats_ensure(); see fly_nats.c.
 * @param url NATS server URL (e.g., "nats://localhost:4222").
 * @return true.
 */
bool fly_nats_init(const char *url);

/**
 * @brief Start this process's NATS thread if it is not running yet.
 * Call from the request path: the first request a process serves marks it as
 * serving (always after the fork) and starts its connection + subscriptions.
 * Cheap after the first call. No-op when NATS is not configured.
 */
void fly_nats_ensure(void);

/**
 * @brief Publish post metadata to flyboard.posts subject.
 * The payload is wrapped with an ML-DSA-65 signature and exported public key
 * so downstream consumers can validate origin without a shared secret.
 * @param title   Post title.
 * @param slug    Post slug.
 * @param summary Post summary.
 * Queued for this process's NATS thread; never blocks on the network.
 * @return true when queued.
 */
bool fly_nats_publish_post(const char *title, const char *slug, const char *summary);

/**
 * @brief Publish a comment/reply notification event to flyboard.comments.
 * Signed JSON payload like fly_nats_publish_post, plus an origin field so
 * receiving nodes can skip events they already recorded locally.
 * @param origin            Site origin of the publishing node (g_config.root_url).
 * @param actor_name        Display name of the comment author.
 * @param kind              "comment" (on a post) or "reply" (to a comment).
 * @param post_slug         Slug of the target post, "" when unknown/file target.
 * @param recipient_user_id Local user id of the notification recipient.
 * @param excerpt           Short excerpt of the comment content.
 * Queued like fly_nats_publish_post().
 * @return true when queued.
 */
bool fly_nats_publish_comment(const char *origin, const char *actor_name, const char *kind, const char *post_slug, long recipient_user_id, const char *excerpt);

/**
 * @brief Shutdown NATS subsystem.
 */
void fly_nats_close(void);

#endif
