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

int main(void) {
    dedupe_t* d = dedupe_new(0.05, 1000);

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
    puts("dedupe: all tests passed");
    return 0;
}
