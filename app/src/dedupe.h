#ifndef DEDUPE_H
#define DEDUPE_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Decides which scene frames are worth publishing. Understands
 *  - com.axis.scene.frame.v1:                      {"detections":[{"object_track_id":..,"class":{..}}]}
 *  - com.axis.analytics_scene_description.v0.beta: {"frame":{"observations":[{"track_id":..,"class":{..}}]}}
 *
 * Compared with the last published frame, ignoring timestamp, bounding_box and score values (lists
 * of {name, score} count with their best entry only):
 *  - a classified object appeared/disappeared or its attributes changed -> change
 *  - an object moved further than move_threshold (normalized image units) -> change
 * A frame with only "timestamp" (no list) is an empty scene. Objects without a "class" are ignored
 * unless publish_unclassified is set. Faces ("Face", "Head" in frame.v1) never
 * cause a change by themselves; they are part of whatever frame is published.
 *
 * At most one frame per min_interval_ms is published. A change that happens inside the interval is
 * remembered; the newest such frame is handed out by dedupe_take_pending() once the interval is
 * over (so "the person left" is never lost).
 */
typedef struct dedupe dedupe_t;

dedupe_t* dedupe_new(double move_threshold, int min_interval_ms, bool publish_unclassified);
/**
 * Motion-detector style: publish when an object appears and when it is gone (see dedupe_expired),
 * ignore movement and attribute changes (clothing colours flicker) while the track lives. Only a
 * change of the class type of an existing track still counts.
 */
void dedupe_set_start_stop_only(dedupe_t* d, bool on);

void dedupe_free(dedupe_t* d);

/**
 * An idle frame of com.axis.scene.frame.v1 has no object list ({"channel_id":..,"timestamp":..}).
 * Returns a copy with an explicit "detections":[] for consumers, or NULL if the frame needs no
 * change (anything else is published as it came). Free with g_free().
 */
char* dedupe_normalize(const char* json, size_t len);

/** True if the frame should be published now. now_ms is a monotonic clock. */
bool dedupe_check(dedupe_t* d, const char* json, size_t len, gint64 now_ms);

/** The newest held-back frame if its interval is over, else NULL. Free with g_free(). */
char* dedupe_take_pending(dedupe_t* d, gint64 now_ms);

/**
 * True once if objects were published and no frame with objects has arrived for
 * clear_timeout_ms. The caller then publishes an empty scene.
 */
bool dedupe_expired(dedupe_t* d, gint64 now_ms, gint64 clear_timeout_ms);

#endif
