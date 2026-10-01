#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"

bool mqtt_start(const config_t* cfg);

/** Publishes to "<prefix>/<subtopic>". Thread-safe. Drops the message while disconnected. */
void mqtt_publish(const char* subtopic, const char* payload, size_t len, bool retain);

void mqtt_stop(void);

#endif
