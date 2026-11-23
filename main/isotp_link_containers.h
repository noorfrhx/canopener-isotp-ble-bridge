#ifndef __ISOTP_LINK_CONTAINERS_H__
#define __ISOTP_LINK_CONTAINERS_H__

#define NUM_ISOTP_LINK_CONTAINERS 5	// ECU, TCU, Haldex, DTC broadcast + one flexible module link
#define ISOTP_MODULE_LINK		(NUM_ISOTP_LINK_CONTAINERS - 1)	// takes the ids of any other module the host addresses

typedef struct IsoTpLinkContainer {
    char    name[32];
    IsoTpLink link;
    SemaphoreHandle_t wait_for_isotp_data_sem;
    SemaphoreHandle_t data_mutex;
    SemaphoreHandle_t task_mutex;
    uint8_t *recv_buf;
    uint8_t *send_buf;
    uint8_t *payload_buf;
    uint32_t buffer_size;
    uint16_t number;
} IsoTpLinkContainer;

extern IsoTpLinkContainer isotp_link_containers[NUM_ISOTP_LINK_CONTAINERS];
extern uint16_t isotp_link_container_id;

void configure_isotp_links();
void disable_isotp_links();


#endif
