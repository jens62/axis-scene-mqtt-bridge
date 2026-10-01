#include "events.h"

#include <axsdk/axevent.h>
#include <jansson.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "hold.h"
#include "mqtt.h"

#define MAX_TOPIC_LEVELS 4

typedef struct {
    const char* category;  // "audio" or "motion", static string
    gboolean hold;         // merge on/off bursts
    char** keys;           // NULL-terminated list of data/source key names to look for
} subscription_ctx_t;

static AXEventHandler* handler;
static GArray* subscriptions;  // guint ids
static GPtrArray* contexts;    // subscription_ctx_t*
static hold_t* audio_holds;    // on/off merging per category, NULL if disabled
static hold_t* motion_holds;
static gint64 start_us;        // wall clock at subscription time, to recognise replayed states

static void context_free(gpointer p) {
    subscription_ctx_t* ctx = p;
    g_strfreev(ctx->keys);
    g_free(ctx);
}

static json_t* read_value(const AXEventKeyValueSet* kvs, const char* key) {
    AXEventValueType type;

    if (!ax_event_key_value_set_get_value_type(kvs, key, NULL, &type, NULL))
        return NULL;

    switch (type) {
    case AX_VALUE_TYPE_INT: {
        gint v;
        return ax_event_key_value_set_get_integer(kvs, key, NULL, &v, NULL) ? json_integer(v) : NULL;
    }
    case AX_VALUE_TYPE_BOOL: {
        gboolean v;
        return ax_event_key_value_set_get_boolean(kvs, key, NULL, &v, NULL) ? json_boolean(v) : NULL;
    }
    case AX_VALUE_TYPE_DOUBLE: {
        gdouble v;
        return ax_event_key_value_set_get_double(kvs, key, NULL, &v, NULL) ? json_real(v) : NULL;
    }
    case AX_VALUE_TYPE_STRING: {
        gchar* v = NULL;
        if (!ax_event_key_value_set_get_string(kvs, key, NULL, &v, NULL))
            return NULL;
        json_t* j = json_string(v);
        g_free(v);
        return j;
    }
    default:
        return NULL;  // ElementItems are not supported
    }
}

/** Topic levels are stored with a namespace, which the API wants when reading them back. */
static gchar* read_topic_level(const AXEventKeyValueSet* kvs, const char* key) {
    static const char* const namespaces[] = {"tnsaxis", "tns1", NULL};
    gchar* level = NULL;

    for (const char* const* ns = namespaces; *ns != NULL; ns++)
        if (ax_event_key_value_set_get_string(kvs, key, *ns, &level, NULL))
            return level;
    if (ax_event_key_value_set_get_string(kvs, key, NULL, &level, NULL))
        return level;
    return NULL;
}

/** Builds "AudioClassification/Speech" from the keys topic0..topic3. */
static char* read_topic_path(const AXEventKeyValueSet* kvs) {
    GString* path = g_string_new(NULL);

    for (int i = 0; i < MAX_TOPIC_LEVELS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "topic%d", i);
        gchar* level = read_topic_level(kvs, key);
        if (level == NULL)
            break;
        if (path->len > 0)
            g_string_append_c(path, '/');
        g_string_append(path, level);
        g_free(level);
    }
    return g_string_free(path, FALSE);
}

static void publish_json(const char* sub_topic, const char* json, void* user_data) {
    (void)user_data;
    mqtt_publish(sub_topic, json, strlen(json), false);
}

/** 1/0 for events that switch something on/off (Detected, triggered), -1 for all others. */
static int on_off_state(const AXEventKeyValueSet* kvs) {
    static const char* const keys[] = {"Detected", "triggered", "active", "State", "state"};

    for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
        json_t* v = read_value(kvs, keys[i]);
        if (v == NULL)
            continue;
        int state = json_is_true(v) ? 1 : json_is_false(v) ? 0 : json_is_integer(v) ? json_integer_value(v) != 0 : -1;
        json_decref(v);
        return state;
    }
    return -1;
}

static void on_event(guint subscription, AXEvent* event, gpointer user_data) {
    (void)subscription;
    const subscription_ctx_t* ctx   = user_data;
    const AXEventKeyValueSet* kvs   = ax_event_get_key_value_set(event);
    char* topic_path                = read_topic_path(kvs);
    GDateTime* stamp                = ax_event_get_time_stamp2(event);
    GDateTime* utc                  = g_date_time_to_utc(stamp);
    char* time_str                  = g_date_time_format_iso8601(utc);

    json_t* data = json_object();
    for (char** key = ctx->keys; *key != NULL; key++) {
        json_t* value = read_value(kvs, *key);
        if (value != NULL)
            json_object_set_new(data, *key, value);
    }

    json_t* root = json_pack("{s:s, s:s, s:o}", "time", time_str, "topic", topic_path, "data", data);
    // On subscribe the camera replays the current state of stateful events with their old timestamp.
    gint64 event_us = g_date_time_to_unix(stamp) * G_USEC_PER_SEC + g_date_time_get_microsecond(stamp);
    if (event_us < start_us)
        json_object_set_new(root, "initial", json_true());
    char* json   = json_dumps(root, JSON_COMPACT);

    char* sub_topic = g_strdup_printf("%s/%s", ctx->category, topic_path);
    hold_t* holds = strcmp(ctx->category, "audio") == 0 ? audio_holds : motion_holds;
    int state     = (ctx->hold && holds != NULL) ? on_off_state(kvs) : -1;
    if (state >= 0)
        hold_event(holds, sub_topic, json, state);
    else
        publish_json(sub_topic, json, NULL);

    g_free(sub_topic);
    free(json);
    json_decref(root);
    g_free(time_str);
    g_date_time_unref(utc);
    g_date_time_unref(stamp);
    g_free(topic_path);
    ax_event_free(event);
}

/** Subscribes to one topic spec like "tns1:AudioSource/tnsaxis:TriggerLevel". */
static void subscribe_spec(const char* category, const char* spec, const char* keys_csv) {
    AXEventKeyValueSet* kvs = ax_event_key_value_set_new();
    char** levels           = g_strsplit(spec, "/", MAX_TOPIC_LEVELS);
    char ns[32]             = "tnsaxis";
    guint id                = 0;

    for (int i = 0; levels[i] != NULL; i++) {
        char key[16];
        const char* name = levels[i];
        const char* colon = strchr(name, ':');
        if (colon != NULL) {  // a level may carry its own namespace, otherwise inherit
            snprintf(ns, sizeof(ns), "%.*s", (int)(colon - name), name);
            name = colon + 1;
        }
        snprintf(key, sizeof(key), "topic%d", i);
        ax_event_key_value_set_add_key_value(kvs, key, ns, name, AX_VALUE_TYPE_STRING, NULL);
    }

    subscription_ctx_t* ctx = g_new0(subscription_ctx_t, 1);
    ctx->category           = category;
    ctx->hold               = TRUE;
    ctx->keys               = g_strsplit(keys_csv, ",", -1);
    for (char** k = ctx->keys; *k != NULL; k++)
        g_strstrip(*k);
    g_ptr_array_add(contexts, ctx);

    GError* error = NULL;
    if (ax_event_handler_subscribe(handler, kvs, &id, on_event, ctx, &error)) {
        g_array_append_val(subscriptions, id);
        syslog(LOG_INFO, "Subscribed to %s event '%s' (id %u)", category, spec, id);
    } else {
        syslog(LOG_ERR, "Subscription to '%s' failed: %s", spec, error->message);
        g_clear_error(&error);
    }
    g_strfreev(levels);
    ax_event_key_value_set_free(kvs);
}

static void subscribe_list(const char* category, const char* specs_csv, const char* keys_csv) {
    char** specs = g_strsplit(specs_csv, ",", -1);

    for (char** s = specs; *s != NULL; s++) {
        g_strstrip(*s);
        if ((*s)[0] != '\0')
            subscribe_spec(category, *s, keys_csv);
    }
    g_strfreev(specs);
}

bool events_start(const config_t* cfg) {
    start_us      = g_get_real_time();
    audio_holds   = cfg->audio_hold_s > 0 ? hold_new(cfg->audio_hold_s, publish_json, NULL) : NULL;
    motion_holds  = cfg->motion_hold_s > 0 ? hold_new(cfg->motion_hold_s, publish_json, NULL) : NULL;
    handler       = ax_event_handler_new();
    subscriptions = g_array_new(FALSE, FALSE, sizeof(guint));
    contexts      = g_ptr_array_new_with_free_func(context_free);

    if (cfg->publish_audio)
        subscribe_list("audio", cfg->audio_events, cfg->event_keys);
    if (cfg->publish_motion)
        subscribe_list("motion", cfg->motion_events, cfg->event_keys);
    return true;
}

void events_stop(void) {
    if (handler == NULL)
        return;
    for (guint i = 0; i < subscriptions->len; i++)
        ax_event_handler_unsubscribe(handler, g_array_index(subscriptions, guint, i), NULL);
    ax_event_handler_free(handler);
    handler = NULL;
    hold_free(audio_holds);
    hold_free(motion_holds);
    audio_holds = motion_holds = NULL;
    g_array_free(subscriptions, TRUE);
    g_ptr_array_free(contexts, TRUE);
}
