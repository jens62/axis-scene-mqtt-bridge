// Host-side test of the frame de-duplication: see tests/run.sh
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../app/src/dedupe.h"

static char* frame(const char* ts, const char* id, double left, double score, const char* color,
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

static bool check(dedupe_t* d, const char* json, long ms) {
    return dedupe_check(d, json, strlen(json), ms);
}

// com.axis.analytics_scene_description.v0.beta, shapes taken from a capture of the M4228-LVE.
static char* obs(const char* ts, const char* human_id, double left, const char* upper, double score,
                 bool with_face, bool with_unclassified) {
    static char buf[4][2048];
    static int n;
    char* b = buf[n++ % 4];
    int len = snprintf(b, 2048, "{\"frame\":{\"observations\":[");
    if (with_unclassified)
        len += snprintf(b + len, 2048 - len,
                        "{\"bounding_box\":{\"bottom\":0.5797,\"left\":0.9777,\"right\":0.9872,"
                        "\"top\":0.5677},\"timestamp\":\"%s\",\"track_id\":\"u1\"}", ts);
    if (human_id != NULL)
        len += snprintf(b + len, 2048 - len,
                        "%s{\"bounding_box\":{\"bottom\":0.5763,\"left\":%.4f,\"right\":%.4f,"
                        "\"top\":0.2213},\"class\":{\"lower_clothing_colors\":[{\"name\":\"Blue\","
                        "\"score\":%.2f}],\"score\":%.2f,\"type\":\"Human\",\"upper_clothing_colors\":"
                        "[{\"name\":\"%s\",\"score\":%.2f}]},\"timestamp\":\"%s\",\"track_id\":\"%s\"}",
                        with_unclassified ? "," : "", left, left + 0.07, score, score, upper, score, ts,
                        human_id);
    if (with_face)
        len += snprintf(b + len, 2048 - len,
                        "%s{\"bounding_box\":{\"bottom\":0.3787,\"left\":0.2636,\"right\":0.2962,"
                        "\"top\":0.2961},\"class\":{\"score\":%.2f,\"type\":\"Face\"},"
                        "\"timestamp\":\"%s\",\"track_id\":\"f1\"}",
                        (with_unclassified || human_id != NULL) ? "," : "", score, ts);
    snprintf(b + len, 2048 - len, "],\"operations\":[],\"timestamp\":\"%s\"}}", ts);
    return b;
}

static void test_observations(void) {
    dedupe_t* d = dedupe_new(0.05, 1000, false);

    // Only an unclassified fresh track: nothing worth reporting.
    assert(!check(d, obs("t0", NULL, 0, "", 0, false, true), 0));
    // The person gets classified (unclassified track still present) -> publish.
    assert(check(d, obs("t1", "h1", 0.13, "Green", 0.82, false, true), 100));
    // 10 Hz stream: score jitter, small moves, a face appearing as extra object.
    long ms = 200;
    int published = 0;
    for (int i = 0; i < 30; i++, ms += 100)
        published += check(d, obs("t", "h1", 0.13 + 0.001 * i, "Green", 0.82 - 0.005 * i, false, true), ms);
    assert(published == 0);
    // A face shows up (new classified track) -> publish, then its score jitter is suppressed.
    assert(check(d, obs("t2", "h1", 0.16, "Green", 0.80, true, true), ms));
    assert(!check(d, obs("t3", "h1", 0.16, "Green", 0.70, true, true), ms + 100));
    // Upper clothing colour changes -> publish immediately.
    assert(check(d, obs("t4", "h1", 0.16, "Beige", 0.70, true, true), ms + 200));
    // Person walks away: moved far, after the min interval -> publish; before it -> suppressed.
    assert(!check(d, obs("t5", "h1", 0.50, "Beige", 0.70, true, true), ms + 300));
    assert(check(d, obs("t6", "h1", 0.50, "Beige", 0.70, true, true), ms + 1300));
    // Everyone gone (only the unclassified track left) -> one publish, then quiet.
    assert(check(d, obs("t7", NULL, 0, "", 0, false, true), ms + 1400));
    assert(!check(d, obs("t8", NULL, 0, "", 0, false, true), ms + 1500));
    dedupe_free(d);

    // With publish_unclassified the fresh track counts.
    d = dedupe_new(0.05, 1000, true);
    assert(check(d, obs("t0", NULL, 0, "", 0, false, true), 0));
    assert(!check(d, obs("t1", NULL, 0, "", 0, false, true), 100));
    dedupe_free(d);
}

int main(void) {
    dedupe_t* d = dedupe_new(0.05, 1000, true);

    // New track -> publish.
    assert(check(d, frame("t1", "A", 0.10, 0.54, "green", "Human"), 0));
    // Only timestamp and score jitter -> suppressed.
    assert(!check(d, frame("t2", "A", 0.10, 0.50, "green", "Human"), 100));
    // Small move below threshold -> suppressed.
    assert(!check(d, frame("t3", "A", 0.12, 0.54, "green", "Human"), 200));
    // Big move but within min interval -> suppressed.
    assert(!check(d, frame("t4", "A", 0.30, 0.54, "green", "Human"), 500));
    // Big move after min interval -> published.
    assert(check(d, frame("t5", "A", 0.30, 0.54, "green", "Human"), 1200));
    // Best color changes -> published immediately.
    assert(check(d, frame("t6", "A", 0.30, 0.54, "blue", "Human"), 1300));
    // Class changes -> published.
    assert(check(d, frame("t7", "A", 0.30, 0.54, "blue", "Vehicle"), 1400));
    // New track id -> published.
    assert(check(d, frame("t8", "B", 0.30, 0.54, "blue", "Vehicle"), 1500));
    // Empty scene frame -> published once, then suppressed.
    const char* empty = "{\"channel_id\":1,\"detections\":[],\"timestamp\":\"t9\"}";
    assert(check(d, empty, 1600));
    assert(!check(d, empty, 1700));
    // Not JSON / not a scene -> never swallowed.
    assert(check(d, "garbage", 1800));
    assert(check(d, "{\"foo\":1}", 1900));

    // Timeout: objects published, then no frames -> expired exactly once.
    assert(check(d, frame("t10", "C", 0.10, 0.54, "green", "Human"), 2000));
    assert(!dedupe_expired(d, 3000, 3000));
    assert(dedupe_expired(d, 5000, 3000));
    assert(!dedupe_expired(d, 6000, 3000));
    // Same object again after the clear is a new appearance -> published.
    assert(check(d, frame("t11", "C", 0.10, 0.54, "green", "Human"), 7000));

    dedupe_free(d);
    test_observations();
    puts("dedupe: all tests passed");
    return 0;
}
