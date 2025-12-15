#pragma once
#include <stddef.h>
#ifndef SERVER_H
#define SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start the background UDP server that receives sensor packets.
 * The server will call into the moisture module to update values.
 */
int server_start(void);
void server_stop(void);

/* Send a small UDP control command to the device identified by `mac`.
 * `activate` should be 1 to enable D0, 0 to disable. Returns 0 on success
 * or -1 if the MAC is unknown or send fails.
 */
int server_send_cmd_to_mac(const char *mac, int activate);

/* Send an arbitrary text message to the device identified by `mac` over UDP.
 * Returns 0 on success or -1 on error (unknown mac or send failure).
 */
int server_send_text_to_mac(const char *mac, const char *text);

/* Retrieve recent debug/UDP lines received from device `mac`.
 * `outbuf` is filled with newline-separated lines (up to `outbuflen-1`).
 * Returns number of bytes written or -1 if mac not found.
 */
int server_get_logs_for_mac(const char *mac, char *outbuf, size_t outbuflen);
/* Retrieve the current live serial output for `mac` (overwrites previous).
 * Returns number of bytes written or -1 if mac not found.
 */
int server_get_live_text_for_mac(const char *mac, char *outbuf, size_t outbuflen);
/* Return last-known D0 output state for a device: 1=HIGH, 0=LOW, -1=unknown */
int server_get_output_state_for_mac(const char *mac);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_H */
