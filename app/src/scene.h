#ifndef SCENE_H
#define SCENE_H

#include <stdbool.h>

#include "config.h"

/** Subscribes to the scene metadata topic and publishes changes to "<prefix>/objects". */
bool scene_start(const config_t* cfg);
void scene_stop(void);

#endif
