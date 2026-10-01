#include "scene.h"

#include <mdb/connection.h>
#include <mdb/error.h>
#include <mdb/subscriber.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "dedupe.h"
#include "mqtt.h"

#define OBJECTS_TOPIC "objects"

static mdb_connection_t* connection;
static mdb_subscriber_config_t* subscriber_config;
static mdb_subscriber_t* subscriber;
static dedupe_t* dedupe;
static char* topic;
static char* source;
static guint clear_timer;
static gint received_frames;   // all frames from the camera since the last stats line
static gint published_frames;  // frames that passed the de-duplication
static guint tick;
static gint64 clear_timeout_ms;

static gint64 now_ms(void) {
    return g_get_monotonic_time() / 1000;
}

static void on_connection_error(const mdb_error_t* error, void* user_data) {
    (void)user_data;
    // Let the ACAP framework respawn us, a half-dead broker connection is not recoverable here.
    syslog(LOG_ERR, "Message broker connection error: %s, aborting", error->message);
    abort();
}

static void on_message(const mdb_message_t* message, void* user_data) {
    (void)user_data;
    const mdb_message_payload_t* payload = mdb_message_get_payload(message);

    if (g_atomic_int_add(&received_frames, 1) == 0 && g_atomic_int_get(&published_frames) == 0)
        syslog(LOG_INFO, "First scene frame received");
    if (dedupe_check(dedupe, (const char*)payload->data, payload->size, now_ms())) {
        g_atomic_int_inc(&published_frames);
        mqtt_publish(OBJECTS_TOPIC, (const char*)payload->data, payload->size, false);
    }
}

static void on_channel_registered(const mdb_channel_info_t* info, void* user_data) {
    (void)info;
    (void)user_data;
    syslog(LOG_INFO, "Scene channel %s (source %s) registered", topic, source);
}

static void on_channel_unregistered(void* user_data) {
    (void)user_data;
    syslog(LOG_WARNING, "Scene channel %s (source %s) unregistered or lost", topic, source);
}

static void on_done_subscriber_create(const mdb_error_t* error, void* user_data) {
    (void)user_data;
    if (error != NULL) {
        syslog(LOG_ERR, "Subscription to %s (%s) failed: %s, aborting", topic, source, error->message);
        abort();
    }
    syslog(LOG_INFO, "Subscribed to %s (source %s)", topic, source);
}

/** The camera sends nothing when the scene is empty, so announce that ourselves. */
static gboolean check_scene_cleared(gpointer user_data) {
    (void)user_data;
    if (++tick % 60 == 0) {
        gint received  = g_atomic_int_exchange(&received_frames, 0);
        gint published = g_atomic_int_exchange(&published_frames, 0);
        syslog(LOG_INFO, "Scene frames in the last minute: %d received, %d published",
               received, published);
    }
    if (dedupe_expired(dedupe, now_ms(), clear_timeout_ms)) {
        GDateTime* now = g_date_time_new_now_utc();
        char* ts       = g_date_time_format_iso8601(now);
        char* json = g_strdup_printf("{\"detections\":[],\"timestamp\":\"%s\",\"synthetic\":true}", ts);
        g_date_time_unref(now);
        mqtt_publish(OBJECTS_TOPIC, json, strlen(json), false);
        g_free(json);
        g_free(ts);
    }
    return G_SOURCE_CONTINUE;
}

bool scene_start(const config_t* cfg) {
    mdb_error_t* error = NULL;

    dedupe           = dedupe_new(cfg->move_threshold, cfg->min_interval_ms, cfg->publish_unclassified);
    topic            = g_strdup(cfg->scene_topic);
    source           = g_strdup(cfg->scene_source);
    clear_timeout_ms = (gint64)cfg->clear_timeout_s * 1000;

    connection = mdb_connection_create(on_connection_error, NULL, &error);
    if (error != NULL)
        goto fail;
    subscriber_config = mdb_subscriber_config_create(topic, source, on_message, NULL, &error);
    if (error != NULL)
        goto fail;
    mdb_subscriber_config_set_on_channel_registered_callback(subscriber_config,
                                                             on_channel_registered, NULL, NULL);
    mdb_subscriber_config_set_on_channel_unregistered_callback(subscriber_config,
                                                               on_channel_unregistered, NULL, NULL);
    subscriber = mdb_subscriber_create_async(connection, subscriber_config,
                                             on_done_subscriber_create, NULL, &error);
    if (error != NULL)
        goto fail;

    clear_timer = g_timeout_add_seconds(1, check_scene_cleared, NULL);
    return true;

fail:
    syslog(LOG_ERR, "Cannot subscribe to scene metadata: %s", error->message);
    mdb_error_destroy(&error);
    scene_stop();
    return false;
}

void scene_stop(void) {
    if (clear_timer != 0) {
        g_source_remove(clear_timer);
        clear_timer = 0;
    }
    mdb_subscriber_destroy(&subscriber);
    mdb_subscriber_config_destroy(&subscriber_config);
    mdb_connection_destroy(&connection);
    dedupe_free(dedupe);
    dedupe = NULL;
    g_free(topic);
    g_free(source);
    topic = source = NULL;
}
