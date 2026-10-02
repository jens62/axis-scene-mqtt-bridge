// Replays "<seconds> <json>" lines through the frame filter: replay.c < capture.txt
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../app/src/dedupe.h"

int main(int argc, char** argv) {
    double thr = argc > 1 ? atof(argv[1]) : 0.05;
    int interval = argc > 2 ? atoi(argv[2]) : 1000;
    int start_stop = argc > 3 ? atoi(argv[3]) : 0;
    long clear_ms = argc > 4 ? atol(argv[4]) : 3000;
    dedupe_t* d = dedupe_new(thr, interval, false);
    dedupe_set_start_stop_only(d, start_stop);
    long last = 0;
    static char line[65536];
    while (fgets(line, sizeof line, stdin)) {
        char* json = strchr(line, '{');
        if (!json) continue;
        long ms;
        if (line[0] == 'T') {  // T<hh:mm:ss.fff>
            int h, m; double sec;
            sscanf(line + 1, "%d:%d:%lf", &h, &m, &sec);
            ms = (long)(((h * 60 + m) * 60 + sec) * 1000);
        } else
            ms = atol(line) * 1000;
        while (last && dedupe_expired(d, ms, clear_ms))
            printf("t=%.1fs EMPTY (cleared)\n", ms / 1000.0);
        last = ms;
        int pub = dedupe_check(d, json, strlen(json), ms);
        printf("t=%.1fs %s\n", ms / 1000.0, pub ? "PUBLISH" : "drop");
    }
    if (dedupe_expired(d, last + clear_ms, clear_ms))
        printf("t=%.1fs EMPTY (cleared after last frame)\n", (last + clear_ms) / 1000.0);
    dedupe_free(d);
    return 0;
}
