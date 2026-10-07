#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static char* get_string(AXParameter* handle, const char* name) {
    GError* error = NULL;
    gchar* value  = NULL;

    if (!ax_parameter_get(handle, name, &value, &error)) {
        syslog(LOG_ERR, "Cannot read parameter %s: %s", name, error->message);
        g_clear_error(&error);
        return g_strdup("");
    }
    g_strstrip(value);
    return value;
}

static bool get_bool(AXParameter* handle, const char* name) {
    char* value = get_string(handle, name);
    bool yes    = strcmp(value, "yes") == 0;
    g_free(value);
    return yes;
}

static int get_int(AXParameter* handle, const char* name, int fallback, int min) {
    char* value = get_string(handle, name);
    char* end   = NULL;
    long n      = strtol(value, &end, 10);
    int result  = (end != value && *end == '\0' && n >= min && n <= 65535 * 1000) ? (int)n
                                                                                  : fallback;
    g_free(value);
    return result;
}

static double get_double(AXParameter* handle, const char* name, double fallback) {
    char* value  = get_string(handle, name);
    char* end    = NULL;
    double d     = g_ascii_strtod(value, &end);
    double result = (end != value && *end == '\0' && d >= 0.0) ? d : fallback;
    g_free(value);
    return result;
}

/** Axis host names are "axis-<MAC>", which equals "axis-<serial number>". */
static char* default_prefix(AXParameter* handle) {
    GError* error  = NULL;
    gchar* serial  = NULL;

    if (!ax_parameter_get(handle, "Properties.System.SerialNumber", &serial, &error)) {
        syslog(LOG_WARNING, "Cannot read serial number: %s", error->message);
        g_clear_error(&error);
        return g_strdup("axis/unknown/bridge");
    }
    char* prefix = g_strdup_printf("axis/%s/bridge", serial);
    g_free(serial);
    return prefix;
}

bool config_load(AXParameter* handle, config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->mqtt_host     = get_string(handle, "MqttHost");
    cfg->mqtt_port     = get_int(handle, "MqttPort", 1883, 1);
    cfg->mqtt_user     = get_string(handle, "MqttUser");
    cfg->mqtt_password = get_string(handle, "MqttPassword");
    cfg->topic_prefix  = get_string(handle, "TopicPrefix");
    if (cfg->topic_prefix[0] == '\0') {
        g_free(cfg->topic_prefix);
        cfg->topic_prefix = default_prefix(handle);
    }
    // No leading/trailing slashes, they would create empty topic levels.
    g_strdelimit(cfg->topic_prefix, "#+", '_');
    g_strstrip(cfg->topic_prefix);
    size_t len = strlen(cfg->topic_prefix);
    while (len > 0 && cfg->topic_prefix[len - 1] == '/')
        cfg->topic_prefix[--len] = '\0';

    cfg->publish_objects = get_bool(handle, "PublishObjects");
    cfg->publish_audio   = get_bool(handle, "PublishAudio");
    cfg->publish_motion  = get_bool(handle, "PublishMotion");
    cfg->publish_unclassified = get_bool(handle, "PublishUnclassified");
    cfg->scene_transport = get_string(handle, "SceneTransport");
    if (strcmp(cfg->scene_transport, "messagebroker") != 0) {
        g_free(cfg->scene_transport);
        cfg->scene_transport = g_strdup("devicedatahub");
    }
    cfg->scene_topic     = get_string(handle, "SceneTopic");
    // The deprecated topic only exists in the Message Broker. A setting stored by an older version
    // would make the Device Data Hub subscription fail, so use its successor instead.
    if (strcmp(cfg->scene_transport, "devicedatahub") == 0 &&
        strcmp(cfg->scene_topic, "com.axis.analytics_scene_description.v0.beta") == 0) {
        syslog(LOG_WARNING, "SceneTopic %s is deprecated and not available in Device Data Hub, "
                            "using com.axis.scene.frame.v1", cfg->scene_topic);
        g_free(cfg->scene_topic);
        cfg->scene_topic = g_strdup("com.axis.scene.frame.v1");
    }
    cfg->scene_source    = get_string(handle, "SceneSource");
    cfg->objects_start_stop = get_bool(handle, "ObjectsStartStopOnly");
    cfg->move_threshold  = get_double(handle, "MoveThreshold", 0.05);
    cfg->min_interval_ms = get_int(handle, "MinIntervalMs", 1000, 0);
    cfg->clear_timeout_s = get_int(handle, "ClearTimeoutSec", 3, 1);
    cfg->audio_hold_s    = get_int(handle, "AudioHoldSec", 5, 0);
    cfg->motion_hold_s   = get_int(handle, "MotionHoldSec", 5, 0);
    cfg->audio_events    = get_string(handle, "AudioEvents");
    cfg->motion_events   = get_string(handle, "MotionEvents");
    cfg->event_keys      = get_string(handle, "EventKeys");
    return true;
}

void config_free(config_t* cfg) {
    g_free(cfg->mqtt_host);
    g_free(cfg->mqtt_user);
    g_free(cfg->mqtt_password);
    g_free(cfg->topic_prefix);
    g_free(cfg->scene_transport);
    g_free(cfg->scene_topic);
    g_free(cfg->scene_source);
    g_free(cfg->audio_events);
    g_free(cfg->motion_events);
    g_free(cfg->event_keys);
    memset(cfg, 0, sizeof(*cfg));
}

void config_log(const config_t* cfg) {
    syslog(LOG_INFO, "Broker %s:%d, user '%s', password %s", cfg->mqtt_host, cfg->mqtt_port,
           cfg->mqtt_user, cfg->mqtt_password[0] != '\0' ? "set" : "not set");
    syslog(LOG_INFO, "Topic prefix %s", cfg->topic_prefix);
    syslog(LOG_INFO, "Publish: objects %s, audio %s, motion %s, unclassified objects %s",
           cfg->publish_objects ? "yes" : "no", cfg->publish_audio ? "yes" : "no",
           cfg->publish_motion ? "yes" : "no", cfg->publish_unclassified ? "yes" : "no");
    syslog(LOG_INFO, "Scene transport %s, topic %s, source (channel) %s", cfg->scene_transport,
           cfg->scene_topic, cfg->scene_source);
    syslog(LOG_INFO, "Objects: %s", cfg->objects_start_stop ? "start/stop only" : "also movement and attribute changes");
    syslog(LOG_INFO, "Move threshold %.3f, min interval %d ms, empty scene after %d s",
           cfg->move_threshold, cfg->min_interval_ms, cfg->clear_timeout_s);
    syslog(LOG_INFO, "Hold time audio %d s, motion %d s", cfg->audio_hold_s, cfg->motion_hold_s);
    syslog(LOG_INFO, "Audio events: %s", cfg->audio_events);
    syslog(LOG_INFO, "Motion events: %s", cfg->motion_events);
    syslog(LOG_INFO, "Event keys: %s", cfg->event_keys);
}
