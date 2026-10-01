#ifndef DEDUPE_H
#define DEDUPE_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Decides which scene frames (com.axis.scene.frame.v1) are worth publishing.
 *
 * A frame arrives every ~100 ms and differs from its predecessor in the
 * timestamp, in the bounding boxes (objects move) and in classifier scores
 * (they jitter). A frame is published if
 *  - a track appeared or disappeared, or
 *  - the "semantic" content of a track changed: everything except timestamp,
 *    bounding_box and score values; lists of {name, score} (colors, ...) count
 *    only with their best entry, or
 *  - a track moved further than move_threshold (normalized image units) since
 *    the last published frame AND min_interval_ms have passed.
 */
typedef struct dedupe dedupe_t;

dedupe_t* dedupe_new(double move_threshold, int min_interval_ms);
void dedupe_free(dedupe_t* d);

/** True if the frame should be published. now_ms is a monotonic clock. */
bool dedupe_check(dedupe_t* d, const char* json, size_t len, gint64 now_ms);

/**
 * True once if objects were published and no frame with objects has arrived for
 * clear_timeout_ms. The caller then publishes an empty scene.
 */
bool dedupe_expired(dedupe_t* d, gint64 now_ms, gint64 clear_timeout_ms);

#endif
