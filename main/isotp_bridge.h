#ifndef BRIDGE_H
#define BRIDGE_H

// Internal queue flags, independent of the BridgeLEG wire protocol.
#define ISOTP_SEND_ELM_PHYSICAL 1
#define ISOTP_SEND_ELM_FUNCTIONAL 2

void		isotp_init();
void		isotp_deinit();
void		isotp_start_task();
void		isotp_stop_task();

void		bridge_connect();
void		bridge_disconnect();
void		bridge_received_ble(const void* src, size_t size);
int32_t		bridge_send_isotp(send_message_t *msg);
uint16_t	bridge_send_available();

#endif