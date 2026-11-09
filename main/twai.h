#ifndef TWAI_H
#define TWAI_H

// The ESP32-C5 has TWAI-FD controllers, which the legacy driver/twai.h API does
// not support. This module wraps the new esp_twai node API but keeps the
// classic twai_message_t struct the rest of the bridge uses.
#include "hal/twai_types_deprecated.h"

void twai_init();
void twai_deinit();
void twai_start_task();
void twai_stop_task();
void twai_send(twai_message_t *twai_tx_msg);

#endif
