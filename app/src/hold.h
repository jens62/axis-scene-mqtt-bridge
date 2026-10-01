#ifndef HOLD_H
#define HOLD_H

#include <glib.h>

/**
 * Merges bursts of on/off events into episodes ("someone is speaking").
 *
 * Per topic: the first "on" is passed on immediately, further "on" events while the episode
 * lasts are swallowed. An "off" is held back for hold_s seconds; if another "on" arrives in
 * that time the episode simply continues, otherwise the (last) "off" is passed on when the
 * time is up. An "off" with no episode running (e.g. replayed start-up state) passes at once.
 *
 * Needs a running GLib main loop (timers); not thread safe, call from the main loop only.
 */
typedef struct hold hold_t;

typedef void (*hold_publish_fn)(const char* sub_topic, const char* json, void* user_data);

hold_t* hold_new(int hold_s, hold_publish_fn publish, void* user_data);
void hold_free(hold_t* h);

/** state: 1 = on, 0 = off. */
void hold_event(hold_t* h, const char* sub_topic, const char* json, int state);

#endif
