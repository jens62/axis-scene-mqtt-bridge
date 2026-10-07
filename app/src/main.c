#include <glib-unix.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "config.h"
#include "events.h"
#include "mqtt.h"
#include "scene.h"

#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif

static GMainLoop* loop;

static gboolean on_signal(gpointer user_data) {
    (void)user_data;
    syslog(LOG_INFO, "Stopping");
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gboolean quit_for_restart(gpointer user_data) {
    (void)user_data;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

/**
 * Shutdown must never hang: a process that neither works nor exits is not restarted by the
 * camera. If cleaning up takes too long, leave with an error code so that it is respawned.
 */
static gpointer shutdown_watchdog(gpointer user_data) {
    (void)user_data;
    g_usleep(5 * G_USEC_PER_SEC);
    syslog(LOG_ERR, "Shutdown did not finish within 5 s, exiting");
    _exit(EXIT_FAILURE);
    return NULL;
}

static GHashTable* loaded_values;  // parameter name -> value this run was started with

/** The camera's name is "root.Axis_scene_mqtt_bridge.MqttHost"; ours is the last part. */
static const char* short_name(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot != NULL ? dot + 1 : name;
}

/**
 * Settings are read once at start: a change ends the app and the ACAP framework respawns it.
 * The camera also notifies all parameters of the group at once (e.g. after an upgrade) with the
 * values in use: those are ignored, only a different value restarts the app.
 */
static void on_parameter_changed(const gchar* name, const gchar* value, gpointer user_data) {
    (void)user_data;
    const char* key = short_name(name);
    const char* old = g_hash_table_lookup(loaded_values, key);
    char* now       = g_strstrip(g_strdup(value != NULL ? value : ""));
    gboolean same   = old != NULL && strcmp(old, now) == 0;
    g_free(now);
    if (same) {
        syslog(LOG_INFO, "Parameter %s notified with the value in use, ignored", key);
        return;
    }
    syslog(LOG_INFO, "Parameter %s changed, restarting", key);
    g_timeout_add_seconds(1, quit_for_restart, NULL);
}

static void watch_parameters(AXParameter* handle) {
    static const char* const names[] = {
        "MqttHost", "MqttPort", "MqttUser", "MqttPassword", "TopicPrefix", "PublishObjects",
        "PublishAudio", "PublishMotion", "PublishUnclassified", "SceneTransport", "SceneTopic", "SceneSource", "ObjectsStartStopOnly", "MoveThreshold",
        "MinIntervalMs", "ClearTimeoutSec", "AudioHoldSec", "MotionHoldSec", "AudioEvents", "MotionEvents", "EventKeys",
    };
    loaded_values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        gchar* value = NULL;
        if (ax_parameter_get(handle, names[i], &value, NULL)) {
            g_strstrip(value);
            g_hash_table_insert(loaded_values, g_strdup(names[i]), value);
        }
        ax_parameter_register_callback(handle, names[i], on_parameter_changed, NULL, NULL);
    }
}

int main(void) {
    openlog(APP_NAME, LOG_PID, LOG_USER);

    GError* error       = NULL;
    AXParameter* handle = ax_parameter_new(APP_NAME, &error);
    if (handle == NULL) {
        syslog(LOG_ERR, "Cannot open parameters: %s", error->message);
        return EXIT_FAILURE;
    }

    syslog(LOG_INFO, "%s %s started", APP_NAME, APP_VERSION);

    config_t cfg;
    config_load(handle, &cfg);
    config_log(&cfg);
    watch_parameters(handle);

    loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_unix_signal_add(SIGINT, on_signal, NULL);

    if (cfg.mqtt_host[0] == '\0') {
        syslog(LOG_WARNING, "MqttHost is not set, idle until configured");
    } else if (mqtt_start(&cfg)) {
        if (cfg.publish_objects && !scene_start(&cfg))
            syslog(LOG_ERR, "Object metadata disabled, see above");
        events_start(&cfg);
    }

    g_main_loop_run(loop);

    syslog(LOG_INFO, "Shutting down");
    g_thread_unref(g_thread_new("watchdog", shutdown_watchdog, NULL));
    events_stop();
    syslog(LOG_INFO, "Event subscriptions closed");
    scene_stop();
    syslog(LOG_INFO, "Scene subscription closed");
    mqtt_stop();
    syslog(LOG_INFO, "MQTT closed");
    g_main_loop_unref(loop);
    config_free(&cfg);
    ax_parameter_free(handle);
    return EXIT_SUCCESS;
}
