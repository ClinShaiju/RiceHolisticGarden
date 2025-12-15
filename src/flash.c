/*
 * flash.c
 * Clean implementation of the flasher thread:
 * - find a serial device (ttyACM* / ttyUSB*)
 * - optionally copy the sketch and inject config.h from env vars
 * - run arduino-cli upload and stream output to the UI via
 *   `moisture_flash_status_update()`
 * - after upload re-scan /dev to find the serial node and wait for a
 *   registration line containing a MAC address (aa:bb:cc:dd:ee:ff)
 * - call `moisture_add_plot_for_sensor(mac)` on success, or with NULL
 *   to add an unassigned plot on failure.
 */

#include "flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>

#include "src/moisture.h"

#define FIRMWARE_PATH "firmware/plant_sensor"
#define FQBN_ENV_VAR "FLASH_FQBN"
#define DEFAULT_FQBN "arduino:samd:nano_33_iot"
#define SERIAL_WAIT_SEC 60

static int open_serial_rd(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOCTTY);
    if (fd < 0) return -1;
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) { close(fd); return -1; }
    cfmakeraw(&tty);
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag |= (CLOCAL | CREAD);
    tcsetattr(fd, TCSANOW, &tty);
    return fd;
}

static int extract_mac_from_string(const char *s, char *out, size_t out_len)
{
    const char *p = s;
    while (*p) {
        if (strlen(p) >= 17) {
            int ok = 1;
            for (int i = 0; i < 17; ++i) {
                char c = p[i];
                if ((i % 3) == 2) { if (c != ':') { ok = 0; break; } }
                else { if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { ok = 0; break; } }
            }
            if (ok) {
                if (out_len > 17) { memcpy(out, p, 17); out[17] = '\0'; return 0; }
                return -1;
            }
        }
        ++p;
    }
    return -1;
}

static int wait_for_registration_on_serial(const char *devpath, int timeout_sec, char *out_mac, size_t out_mac_len)
{
    int fd = open_serial_rd(devpath);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    char buf[256]; int pos = 0; int ticks = 0;
    while (ticks < timeout_sec * 10) {
        usleep(100000); ++ticks;
        ssize_t r = read(fd, buf + pos, 1);
        if (r > 0) {
            if (buf[pos] == '\n' || buf[pos] == '\r') {
                buf[pos] = '\0';
                if (extract_mac_from_string(buf, out_mac, out_mac_len) == 0) { close(fd); return 0; }
                pos = 0;
            } else {
                ++pos; if (pos >= (int)sizeof(buf)-1) pos = 0;
            }
        } else if (r == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { if ((ticks % 10) == 0) moisture_flash_status_update("Waiting for serial registration..."); continue; }
            else { char eb[128]; snprintf(eb, sizeof(eb), "Serial read error: %s", strerror(errno)); moisture_flash_status_update(eb); break; }
        }
    }
    close(fd); return -1;
}

static int find_first_serial(char *out_path, size_t out_len)
{
    DIR *d = opendir("/dev"); if (!d) return -1;
    struct dirent *ent; int found = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "ttyACM", 6) == 0 || strncmp(ent->d_name, "ttyUSB", 6) == 0) { snprintf(out_path, out_len, "/dev/%s", ent->d_name); found = 1; break; }
    }
    closedir(d); return found ? 0 : -1;
}

static void *flash_thread_fn(void *arg)
{
    (void)arg;
    FILE *m = fopen("/tmp/flash_thread_started", "a"); if (m) { fprintf(m, "flash thread started\n"); fclose(m); }

    moisture_flash_status_update("Searching for serial device...");
    char devpath[256] = {0}; if (find_first_serial(devpath, sizeof(devpath)) != 0) { moisture_flash_status_update("No serial device found"); moisture_add_plot_for_sensor(NULL); return NULL; }

    const char *env_ssid = getenv("FLASH_SSID"); const char *env_pass = getenv("FLASH_PASS"); const char *env_target = getenv("FLASH_TARGET_IP"); const char *env_ctrl = getenv("FLASH_CONTROL_PIN");
    char use_path[512]; strncpy(use_path, FIRMWARE_PATH, sizeof(use_path)); use_path[sizeof(use_path)-1] = '\0'; int created_tmp = 0; char tmpdir[512];
    if (env_ssid || env_pass || env_target) {
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/plant_sensor_%d", (int)getpid()); mkdir(tmpdir, 0755);
        char src[512], dst[512]; snprintf(src, sizeof(src), "%s/plant_sensor.ino", FIRMWARE_PATH); snprintf(dst, sizeof(dst), "%s/plant_sensor.ino", tmpdir);
        FILE *fr = fopen(src, "rb"); if (fr) {
            FILE *fw = fopen(dst, "wb"); if (fw) { char dbuf[4096]; size_t n; while ((n = fread(dbuf,1,sizeof(dbuf),fr))>0) fwrite(dbuf,1,n,fw); fclose(fw); }
            fclose(fr);
            snprintf(dst, sizeof(dst), "%s/config.h", tmpdir);
            FILE *fc = fopen(dst, "wb");
            if (fc) {
                if (env_ssid) fprintf(fc, "#define WIFI_SSID \"%s\"\n", env_ssid);
                if (env_pass) fprintf(fc, "#define WIFI_PASS \"%s\"\n", env_pass);
                if (env_target) fprintf(fc, "#define TARGET_IP \"%s\"\n", env_target);
                /* Ensure control pin default is D2 unless overridden */
                if (env_ctrl) fprintf(fc, "#define CONTROL_PIN %s\n", env_ctrl);
                else fprintf(fc, "#define CONTROL_PIN 2\n");
                fclose(fc);
                strncpy(use_path, tmpdir, sizeof(use_path)); created_tmp = 1;
            }
        }
    }

    const char *arduino_cli = NULL; if (access("/usr/local/bin/arduino-cli", X_OK) == 0) arduino_cli = "/usr/local/bin/arduino-cli"; else if (access("/usr/bin/arduino-cli", X_OK) == 0) arduino_cli = "/usr/bin/arduino-cli";
    if (arduino_cli) {
        const char *fqbn = getenv(FQBN_ENV_VAR); if (!fqbn) fqbn = DEFAULT_FQBN;

        char compile_cmd[2048];
        snprintf(compile_cmd, sizeof(compile_cmd), "%s compile --fqbn %s %s 2>&1", arduino_cli, fqbn, use_path);
        moisture_flash_status_update("Compiling sketch...");
        FILE *pc = popen(compile_cmd, "r");
        int compile_ok = 0;
        if (pc) {
            char line[512];
            while (fgets(line, sizeof(line), pc)) moisture_flash_status_update(line);
            int rc = pclose(pc);
            if (rc == 0) compile_ok = 1;
            else {
                char eb[128]; snprintf(eb, sizeof(eb), "Compile failed (rc=%d)", rc); moisture_flash_status_update(eb);
            }
        } else {
            moisture_flash_status_update("Failed to run arduino-cli compile");
        }

        if (compile_ok) {
            char cmd[2048];
            snprintf(cmd, sizeof(cmd), "%s upload -p %s --fqbn %s %s 2>&1", arduino_cli, devpath, fqbn, use_path);
            moisture_flash_status_update("Flashing device...");
            FILE *p = popen(cmd, "r");
            if (p) {
                char line[512]; while (fgets(line, sizeof(line), p)) moisture_flash_status_update(line);
                pclose(p);
            } else moisture_flash_status_update("Failed to run arduino-cli upload");
        } else {
            moisture_flash_status_update("Skipping upload due to compile errors");
        }
    } else { moisture_flash_status_update("arduino-cli not found; skipping flash"); }

    char newpath[256] = {0}; if (find_first_serial(newpath, sizeof(newpath)) == 0) {
        char msg[128]; snprintf(msg, sizeof(msg), "Using serial device %s for registration", newpath); moisture_flash_status_update(msg);
        char mac[32] = {0}; if (wait_for_registration_on_serial(newpath, SERIAL_WAIT_SEC, mac, sizeof(mac)) == 0) { char reg[128]; snprintf(reg, sizeof(reg), "Registered %s", mac); moisture_flash_status_update(reg); moisture_add_plot_for_sensor(mac); }
        else { moisture_flash_status_update("No registration received; adding unassigned plot"); moisture_add_plot_for_sensor(NULL); }
    } else { moisture_flash_status_update("No serial device found after upload"); moisture_add_plot_for_sensor(NULL); }

    if (created_tmp) { char tmpfile[512]; snprintf(tmpfile, sizeof(tmpfile), "%s/plant_sensor.ino", tmpdir); unlink(tmpfile); snprintf(tmpfile, sizeof(tmpfile), "%s/config.h", tmpdir); unlink(tmpfile); rmdir(tmpdir); }

    moisture_flash_status_update(NULL);
    return NULL;
}

int flash_flash_first_device_and_register(void)
{
    pthread_t t; FILE *marker = fopen("/tmp/flash_start_request", "a"); if (marker) { fprintf(marker, "flash start requested\n"); fclose(marker); }
    int rc = pthread_create(&t, NULL, flash_thread_fn, NULL);
    if (rc != 0) { char eb[128]; snprintf(eb, sizeof(eb), "pthread_create failed: %s", strerror(rc)); moisture_flash_status_update(eb); return -1; }
    pthread_detach(t);
    return 0;
}
