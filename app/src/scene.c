#include "scene.h"

#include <datahub/client.h>
#include <datahub/subscriber.h>
#include <mdb/connection.h>
#include <mdb/error.h>
#include <mdb/subscriber.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "dedupe.h"
#include "mqtt.h"

#define OBJECTS_TOPIC "objects"

static mdb_connection_t* connection;
static mdb_subscriber_config_t* subscriber_config;
static mdb_subscriber_t* subscriber;
static DHClient* dh_client;
static DHSubscriber* dh_subscriber;
static volatile gint dh_connected;
static dedupe_t* dedupe;
static char* topic;
static char* source;
static guint clear_timer;
static gint received_frames;   // all frames from the camera since the last stats line
static gint published_frames;  // frames that passed the de-duplication
static guint tick;
static gint first_frame_seen;
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

/** Publishes a frame; an idle frame gets an explicit empty "detections" list. */
static void publish_frame(const char* data, size_t size) {
    char* normalized = dedupe_normalize(data, size);
    if (normalized != NULL) {
        mqtt_publish(OBJECTS_TOPIC, normalized, strlen(normalized), false);
        g_free(normalized);
    } else {
        mqtt_publish(OBJECTS_TOPIC, data, size, false);
    }
}

/** One frame from the camera, whichever transport it came by. Called from the transport's thread. */
static void on_frame(const char* data, size_t size) {
    g_atomic_int_inc(&received_frames);
    if (g_atomic_int_compare_and_exchange(&first_frame_seen, 0, 1))
        syslog(LOG_INFO, "First scene frame received");
    if (dedupe_check(dedupe, data, size, now_ms())) {
        g_atomic_int_inc(&published_frames);
        publish_frame(data, size);
    }
}

static void on_message(const mdb_message_t* message, void* user_data) {
    (void)user_data;
    const mdb_message_payload_t* payload = mdb_message_get_payload(message);
    on_frame((const char*)payload->data, payload->size);
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
    char* pending = dedupe_take_pending(dedupe, now_ms());
    if (pending != NULL) {
        g_atomic_int_inc(&published_frames);
        publish_frame(pending, strlen(pending));
        g_free(pending);
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

/* ---- Device Data Hub ---- */

static bool dh_failed(DHError* err, const char* context) {
    if (err == NULL)
        return false;
    syslog(LOG_ERR, "Device Data Hub: %s failed: %s", context, dh_error_to_string(err));
    dh_error_destroy(err);
    return true;
}

static void on_dh_sample(const DHTopicSample* sample, void* user_data) {
    (void)user_data;
    const char* json = dh_topic_data_get_json_data(dh_topic_sample_get_data(sample));
    if (json != NULL)
        on_frame(json, strlen(json));
}

static void on_dh_connection(DHConnectionState state, void* user_data) {
    (void)user_data;
    if (state == DH_CONN_CONNECTED) {
        g_atomic_int_set(&dh_connected, 1);
    } else if (state == DH_CONN_DISCONNECTED && g_atomic_int_get(&dh_connected)) {
        // Like the Message Broker connection: let the ACAP framework respawn us.
        syslog(LOG_ERR, "Device Data Hub connection lost, aborting");
        abort();
    }
}

static bool dh_start(void) {
    DHError* err = NULL;

    dh_client = dh_client_create("axis-scene-mqtt-bridge", &err);
    if (dh_client == NULL) {
        dh_failed(err, "create client");
        return false;
    }
    dh_client_set_logging(dh_client, DH_LOG_WARNING, DH_LOG_TARGET_SYSLOG, &err);
    dh_failed(err, "set logging");
    err = NULL;
    dh_client_set_connection_update_callback(dh_client, on_dh_connection, NULL, &err);
    if (dh_failed(err, "set connection callback"))
        return false;
    dh_client_connect(dh_client, &err);
    if (dh_failed(err, "connect"))
        return false;

    dh_subscriber = dh_client_create_subscriber(dh_client, "scene frames", &err);
    if (dh_failed(err, "create subscriber"))
        return false;
    dh_subscriber_set_data_callback(dh_subscriber, on_dh_sample, NULL, &err);
    if (dh_failed(err, "set data callback"))
        return false;

    DHFilter* filter = dh_filter_create();
    dh_filter_add_topic_name(filter, topic, &err);
    if (dh_failed(err, "add topic to filter")) {
        dh_filter_destroy(filter);
        return false;
    }
    // The source is the channel number; a source that is not a number subscribes to all channels.
    char* end = NULL;
    long channel = strtol(source, &end, 10);
    if (end != source && *end == '\0') {
        DHInstanceKeys* keys = dh_instance_keys_create();
        bool failed = false;
        dh_instance_keys_add_integer(keys, "channel_id", channel, &err);
        if (dh_failed(err, "add channel_id")) {
            failed = true;
        } else {
            err = NULL;
            dh_filter_add_instance(filter, keys, &err);
            failed = dh_failed(err, "add instance filter");
        }
        dh_instance_keys_destroy(keys);
        if (failed) {
            dh_filter_destroy(filter);
            return false;
        }
    }
    DHSubscribeOptions* options = dh_subscribe_options_create();
    dh_subscribe_options_add_filter(options, filter, &err);
    dh_filter_destroy(filter);
    if (dh_failed(err, "add filter")) {
        dh_subscribe_options_destroy(options);
        return false;
    }
    dh_subscribe_options_set_enable_data_updates(options, true);
    dh_subscriber_subscribe(dh_subscriber, options, &err);
    dh_subscribe_options_destroy(options);
    if (dh_failed(err, "subscribe"))
        return false;

    syslog(LOG_INFO, "Subscribed to %s (channel %s) through Device Data Hub", topic, source);
    return true;
}

static void dh_stop(void) {
    if (dh_subscriber != NULL) {
        dh_subscriber_destroy(dh_subscriber);
        dh_subscriber = NULL;
    }
    if (dh_client != NULL) {
        g_atomic_int_set(&dh_connected, 0);  // a deliberate disconnect is no reason to abort
        DHError* err = NULL;
        dh_client_disconnect(dh_client, &err);
        dh_failed(err, "disconnect");
        dh_client_destroy(dh_client);
        dh_client = NULL;
    }
}

/* ---- Message Broker (deprecated, removed in AXIS OS 13) ---- */

static bool mdb_start(void) {
    mdb_error_t* error = NULL;

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
    return true;

fail:
    syslog(LOG_ERR, "Cannot subscribe to scene metadata: %s", error->message);
    mdb_error_destroy(&error);
    return false;
}

bool scene_start(const config_t* cfg) {
    dedupe           = dedupe_new(cfg->move_threshold, cfg->min_interval_ms, cfg->publish_unclassified);
    dedupe_set_start_stop_only(dedupe, cfg->objects_start_stop);
    topic            = g_strdup(cfg->scene_topic);
    source           = g_strdup(cfg->scene_source);
    clear_timeout_ms = (gint64)cfg->clear_timeout_s * 1000;

    bool ok = strcmp(cfg->scene_transport, "messagebroker") == 0 ? mdb_start() : dh_start();
    if (!ok) {
        scene_stop();
        return false;
    }
    clear_timer = g_timeout_add_seconds(1, check_scene_cleared, NULL);
    return true;
}

void scene_stop(void) {
    if (clear_timer != 0) {
        g_source_remove(clear_timer);
        clear_timer = 0;
    }
    dh_stop();
    mdb_subscriber_destroy(&subscriber);
    mdb_subscriber_config_destroy(&subscriber_config);
    mdb_connection_destroy(&connection);
    dedupe_free(dedupe);
    dedupe = NULL;
    g_free(topic);
    g_free(source);
    topic = source = NULL;
}
