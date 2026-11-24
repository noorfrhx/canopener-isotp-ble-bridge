#ifndef ELM_BLE_H
#define ELM_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void elm_ble_init(void (*notify)(const char *data, size_t size));
void elm_ble_deinit(void);
void elm_ble_set_active(bool active);
void elm_ble_receive(const uint8_t *data, size_t size);
// Returns true if an ELM session owns the bus and binary BLE output must be suppressed.
bool elm_ble_reply(uint32_t source, uint32_t destination, uint8_t flags,
                   const uint8_t *data, size_t size);

#endif
