#ifndef EVENTS_H
#define EVENTS_H

#include <stdbool.h>

#include "config.h"

/**
 * Subscribes to the configured audio and motion events of the camera's event
 * system and publishes every event as JSON to "<prefix>/audio/<topic path>" or
 * "<prefix>/motion/<topic path>". Stateful events are only sent by the camera
 * when their state changes (PropertyOperation "Changed"), so no filtering is
 * done here.
 */
bool events_start(const config_t* cfg);
void events_stop(void);

#endif
