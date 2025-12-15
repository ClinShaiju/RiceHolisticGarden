#pragma once
#ifndef FLASH_H
#define FLASH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Try to find a USB serial device, flash the configured firmware using arduino-cli
 * (if available), then wait for a registration string on the serial port.
 * If registration is received it will call into the moisture module to add the
 * sensor. If not, a new plot is added with an automatically assigned id.
 * Returns 0 on success (flashing attempted), -1 on failure.
 */
int flash_flash_first_device_and_register(void);

#ifdef __cplusplus
}
#endif

#endif /* FLASH_H */
