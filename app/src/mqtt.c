#include "mqtt.h"

#include <mosquitto.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static struct mosquitto* client;
static char prefix[256];
static char status_topic[300];

static void full_topic(char* out, size_t size, const char* subtopic) {
    snprintf(out, size, "%s/%s", prefix, subtopic);
}

static void on_connect(struct mosquitto* mosq, void* user, int rc) {
    (void)user;
    if (rc != 0) {
        syslog(LOG_WARNING, "MQTT connect refused: %s", mosquitto_connack_string(rc));
        return;
    }
    syslog(LOG_INFO, "MQTT connected");
    mosquitto_publish(mosq, NULL, status_topic, 6, "online", 1, true);
}

static void on_disconnect(struct mosquitto* mosq, void* user, int rc) {
    (void)mosq;
    (void)user;
    if (rc != 0)
        syslog(LOG_WARNING, "MQTT connection lost, reconnecting");
}

bool mqtt_start(const config_t* cfg) {
    char client_id[128];
    char host[64] = "unknown";

    gethostname(host, sizeof(host) - 1);
    snprintf(prefix, sizeof(prefix), "%s", cfg->topic_prefix);
    full_topic(status_topic, sizeof(status_topic), "status");
    snprintf(client_id, sizeof(client_id), "%s-%s", APP_NAME, host);

    mosquitto_lib_init();
    client = mosquitto_new(client_id, true, NULL);
    if (client == NULL) {
        syslog(LOG_ERR, "mosquitto_new failed");
        return false;
    }
    if (cfg->mqtt_user[0] != '\0')
        mosquitto_username_pw_set(client, cfg->mqtt_user, cfg->mqtt_password);

    mosquitto_will_set(client, status_topic, 7, "offline", 1, true);
    mosquitto_reconnect_delay_set(client, 1, 30, true);
    mosquitto_connect_callback_set(client, on_connect);
    mosquitto_disconnect_callback_set(client, on_disconnect);

    int rc = mosquitto_connect_async(client, cfg->mqtt_host, cfg->mqtt_port, 30);
    if (rc != MOSQ_ERR_SUCCESS) {
        syslog(LOG_ERR, "MQTT connect to %s:%d failed: %s",
               cfg->mqtt_host, cfg->mqtt_port, mosquitto_strerror(rc));
        return false;
    }
    rc = mosquitto_loop_start(client);
    if (rc != MOSQ_ERR_SUCCESS) {
        syslog(LOG_ERR, "mosquitto_loop_start failed: %s", mosquitto_strerror(rc));
        return false;
    }
    syslog(LOG_INFO, "MQTT client started for %s:%d, topic prefix %s",
           cfg->mqtt_host, cfg->mqtt_port, prefix);
    return true;
}

void mqtt_publish(const char* subtopic, const char* payload, size_t len, bool retain) {
    static unsigned dropped;
    char topic[512];

    if (client == NULL)
        return;
    full_topic(topic, sizeof(topic), subtopic);
    int rc = mosquitto_publish(client, NULL, topic, (int)len, payload, 1, retain);
    if (rc != MOSQ_ERR_SUCCESS && (dropped++ % 100) == 0)
        syslog(LOG_WARNING, "MQTT publish to %s failed (%s), %u dropped so far",
               topic, mosquitto_strerror(rc), dropped);
}

void mqtt_stop(void) {
    if (client == NULL)
        return;
    mosquitto_publish(client, NULL, status_topic, 7, "offline", 1, true);
    mosquitto_disconnect(client);
    mosquitto_loop_stop(client, false);
    mosquitto_destroy(client);
    client = NULL;
    mosquitto_lib_cleanup();
}
