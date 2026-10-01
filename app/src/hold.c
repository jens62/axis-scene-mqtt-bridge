#include "hold.h"

typedef struct {
    hold_t* owner;
    char* sub_topic;
    char* pending_off;  // JSON of the last "off", published when the hold time is up
    guint timer;
    gboolean active;
} episode_t;

struct hold {
    int hold_s;
    hold_publish_fn publish;
    void* user_data;
    GHashTable* episodes;  // sub topic -> episode_t*
};

static void cancel_pending(episode_t* e) {
    if (e->timer != 0) {
        g_source_remove(e->timer);
        e->timer = 0;
    }
    g_free(e->pending_off);
    e->pending_off = NULL;
}

static void episode_free(gpointer p) {
    episode_t* e = p;
    cancel_pending(e);
    g_free(e->sub_topic);
    g_free(e);
}

static gboolean hold_expired(gpointer p) {
    episode_t* e = p;

    e->timer  = 0;  // this source is removed by returning G_SOURCE_REMOVE
    e->active = FALSE;
    e->owner->publish(e->sub_topic, e->pending_off, e->owner->user_data);
    cancel_pending(e);
    return G_SOURCE_REMOVE;
}

hold_t* hold_new(int hold_s, hold_publish_fn publish, void* user_data) {
    hold_t* h   = g_new0(hold_t, 1);
    h->hold_s   = hold_s;
    h->publish  = publish;
    h->user_data = user_data;
    h->episodes = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, episode_free);
    return h;
}

void hold_free(hold_t* h) {
    if (h == NULL)
        return;
    g_hash_table_destroy(h->episodes);
    g_free(h);
}

void hold_event(hold_t* h, const char* sub_topic, const char* json, int state) {
    episode_t* e = g_hash_table_lookup(h->episodes, sub_topic);

    if (e == NULL) {
        e            = g_new0(episode_t, 1);
        e->owner     = h;
        e->sub_topic = g_strdup(sub_topic);
        g_hash_table_insert(h->episodes, e->sub_topic, e);
    }

    if (state == 1) {
        cancel_pending(e);  // the episode goes on, the held "off" is dropped
        if (!e->active) {
            e->active = TRUE;
            h->publish(sub_topic, json, h->user_data);
        }
        return;
    }

    if (!e->active) {
        h->publish(sub_topic, json, h->user_data);
        return;
    }
    cancel_pending(e);
    e->pending_off = g_strdup(json);
    e->timer       = g_timeout_add_seconds((guint)h->hold_s, hold_expired, e);
}
