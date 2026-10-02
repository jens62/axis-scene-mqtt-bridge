#include "dedupe.h"

#include <jansson.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* signature;
    double cx;
    double cy;
} track_t;

struct dedupe {
    GMutex lock;
    double move_threshold;
    bool publish_unclassified;
    bool start_stop_only;  // only track appear/disappear (or class type change) counts
    gint64 min_interval_ms;
    GHashTable* published;  // track id -> track_t*, state of the last published frame
    gint64 last_publish_ms;
    gint64 last_objects_ms;  // last frame that contained objects
    char* pending_json;      // newest frame that differs from `published`, held back by the interval
    GHashTable* pending;     // its tracks
};

static void track_free(gpointer p) {
    track_t* t = p;
    free(t->signature);
    free(t);
}

static GHashTable* track_table_new(void) {
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, track_free);
}

static bool is_scored_name(const json_t* obj) {
    return json_is_object(obj) && json_object_get(obj, "name") != NULL &&
           json_object_get(obj, "score") != NULL;
}

/** Returns a new reference: the value without the parts that change on every frame. */
static json_t* canonical(const json_t* value) {
    if (json_is_object(value)) {
        json_t* out = json_object();
        const char* key;
        json_t* child;
        json_object_foreach((json_t*)value, key, child) {
            if (strcmp(key, "score") == 0 || strcmp(key, "timestamp") == 0 ||
                strcmp(key, "bounding_box") == 0)
                continue;
            json_t* c = canonical(child);
            json_object_set_new(out, key, c);
        }
        return out;
    }
    if (json_is_array(value)) {
        json_t* out = json_array();
        size_t n    = json_array_size(value);
        if (n > 0 && is_scored_name(json_array_get(value, 0))) {
            // Sorted by score: only the best entry is stable enough to compare.
            json_array_append_new(out, json_incref(json_object_get(json_array_get(value, 0), "name")));
            return out;
        }
        for (size_t i = 0; i < n; i++)
            json_array_append_new(out, canonical(json_array_get(value, i)));
        return out;
    }
    return json_incref((json_t*)value);
}

static double number_or(const json_t* obj, const char* key, double fallback) {
    const json_t* v = json_object_get(obj, key);
    return json_is_number(v) ? json_number_value(v) : fallback;
}

static track_t* track_from_detection(const json_t* detection, bool type_only) {
    track_t* t = calloc(1, sizeof(*t));
    if (type_only) {
        const json_t* type = json_object_get(json_object_get(detection, "class"), "type");
        t->signature       = strdup(json_is_string(type) ? json_string_value(type) : "");
    } else {
        json_t* c    = canonical(detection);
        t->signature = json_dumps(c, JSON_COMPACT | JSON_SORT_KEYS);
        json_decref(c);
    }

    const json_t* box = json_object_get(detection, "bounding_box");
    if (json_is_object(box)) {
        t->cx = (number_or(box, "left", 0) + number_or(box, "right", 0)) / 2.0;
        t->cy = (number_or(box, "top", 0) + number_or(box, "bottom", 0)) / 2.0;
    }
    return t;
}

static bool is_face(const json_t* object) {
    const json_t* type = json_object_get(json_object_get(object, "class"), "type");
    return json_is_string(type) && strcmp(json_string_value(type), "Face") == 0;
}

/** The list of objects, whichever message format the camera uses. NULL if this is no scene frame. */
static json_t* object_list(json_t* root) {
    json_t* list = json_object_get(root, "detections");
    if (json_is_array(list))
        return list;
    list = json_object_get(json_object_get(root, "frame"), "observations");
    return json_is_array(list) ? list : NULL;
}

static const char* track_id_of(const json_t* object) {
    const json_t* id = json_object_get(object, "object_track_id");
    if (!json_is_string(id))
        id = json_object_get(object, "track_id");
    return json_is_string(id) ? json_string_value(id) : NULL;
}

dedupe_t* dedupe_new(double move_threshold, int min_interval_ms, bool publish_unclassified) {
    dedupe_t* d             = calloc(1, sizeof(*d));
    d->move_threshold       = move_threshold;
    d->publish_unclassified = publish_unclassified;
    d->min_interval_ms      = min_interval_ms;
    d->published            = track_table_new();
    d->last_publish_ms      = G_MININT64 / 2;  // the first change is never held back
    g_mutex_init(&d->lock);
    return d;
}

void dedupe_set_start_stop_only(dedupe_t* d, bool on) {
    d->start_stop_only = on;
}

static void clear_pending(dedupe_t* d) {
    free(d->pending_json);
    d->pending_json = NULL;
    if (d->pending != NULL) {
        g_hash_table_destroy(d->pending);
        d->pending = NULL;
    }
}

void dedupe_free(dedupe_t* d) {
    if (d == NULL)
        return;
    clear_pending(d);
    g_hash_table_destroy(d->published);
    g_mutex_clear(&d->lock);
    free(d);
}

/** Does `current` differ from `published` in a way that is worth a message? */
static bool differs(const dedupe_t* d, GHashTable* current) {
    if (g_hash_table_size(current) != g_hash_table_size(d->published))
        return true;

    GHashTableIter it;
    gpointer k, v;
    bool moved = false;
    g_hash_table_iter_init(&it, current);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const track_t* cur  = v;
        const track_t* prev = g_hash_table_lookup(d->published, k);
        if (prev == NULL || strcmp(prev->signature, cur->signature) != 0)
            return true;
        if (!d->start_stop_only && hypot(cur->cx - prev->cx, cur->cy - prev->cy) > d->move_threshold)
            moved = true;
    }
    return moved;
}

bool dedupe_check(dedupe_t* d, const char* data, size_t len, gint64 now_ms) {
    json_error_t err;
    json_t* root = json_loadb(data, len, 0, &err);
    if (root == NULL)
        return true;  // not JSON we understand: never swallow it

    json_t* detections = object_list(root);
    if (detections == NULL) {
        json_decref(root);
        return true;
    }

    GHashTable* current = track_table_new();
    size_t i;
    json_t* det;
    json_array_foreach(detections, i, det) {
        if (is_face(det))
            continue;
        if (!d->publish_unclassified && json_object_get(det, "class") == NULL)
            continue;
        const char* id = track_id_of(det);
        char fallback[32];
        snprintf(fallback, sizeof(fallback), "#%zu", i);
        char* key = g_strdup(id != NULL ? id : fallback);
        g_hash_table_replace(current, key, track_from_detection(det, d->start_stop_only));
    }
    json_decref(root);

    g_mutex_lock(&d->lock);

    bool has_objects = g_hash_table_size(current) > 0;
    bool publish     = false;

    if (!differs(d, current)) {
        clear_pending(d);  // back to the published state: nothing left to tell
    } else if (now_ms - d->last_publish_ms >= d->min_interval_ms) {
        publish = true;
    } else {
        clear_pending(d);  // keep only the newest held-back frame
        d->pending_json = strndup(data, len);
        d->pending      = current;
        current         = NULL;
    }

    if (publish) {
        clear_pending(d);
        g_hash_table_destroy(d->published);
        d->published       = current;
        d->last_publish_ms = now_ms;
        current            = NULL;
    }
    if (has_objects)
        d->last_objects_ms = now_ms;

    g_mutex_unlock(&d->lock);
    if (current != NULL)
        g_hash_table_destroy(current);
    return publish;
}

char* dedupe_take_pending(dedupe_t* d, gint64 now_ms) {
    char* out = NULL;

    g_mutex_lock(&d->lock);
    if (d->pending_json != NULL && now_ms - d->last_publish_ms >= d->min_interval_ms) {
        out = g_strdup(d->pending_json);
        g_hash_table_destroy(d->published);
        d->published = d->pending;
        d->pending   = NULL;
        clear_pending(d);
        d->last_publish_ms = now_ms;
        if (g_hash_table_size(d->published) > 0)
            d->last_objects_ms = now_ms;
    }
    g_mutex_unlock(&d->lock);
    return out;
}

bool dedupe_expired(dedupe_t* d, gint64 now_ms, gint64 clear_timeout_ms) {
    bool expired = false;

    g_mutex_lock(&d->lock);
    if (g_hash_table_size(d->published) > 0 && now_ms - d->last_objects_ms >= clear_timeout_ms) {
        g_hash_table_remove_all(d->published);
        clear_pending(d);
        d->last_publish_ms = now_ms;
        expired            = true;
    }
    g_mutex_unlock(&d->lock);
    return expired;
}
