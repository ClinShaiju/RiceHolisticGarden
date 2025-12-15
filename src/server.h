#pragma once
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

#ifdef __cplusplus
}
#endif

#endif /* SERVER_H */
