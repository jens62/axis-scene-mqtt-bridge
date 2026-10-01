// Host-side test of the audio hold time, see tests/run.sh
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../app/src/hold.h"

static GPtrArray* sent;

static void record(const char* topic, const char* json, void* user_data) {
    (void)user_data;
    g_ptr_array_add(sent, g_strdup_printf("%s %s", topic, json));
}

static void run_for_ms(int ms) {
    for (int i = 0; i < ms / 10; i++) {
        while (g_main_context_iteration(NULL, FALSE))
            ;
        g_usleep(10 * 1000);
    }
}

static void expect(guint count, const char* last) {
    assert(sent->len == count);
    if (last != NULL)
        assert(strcmp(g_ptr_array_index(sent, count - 1), last) == 0);
}

int main(void) {
    sent        = g_ptr_array_new_with_free_func(g_free);
    hold_t* h   = hold_new(1, record, NULL);

    // Replayed start-up state "off" while no episode runs: passes at once.
    hold_event(h, "audio/Speech", "off0", 0);
    expect(1, "audio/Speech off0");

    // A burst train: on, off, on, off, on, off within the hold time.
    hold_event(h, "audio/Speech", "on1", 1);
    expect(2, "audio/Speech on1");
    hold_event(h, "audio/Speech", "off1", 0);
    expect(2, NULL);
    run_for_ms(300);
    hold_event(h, "audio/Speech", "on2", 1);   // swallowed, cancels the held off
    hold_event(h, "audio/Speech", "off2", 0);
    run_for_ms(300);
    hold_event(h, "audio/Speech", "on3", 1);   // swallowed
    hold_event(h, "audio/Speech", "off3", 0);
    expect(2, NULL);

    // Another topic is independent.
    hold_event(h, "audio/Shout", "shout_on", 1);
    expect(3, "audio/Shout shout_on");

    // Quiet for longer than the hold time: only the last off is published.
    run_for_ms(1300);
    expect(4, "audio/Speech off3");

    // A new episode starts again with an immediate "on".
    hold_event(h, "audio/Speech", "on4", 1);
    expect(5, "audio/Speech on4");

    // Freeing with a pending timer must not crash or fire.
    hold_event(h, "audio/Speech", "off4", 0);
    hold_free(h);
    run_for_ms(1300);
    expect(5, NULL);

    g_ptr_array_free(sent, TRUE);
    puts("hold: all tests passed");
    return 0;
}
