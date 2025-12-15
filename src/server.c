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
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

static pthread_t server_thread;
static int server_running = 0;

/* ---- File-scope mapping & helpers ---- */
static pthread_mutex_t maps_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Per-mac mapping + small ring buffer of recent debug lines */
#define SERVER_MAP_MAX 32
#define SERVER_LOG_LINES 64
#define SERVER_LOG_LINE_LEN 128
/* Live text buffer length per device (shows recent serial output). */
#define SERVER_LIVE_BUFSZ 8192
struct mac_ip_map_entry {
    char mac[32];
    struct sockaddr_in addr;
    char logs[SERVER_LOG_LINES][SERVER_LOG_LINE_LEN];
    int log_head; /* next write index */
    int log_count; /* number of stored lines (<= SERVER_LOG_LINES) */
    /* live serial buffer (rolling tail) */
    char live_text[SERVER_LIVE_BUFSZ];
    size_t live_len;
    int last_output_state; /* 1=HIGH,0=LOW,-1 unknown */
} maps[SERVER_MAP_MAX];
static int maps_count = 0;

/* Serial reader thread state */
static pthread_t serial_thread;
static int serial_running = 0;

/* Append a line to the live_text tail (keeps last SERVER_LIVE_BUFSZ bytes)
 * Keeps lines separated by '\n'. */
static void append_live_text(struct mac_ip_map_entry *e, const char *line) {
    /* Overwrite live_text with the most recent message (not an append).
     * This keeps the UI popup small and shows the current device status.
     */
    if (!e || !line) return;
    size_t l = strlen(line);
    if (l >= SERVER_LIVE_BUFSZ) l = SERVER_LIVE_BUFSZ - 1;
    memcpy(e->live_text, line, l);
    e->live_text[l] = '\0';
    e->live_len = l;
}

/* Helper to add log line to entry index */
static void add_log_to_entry(int idx, const char *line) {
    if (idx < 0 || idx >= maps_count) return;
    struct mac_ip_map_entry *e = &maps[idx];
    if (!line) return;
    /* store timestamped line, truncated */
    strncpy(e->logs[e->log_head], line, SERVER_LOG_LINE_LEN-1);
    e->logs[e->log_head][SERVER_LOG_LINE_LEN-1] = '\0';
    e->log_head = (e->log_head + 1) % SERVER_LOG_LINES;
    if (e->log_count < SERVER_LOG_LINES) e->log_count++;
    /* also append to live buffer (serial-style) */
    append_live_text(e, line);
}



static void *server_thread_fn(void *arg) {
    (void)arg;
    int sockfd = -1;
    struct sockaddr_in addr;
    char buf[256];
    /* Simple MAC->IP mapping (moved to file-scope so other code can send
     * control packets to known devices). Protected by `maps_mutex`. */
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
            /* Also stash into per-mac log buffer when possible. Try to
             * attribute the packet to a MAC. Many payloads start with the
             * mac string; otherwise match by source address. */
            char firsttok[64] = {0};
            if (sscanf(buf, "%63s", firsttok) == 1) {
                /* crude check for colon-separated MAC */
                if (strchr(firsttok, ':')) {
                    /* find or create mapping index for this mac */
                    pthread_mutex_lock(&maps_mutex);
                    int found_idx = -1;
                    for (int i = 0; i < maps_count; ++i) if (strcasecmp(maps[i].mac, firsttok) == 0) { found_idx = i; break; }
                    if (found_idx >= 0) {
                        add_log_to_entry(found_idx, buf);
                        /* Try to parse control pin state from debug message */
                        {
                            char statebuf[32] = {0};
                            int pin = -1;
                            if (sscanf(buf, "%*[^C]CONTROL_PIN (D%d) state: %31s", &pin, statebuf) == 2) {
                                if (pin >= 0) {
                                    if (strcasecmp(statebuf, "HIGH") == 0) maps[found_idx].last_output_state = 1;
                                    else if (strcasecmp(statebuf, "LOW") == 0) maps[found_idx].last_output_state = 0;
                                }
                            } else {
                                /* also accept ACK styles like: "CMD: set D%d = %d" or "CMD D%d %d" */
                                int pin2=-1, val=-1;
                                if (sscanf(buf, "CMD: set D%d = %d", &pin2, &val) == 2 || sscanf(buf, "CMD D%d %d", &pin2, &val) == 2) {
                                    if (pin2 >= 0 && val >= 0) maps[found_idx].last_output_state = (val ? 1 : 0);
                                }
                            }
                        }
                    } else {
                        /* If this looks like a mac but no mapping exists yet, create one tied to the source address */
                        if (maps_count < SERVER_MAP_MAX) {
                            strncpy(maps[maps_count].mac, firsttok, sizeof(maps[maps_count].mac)-1);
                            maps[maps_count].mac[sizeof(maps[maps_count].mac)-1] = '\0';
                            maps[maps_count].addr = src;
                            maps[maps_count].log_head = 0; maps[maps_count].log_count = 0; maps[maps_count].live_len = 0; maps[maps_count].live_text[0] = '\0';
                            add_log_to_entry(maps_count, buf);
                            maps[maps_count].last_output_state = -1;
                            maps_count++;
                        } else {
                            /* try to find by source addr (IP only, ignore port since Arduino uses random source ports) */
                            int byaddr = -1;
                            for (int i = 0; i < maps_count; ++i) if (maps[i].addr.sin_addr.s_addr == src.sin_addr.s_addr) { byaddr = i; break; }
                            if (byaddr >= 0) {
                                add_log_to_entry(byaddr, buf);
                                /* parse output state for byaddr */
                                char statebuf[32] = {0}; int pin=-1;
                                if (sscanf(buf, "%*[^C]CONTROL_PIN (D%d) state: %31s", &pin, statebuf) == 2) {
                                    if (pin >= 0) {
                                        if (strcasecmp(statebuf, "HIGH") == 0) maps[byaddr].last_output_state = 1;
                                        else if (strcasecmp(statebuf, "LOW") == 0) maps[byaddr].last_output_state = 0;
                                    }
                                }
                            }
                        }
                    }
                    pthread_mutex_unlock(&maps_mutex);
                } else {
                    /* try mapping by source address (IP only since device sends from varying source ports) */
                    pthread_mutex_lock(&maps_mutex);
                    int byaddr = -1;
                    for (int i = 0; i < maps_count; ++i) if (maps[i].addr.sin_addr.s_addr == src.sin_addr.s_addr) { byaddr = i; break; }
                    if (byaddr >= 0) add_log_to_entry(byaddr, buf);
                    pthread_mutex_unlock(&maps_mutex);
                }
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
                /* store mapping mac->ip (thread-safe) */
                pthread_mutex_lock(&maps_mutex);
                int found = 0;
                for (int i = 0; i < maps_count; ++i) {
                    if (strcmp(maps[i].mac, mac) == 0) { maps[i].addr = src; found = 1; break; }
                }
                if (!found && maps_count < (int)(sizeof(maps)/sizeof(maps[0]))) {
                    strncpy(maps[maps_count].mac, mac, sizeof(maps[maps_count].mac)-1);
                    maps[maps_count].mac[sizeof(maps[maps_count].mac)-1] = '\0';
                    maps[maps_count].addr = src;
                    /* init log state */
                    maps[maps_count].log_head = 0;
                    maps[maps_count].log_count = 0;
                    maps[maps_count].live_len = 0;
                    maps[maps_count].live_text[0] = '\0';
                    maps[maps_count].last_output_state = -1;
                    maps_count++;
                }
                pthread_mutex_unlock(&maps_mutex);
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
                    /* store mapping mac->ip (thread-safe) */
                    pthread_mutex_lock(&maps_mutex);
                    int found = 0;
                    for (int i = 0; i < maps_count; ++i) {
                        if (strcmp(maps[i].mac, mac) == 0) { maps[i].addr = src; found = 1; break; }
                    }
                    if (!found && maps_count < (int)(sizeof(maps)/sizeof(maps[0]))) {
                        strncpy(maps[maps_count].mac, mac, sizeof(maps[maps_count].mac)-1);
                        maps[maps_count].mac[sizeof(maps[maps_count].mac)-1] = '\0';
                        maps[maps_count].addr = src;
                        maps[maps_count].log_head = 0;
                        maps[maps_count].log_count = 0;
                        maps_count++;
                    }
                    pthread_mutex_unlock(&maps_mutex);
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

int server_send_cmd_to_mac(const char *mac, int activate) {
    if (!mac) return -1;
    struct sockaddr_in dest;
    int found = 0;
    pthread_mutex_lock(&maps_mutex);
    for (int i = 0; i < maps_count; ++i) {
        if (strcmp(maps[i].mac, mac) == 0) { dest = maps[i].addr; found = 1; break; }
    }
    pthread_mutex_unlock(&maps_mutex);
    if (!found) return -1;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    char msg[64];
    /* Simple command format the firmware will accept: "D0 1" or "D0 0" */
    snprintf(msg, sizeof(msg), "D0 %d", activate ? 1 : 0);
    ssize_t rc = sendto(s, msg, strlen(msg), 0, (struct sockaddr*)&dest, sizeof(dest));
    close(s);
    return (rc == (ssize_t)strlen(msg)) ? 0 : -1;
}

int server_send_text_to_mac(const char *mac, const char *text) {
    if (!mac || !text) return -1;
    struct sockaddr_in dest;
    int found = 0;
    pthread_mutex_lock(&maps_mutex);
    for (int i = 0; i < maps_count; ++i) {
        if (strcmp(maps[i].mac, mac) == 0) { dest = maps[i].addr; found = 1; break; }
    }
    pthread_mutex_unlock(&maps_mutex);
    if (!found) return -1;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    ssize_t rc = sendto(s, text, strlen(text), 0, (struct sockaddr*)&dest, sizeof(dest));
    close(s);
    return (rc == (ssize_t)strlen(text)) ? 0 : -1;
}

int server_get_logs_for_mac(const char *mac, char *outbuf, size_t outbuflen) {
    if (!mac || !outbuf || outbuflen == 0) return -1;
    pthread_mutex_lock(&maps_mutex);
    int idx = -1;
    for (int i = 0; i < maps_count; ++i) {
        if (strcasecmp(maps[i].mac, mac) == 0) { idx = i; break; }
    }
    if (idx < 0) { pthread_mutex_unlock(&maps_mutex); return -1; }
    struct mac_ip_map_entry *e = &maps[idx];
    /* Start from oldest entry: head - count */
    int start = (e->log_head - e->log_count + SERVER_LOG_LINES) % SERVER_LOG_LINES;
    size_t used = 0;
    for (int i = 0; i < e->log_count; ++i) {
        int p = (start + i) % SERVER_LOG_LINES;
        const char *ln = e->logs[p];
        if (!ln || ln[0] == '\0') continue;
        size_t l = strlen(ln);
        if (used + l + 2 >= outbuflen) break; /* leave room for newline + null */
        memcpy(outbuf + used, ln, l);
        used += l;
        outbuf[used++] = '\n';
    }
    if (used == 0) outbuf[0] = '\0'; else outbuf[used-1] = '\0'; /* replace last '\n' with NUL */
    pthread_mutex_unlock(&maps_mutex);
    return (int)used;
}

int server_get_live_text_for_mac(const char *mac, char *outbuf, size_t outbuflen) {
    if (!mac || !outbuf || outbuflen == 0) return -1;
    pthread_mutex_lock(&maps_mutex);
    int idx = -1;
    for (int i = 0; i < maps_count; ++i) {
        if (strcasecmp(maps[i].mac, mac) == 0) { idx = i; break; }
    }
    if (idx < 0) { pthread_mutex_unlock(&maps_mutex); return -1; }
    struct mac_ip_map_entry *e = &maps[idx];
    /* Return the most recent message (overwrite behavior) */
    size_t tocopy = e->live_len;
    if (tocopy >= outbuflen) tocopy = outbuflen - 1;
    if (tocopy > 0) memcpy(outbuf, e->live_text, tocopy);
    outbuf[tocopy] = '\0';
    pthread_mutex_unlock(&maps_mutex);
    return (int)tocopy;
}

int server_get_output_state_for_mac(const char *mac) {
    if (!mac) return -1;
    pthread_mutex_lock(&maps_mutex);
    int idx = -1;
    for (int i = 0; i < maps_count; ++i) {
        if (strcasecmp(maps[i].mac, mac) == 0) { idx = i; break; }
    }
    int res = -1;
    if (idx >= 0) res = maps[idx].last_output_state;
    pthread_mutex_unlock(&maps_mutex);
    return res;
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
