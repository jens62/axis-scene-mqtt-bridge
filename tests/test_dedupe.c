// Host-side test of the frame filter: see tests/run.sh
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../app/src/dedupe.h"

static bool check(dedupe_t* d, const char* json, long ms) {
    return dedupe_check(d, json, strlen(json), ms);
}

/* ---- com.axis.scene.frame.v1 ("detections"), no interval ---- */

static char* detection(const char* ts, const char* id, double left, double score, const char* color,
                       const char* type) {
    static char buf[4][1024];
    static int n;
    char* b = buf[n++ % 4];
    snprintf(b, 1024,
             "{\"channel_id\":1,\"detections\":[{\"bounding_box\":{\"bottom\":0.6,\"left\":%.4f,"
             "\"right\":%.4f,\"top\":0.3},\"class\":{\"score\":%.2f,\"type\":\"%s\","
             "\"upper_clothing_colors\":[{\"name\":\"%s\",\"score\":%.2f},{\"name\":\"red\","
             "\"score\":0.1}]},\"object_track_id\":\"%s\"}],\"timestamp\":\"%s\"}",
             left, left + 0.1, score, type, color, score, id, ts);
    return b;
}

static void test_frame_v1(void) {
    dedupe_t* d = dedupe_new(0.05, 0, true);

    assert(check(d, detection("t1", "A", 0.10, 0.54, "green", "Human"), 0));
    assert(!check(d, detection("t2", "A", 0.10, 0.50, "green", "Human"), 100));   // score jitter
    assert(!check(d, detection("t3", "A", 0.12, 0.54, "green", "Human"), 200));   // small move
    assert(check(d, detection("t4", "A", 0.30, 0.54, "green", "Human"), 300));    // big move
    assert(check(d, detection("t5", "A", 0.30, 0.54, "blue", "Human"), 400));     // colour
    assert(check(d, detection("t6", "A", 0.30, 0.54, "blue", "Vehicle"), 500));   // class
    assert(check(d, detection("t7", "B", 0.30, 0.54, "blue", "Vehicle"), 600));   // new track
    const char* empty = "{\"channel_id\":1,\"detections\":[],\"timestamp\":\"t8\"}";
    assert(check(d, empty, 700));
    assert(!check(d, empty, 800));
    assert(check(d, "garbage", 900));            // never swallow what we do not understand
    assert(check(d, "{\"foo\":1}", 1000));

    assert(check(d, detection("t9", "C", 0.10, 0.54, "green", "Human"), 2000));
    assert(!dedupe_expired(d, 3000, 3000));
    assert(dedupe_expired(d, 5000, 3000));
    assert(!dedupe_expired(d, 6000, 3000));
    assert(check(d, detection("t10", "C", 0.10, 0.54, "green", "Human"), 7000));
    dedupe_free(d);
}

/* ---- com.axis.analytics_scene_description.v0.beta ("frame.observations"), shapes from a capture ---- */

enum { UNCLASSIFIED = 1, HUMAN = 2, FACE = 4 };

static char* obs(const char* ts, int parts, double left, const char* upper, double score) {
    static char buf[4][2048];
    static int n;
    char* b = buf[n++ % 4];
    int len = snprintf(b, 2048, "{\"frame\":{\"observations\":[");
    const char* sep = "";
    if (parts & UNCLASSIFIED) {
        len += snprintf(b + len, 2048 - len,
                        "{\"bounding_box\":{\"bottom\":0.5797,\"left\":0.9777,\"right\":0.9872,"
                        "\"top\":0.5677},\"timestamp\":\"%s\",\"track_id\":\"u1\"}", ts);
        sep = ",";
    }
    if (parts & HUMAN) {
        len += snprintf(b + len, 2048 - len,
                        "%s{\"bounding_box\":{\"bottom\":0.5763,\"left\":%.4f,\"right\":%.4f,"
                        "\"top\":0.2213},\"class\":{\"lower_clothing_colors\":[{\"name\":\"Blue\","
                        "\"score\":%.2f}],\"score\":%.2f,\"type\":\"Human\",\"upper_clothing_colors\":"
                        "[{\"name\":\"%s\",\"score\":%.2f}]},\"timestamp\":\"%s\",\"track_id\":\"h1\"}",
                        sep, left, left + 0.07, score, score, upper, score, ts);
        sep = ",";
    }
    if (parts & FACE) {
        len += snprintf(b + len, 2048 - len,
                        "%s{\"bounding_box\":{\"bottom\":0.3787,\"left\":0.2636,\"right\":0.2962,"
                        "\"top\":0.2961},\"class\":{\"score\":%.2f,\"type\":\"Face\"},"
                        "\"timestamp\":\"%s\",\"track_id\":\"f1\"}", sep, score, ts);
    }
    snprintf(b + len, 2048 - len, "],\"operations\":[],\"timestamp\":\"%s\"}}", ts);
    return b;
}

static void test_observations(void) {
    dedupe_t* d = dedupe_new(0.15, 3000, false);

    // Unclassified noise only: nothing to report.
    assert(!check(d, obs("t0", UNCLASSIFIED, 0, "", 0), 0));
    // The person gets classified: the first message is never held back.
    assert(check(d, obs("t1", UNCLASSIFIED | HUMAN, 0.13, "Green", 0.82), 100));
    // 10 Hz stream with jitter and slow drift for 2.9 s: nothing.
    int published = 0;
    for (int i = 0; i < 29; i++)
        published += check(d, obs("t", UNCLASSIFIED | HUMAN, 0.13 + 0.001 * i, "Green", 0.82 - 0.005 * i), 200 + 100 * i);
    assert(published == 0);
    // A face flickers in and out: never a change by itself.
    assert(!check(d, obs("t2", HUMAN | FACE, 0.16, "Green", 0.80), 3100));
    assert(!check(d, obs("t3", HUMAN, 0.16, "Green", 0.80), 3200));
    assert(!check(d, obs("t4", HUMAN | FACE, 0.16, "Green", 0.70), 3300));
    assert(dedupe_take_pending(d, 3400) == NULL);

    // The colour changes inside the interval of the last message (t=100 -> 3100 is over: publishes).
    assert(check(d, obs("t5", HUMAN | FACE, 0.16, "Beige", 0.70), 3400));
    // Another change right after it is held back, and the newest one wins.
    assert(!check(d, obs("t6", HUMAN | FACE, 0.60, "Beige", 0.70), 4000));   // moved far
    assert(!check(d, obs("t7", HUMAN | FACE, 0.62, "Beige", 0.70), 4500));
    assert(dedupe_take_pending(d, 5000) == NULL);                            // interval not over
    char* p = dedupe_take_pending(d, 6400);
    assert(p != NULL && strstr(p, "\"t7\"") != NULL);
    g_free(p);
    assert(dedupe_take_pending(d, 6500) == NULL);                            // handed out once

    // The person leaves inside the interval: not lost, handed out when the interval is over.
    assert(!check(d, obs("t8", UNCLASSIFIED, 0, "", 0), 7000));
    assert(dedupe_take_pending(d, 9000) == NULL);   // last message went out at 6400, due at 9400
    p = dedupe_take_pending(d, 9400);
    assert(p != NULL && strstr(p, "\"t8\"") != NULL);
    g_free(p);
    // Quiet again.
    assert(!check(d, obs("t9", UNCLASSIFIED, 0, "", 0), 10000));
    assert(dedupe_take_pending(d, 10500) == NULL);

    // A change that is undone inside the interval leaves nothing pending.
    assert(check(d, obs("t10", HUMAN, 0.20, "Green", 0.8), 20000));
    assert(!check(d, obs("t11", HUMAN, 0.20, "Beige", 0.8), 20500));
    assert(!check(d, obs("t12", HUMAN, 0.20, "Green", 0.8), 21000));
    assert(dedupe_take_pending(d, 30000) == NULL);
    dedupe_free(d);

    // publish_unclassified: the fresh track counts.
    d = dedupe_new(0.15, 3000, true);
    assert(check(d, obs("t0", UNCLASSIFIED, 0, "", 0), 0));
    assert(!check(d, obs("t1", UNCLASSIFIED, 0, "", 0), 100));
    dedupe_free(d);
}

/* ---- start/stop only: movement and flickering clothing colours are ignored ---- */

static void test_start_stop(void) {
    dedupe_t* d = dedupe_new(0.05, 1000, false);
    dedupe_set_start_stop_only(d, true);

    assert(check(d, obs("t0", HUMAN, 0.10, "Black", 0.6), 0));          // appears
    assert(!check(d, obs("t1", HUMAN, 0.60, "Black", 0.6), 2000));     // moved a lot
    assert(!check(d, obs("t2", HUMAN, 0.60, "Blue", 0.5), 4000));      // colour flips
    assert(!check(d, obs("t3", HUMAN | FACE, 0.30, "Black", 0.4), 6000));
    assert(dedupe_take_pending(d, 20000) == NULL);

    assert(!dedupe_expired(d, 8000, 3000));                              // 2 s since last object
    assert(dedupe_expired(d, 20000, 3000));                              // gone
    assert(check(d, obs("t4", HUMAN, 0.10, "Blue", 0.6), 21000));       // appears again
    dedupe_free(d);
}

/* Sequence from a camera: the classifier drops out for single frames of a tracked person. */
static char* same_track(const char* ts, bool classified) {
    static char buf[2][1024];
    static int n;
    char* b = buf[n++ % 2];
    snprintf(b, 1024,
             "{\"frame\":{\"observations\":[{\"bounding_box\":{\"bottom\":0.6,\"left\":0.38,"
             "\"right\":0.40,\"top\":0.5},%s\"timestamp\":\"%s\",\"track_id\":\"p1\"}],"
             "\"operations\":[],\"timestamp\":\"%s\"}}",
             classified ? "\"class\":{\"score\":0.7,\"type\":\"Human\"}," : "", ts, ts);
    return b;
}

static void test_start_stop_classifier_dropout(void) {
    dedupe_t* d = dedupe_new(0.05, 1000, false);
    dedupe_set_start_stop_only(d, true);

    assert(!check(d, same_track("t0", false), 0));        // not classified yet: nothing to report
    assert(check(d, same_track("t1", true), 1000));       // start
    assert(!check(d, same_track("t2", false), 2000));     // drop-out
    assert(!check(d, same_track("t3", true), 3000));
    assert(!check(d, same_track("t4", false), 4000));
    assert(!dedupe_expired(d, 6000, 3000));               // drop-out frames keep the scene alive
    assert(dedupe_expired(d, 7100, 3000));                // 3.1 s after the last frame
    dedupe_free(d);
}

int main(void) {
    test_frame_v1();
    test_observations();
    test_start_stop();
    test_start_stop_classifier_dropout();
    puts("dedupe: all tests passed");
    return 0;
}
