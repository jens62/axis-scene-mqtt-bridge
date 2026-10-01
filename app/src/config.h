#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <axsdk/axparameter.h>

#define APP_NAME "axis_scene_mqtt_bridge"

typedef struct {
    char* mqtt_host;
    int mqtt_port;
    char* mqtt_user;
    char* mqtt_password;
    char* topic_prefix;  // always set after config_load()
    bool publish_objects;
    bool publish_audio;
    bool publish_motion;
    bool publish_unclassified;  // objects without a class count as a change
    char* scene_topic;
    char* scene_source;
    double move_threshold;  // normalized (0..1) distance a track must move to be re-published
    int min_interval_ms;    // minimum time between "moved only" publishes
    int audio_hold_s;       // merge audio on/off bursts, 0 = off
    int clear_timeout_s;    // no object frames for this long -> publish an empty scene
    char* audio_events;
    char* motion_events;
    char* event_keys;
} config_t;

/** Reads all parameters. Returns false if the parameters can't be read at all. */
bool config_load(AXParameter* handle, config_t* cfg);
void config_free(config_t* cfg);

/** Writes the effective settings to syslog. The password is never logged. */
void config_log(const config_t* cfg);

#endif
