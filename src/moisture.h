#pragma once
/* ui_moisture_dashboard.h */
#ifndef UI_MOISTURE_DASHBOARD_H
#define UI_MOISTURE_DASHBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/* Create the 480x320 Moisture Data fullscreen dashboard */
void ui_moisture_dashboard_absolute(void);

/* Add a new plot that is associated with `sensor_mac`. If `sensor_mac` is NULL
 * or empty a unique mac-like id will be assigned automatically. Returns the
 * plot index (0-based) or -1 on failure.
 */
int moisture_add_plot_for_sensor(const char *sensor_mac);

typedef struct {
	char mac[32];
	float moisture;
} sensor_reading_t;

/* Called by the network/flash code when new sensor readings arrive.
 * This function accepts an array of readings and records the last-received
 * data points. It does NOT update the UI directly. LVGL runs a timer that
 * applies the last received values to the UI at regular intervals.
 */
void moisture_receive_sensor_values(const sensor_reading_t *readings, size_t count);

/* Set the exponential moving average smoothing factor (alpha).
 * Alpha should be in (0..1]. Higher alpha means readings follow new
 * samples more closely. Default is 0.2.
 */
void moisture_set_smoothing_alpha(float alpha);

/* Update the flashing status text shown in the UI. Pass NULL to clear. Safe
 * to call from any thread; updates are marshalled to the LVGL thread.
 */
void moisture_flash_status_update(const char *status);

/* No single-item convenience wrapper: use the batch API `moisture_receive_sensor_values`.
 * The server should pass an array of `sensor_reading_t` and the number of
 * elements. This keeps ownership and batching explicit.
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UI_MOISTURE_DASHBOARD_H */
