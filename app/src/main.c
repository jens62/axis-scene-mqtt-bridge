#include <glib-unix.h>
#include <stdlib.h>
#include <syslog.h>

#include "config.h"
#include "events.h"
#include "mqtt.h"
#include "scene.h"

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

/** Settings are read once at start: a change ends the app and the ACAP framework respawns it. */
static void on_parameter_changed(const gchar* name, const gchar* value, gpointer user_data) {
    (void)value;
    (void)user_data;
    syslog(LOG_INFO, "Parameter %s changed, restarting", name);
    g_timeout_add_seconds(1, quit_for_restart, NULL);
}

static void watch_parameters(AXParameter* handle) {
    static const char* const names[] = {
        "MqttHost", "MqttPort", "MqttUser", "MqttPassword", "TopicPrefix", "PublishObjects",
        "PublishAudio", "PublishMotion", "PublishUnclassified", "SceneTopic", "SceneSource", "MoveThreshold",
        "MinIntervalMs", "ClearTimeoutSec", "AudioEvents", "MotionEvents", "EventKeys",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
        ax_parameter_register_callback(handle, names[i], on_parameter_changed, NULL, NULL);
}

int main(void) {
    openlog(APP_NAME, LOG_PID, LOG_USER);

    GError* error       = NULL;
    AXParameter* handle = ax_parameter_new(APP_NAME, &error);
    if (handle == NULL) {
        syslog(LOG_ERR, "Cannot open parameters: %s", error->message);
        return EXIT_FAILURE;
    }

    config_t cfg;
    config_load(handle, &cfg);
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

    events_stop();
    scene_stop();
    mqtt_stop();
    g_main_loop_unref(loop);
    config_free(&cfg);
    ax_parameter_free(handle);
    return EXIT_SUCCESS;
}
