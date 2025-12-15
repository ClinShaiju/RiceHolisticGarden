#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "src/moisture.h"

static pthread_t server_thread;
static int server_running = 0;

static void *server_thread_fn(void *arg) {
    (void)arg;
    int sockfd = -1;
    struct sockaddr_in addr;
    char buf[256];
    /* Simple MAC->IP mapping */
    struct mac_ip_map_entry { char mac[32]; struct sockaddr_in addr; } maps[32];
    int maps_count = 0;
    /* We forward readings immediately to the moisture module (no batching)
     * so the UI shows the last received value as soon as a packet arrives.
     */

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("server: socket");
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(12345); /* default port */

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("server: bind");
        close(sockfd);
        return NULL;
    }

    server_running = 1;

    while (server_running) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&src, &src_len);
        if (n <= 0) {
            if (n == -1 && errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';
        /* Log incoming packet for debugging (timestamp, src ip:port, payload) */
        {
            char timestr[64];
            time_t t = time(NULL);
            struct tm lt;
            localtime_r(&t, &lt);
            strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &lt);
            char srcip[64]; inet_ntop(AF_INET, &src.sin_addr, srcip, sizeof(srcip));
            int srcport = ntohs(src.sin_port);
            FILE *lf = fopen("/tmp/server.log", "a");
            if (lf) {
                fprintf(lf, "%s %s:%d %s\n", timestr, srcip, srcport, buf);
                fclose(lf);
            }
        }

        /* Expect messages like: "SENSOR <mac> <moisture>", "<mac> <moisture>", or "aa:bb:... ,moist" */
        char mac[32] = {0};
        float moistf = -1.0f;
        /* Accept a simple space-separated "<mac> <float>" (common and convenient) */
        if (sscanf(buf, "%31s %f", mac, &moistf) == 2) {
            /* Accept float voltage reading (e.g. 0.0 - 3.3) */
            if (moistf >= 0.0f && moistf <= 5.0f) {
                /* Forward single reading immediately */
                sensor_reading_t r;
                strncpy(r.mac, mac, sizeof(r.mac)-1);
                r.mac[sizeof(r.mac)-1] = '\0';
                r.moisture = moistf;
                moisture_receive_sensor_values(&r, 1);
                /* store mapping mac->ip */
                if (maps_count < (int)(sizeof(maps)/sizeof(maps[0]))) {
                    int found = 0;
                    for (int i=0;i<maps_count;i++) if (strcmp(maps[i].mac, mac) == 0) { maps[i].addr = src; found = 1; break; }
                    if (!found) { strncpy(maps[maps_count].mac, mac, sizeof(maps[maps_count].mac)-1); maps[maps_count].addr = src; maps_count++; }
                }
                /* No batching: already forwarded above */
            }
        }
        else {
            /* Try CSV "mac,moist" */
            if (sscanf(buf, "%31[^,],%f", mac, &moistf) == 2) {
                    if (moistf >= 0.0f && moistf <= 5.0f) {
                    /* Forward single reading immediately */
                    sensor_reading_t r;
                    strncpy(r.mac, mac, sizeof(r.mac)-1);
                    r.mac[sizeof(r.mac)-1] = '\0';
                    r.moisture = moistf;
                    moisture_receive_sensor_values(&r, 1);
                        if (maps_count < (int)(sizeof(maps)/sizeof(maps[0]))) {
                        int found = 0;
                        for (int i=0;i<maps_count;i++) if (strcmp(maps[i].mac, mac) == 0) { maps[i].addr = src; found = 1; break; }
                        if (!found) { strncpy(maps[maps_count].mac, mac, sizeof(maps[maps_count].mac)-1); maps[maps_count].addr = src; maps_count++; }
                    }
                    /* No batching: already forwarded above */
                }
            }
        }
        /* Optionally flush small batches periodically could be added here */
    }

    close(sockfd);
    /* no pending batch to flush */
    server_running = 0;
    return NULL;
}

int server_start(void) {
    if (server_running) return 0;
    int rc = pthread_create(&server_thread, NULL, server_thread_fn, NULL);
    if (rc != 0) {
        fprintf(stderr, "server_start: pthread_create failed: %s\n", strerror(rc));
        return -1;
    }
    return 0;
}

void server_stop(void) {
    if (!server_running) return;
    server_running = 0;
    /* Sending a dummy datagram to unblock recv */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct sockaddr_in a;
        memset(&a,0,sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(12345);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(s, "", 1, 0, (struct sockaddr*)&a, sizeof(a));
        close(s);
    }
    pthread_join(server_thread, NULL);
}
