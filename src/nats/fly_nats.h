#ifndef FLY_NATS_H
#define FLY_NATS_H

#include <stdbool.h>

/**
 * @brief Initialize flyboard NATS subsystem.
 * Connects to NATS server and subscribes to flyboard.posts.
 * @param url NATS server URL (e.g., "nats://localhost:4222").
 * @return true on success.
 */
bool fly_nats_init(const char *url);

/**
 * @brief Publish post metadata to flyboard.posts subject.
 * The payload is wrapped with an ML-DSA-65 signature and exported public key
 * so downstream consumers can validate origin without a shared secret.
 * @param title   Post title.
 * @param slug    Post slug.
 * @param summary Post summary.
 * @return true on success.
 */
bool fly_nats_publish_post(const char *title, const char *slug, const char *summary);

/**
 * @brief Dispatch incoming NATS messages.
 * Call this in a background thread.
 */
void fly_nats_dispatch(void);

/**
 * @brief Shutdown NATS subsystem.
 */
void fly_nats_close(void);

#endif
