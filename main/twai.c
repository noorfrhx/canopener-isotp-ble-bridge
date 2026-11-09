#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "isotp.h"
#include "isotp_link_containers.h"
#include "constants.h"
#include "connection_handler.h"
#include "board.h"
#include "twai.h"

#define TWAI_TAG 			"TWAI"

// Number of frames that may be queued in the driver at once. Each slot owns
// its frame + data buffer until the driver reports the frame as sent.
#define TWAI_TX_SLOTS		32

typedef enum {
	TWAI_EVT_STATE,
} twai_evt_type_t;

typedef struct {
	twai_evt_type_t		type;
	twai_error_state_t	old_state;
	twai_error_state_t	new_state;
} twai_evt_t;

typedef struct {
	twai_frame_t	frame;
	uint8_t			data[8];
} twai_tx_slot_t;

static twai_node_handle_t			twai_node				= NULL;
static QueueHandle_t				twai_rx_queue			= NULL;
static QueueHandle_t				twai_evt_queue			= NULL;
static SemaphoreHandle_t			twai_tx_slots_sem		= NULL;
static SemaphoreHandle_t			twai_tx_mutex			= NULL;
static twai_tx_slot_t				twai_tx_slots[TWAI_TX_SLOTS];
static uint16_t						twai_tx_slot_next		= 0;
static SemaphoreHandle_t			twai_receive_task_mutex = NULL;
static SemaphoreHandle_t			twai_alert_task_mutex	= NULL;
static SemaphoreHandle_t			twai_bus_off_mutex		= NULL;
static SemaphoreHandle_t			twai_settings_mutex		= NULL;
static bool16						twai_run_task			= false;

void twai_receive_task(void *arg);
void twai_alert_task(void* arg);

void twai_set_run_task(bool16 allow)
{
	tMUTEX(twai_settings_mutex);
		twai_run_task = allow;
	rMUTEX(twai_settings_mutex);
}

bool16 twai_allow_run_task()
{
	tMUTEX(twai_settings_mutex);
		bool16 run_task = twai_run_task;
	rMUTEX(twai_settings_mutex);

	return run_task;
}

/* ------------------------------ ISR callbacks ------------------------------ */

static IRAM_ATTR bool twai_on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
	uint8_t buf[8];
	twai_frame_t rx = {
		.buffer = buf,
		.buffer_len = sizeof(buf),
	};
	if (twai_node_receive_from_isr(handle, &rx) != ESP_OK)
		return false;

	// The bridge only speaks classic CAN; ignore FD and remote frames.
	if (rx.header.fdf || rx.header.rtr)
		return false;

	twai_message_t msg = {0};
	msg.identifier = rx.header.id;
	msg.extd = rx.header.ide;
	msg.data_length_code = rx.header.dlc > 8 ? 8 : rx.header.dlc;
	memcpy(msg.data, buf, msg.data_length_code);

	BaseType_t woken = pdFALSE;
	xQueueSendFromISR(twai_rx_queue, &msg, &woken);
	return woken == pdTRUE;
}

static volatile uint32_t twai_tx_fail_count = 0;

static IRAM_ATTR bool twai_on_tx_done(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *user_ctx)
{
	// Frames in the hardware buffers complete as failed when the node goes
	// bus-off. Count them so the alert task can report the loss.
	if (!edata->is_tx_success)
		twai_tx_fail_count++;
	BaseType_t woken = pdFALSE;
	xSemaphoreGiveFromISR(twai_tx_slots_sem, &woken);
	return woken == pdTRUE;
}

static IRAM_ATTR bool twai_on_state_change(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *user_ctx)
{
	twai_evt_t evt = {
		.type = TWAI_EVT_STATE,
		.old_state = edata->old_sta,
		.new_state = edata->new_sta,
	};
	BaseType_t woken = pdFALSE;
	xQueueSendFromISR(twai_evt_queue, &evt, &woken);
	return woken == pdTRUE;
}

/* -------------------------------- lifecycle -------------------------------- */

void twai_init()
{
	twai_deinit();

	//init mutexes
	twai_receive_task_mutex = xSemaphoreCreateMutex();
	twai_alert_task_mutex	= xSemaphoreCreateMutex();
	twai_bus_off_mutex		= xSemaphoreCreateMutex();
	twai_settings_mutex		= xSemaphoreCreateMutex();
	twai_tx_mutex			= xSemaphoreCreateMutex();

	// TCAN3414 STB pin: drive low for normal (transmit + receive) mode.
	gpio_config_t io_conf = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = GPIO_OUTPUT_PIN_SEL(BOARD_CAN_STB_GPIO),
		.pull_down_en = 1,
		.pull_up_en = 0,
	};
	gpio_config(&io_conf);
	gpio_set_level(BOARD_CAN_STB_GPIO, BOARD_CAN_STB_ACTIVE);

	ESP_LOGI(TWAI_TAG, "Init");
}

void twai_deinit()
{
	bool16 didDeInit = false;

	SemaphoreHandle_t* mutexes[] = {
		&twai_receive_task_mutex, &twai_alert_task_mutex, &twai_bus_off_mutex,
		&twai_settings_mutex, &twai_tx_mutex
	};
	for (int i = 0; i < sizeof(mutexes) / sizeof(mutexes[0]); i++) {
		if (*mutexes[i]) {
			vSemaphoreDelete(*mutexes[i]);
			*mutexes[i] = NULL;
			didDeInit = true;
		}
	}

	if (didDeInit)
		ESP_LOGI(TWAI_TAG, "Deinit");
}

static void twai_driver_install()
{
	twai_rx_queue		= xQueueCreate(CAN_INTERNAL_BUFFER_SIZE, sizeof(twai_message_t));
	twai_evt_queue		= xQueueCreate(16, sizeof(twai_evt_t));
	twai_tx_slots_sem	= xSemaphoreCreateCounting(TWAI_TX_SLOTS, TWAI_TX_SLOTS);
	twai_tx_slot_next	= 0;

	twai_onchip_node_config_t node_config = {
		.io_cfg = {
			.tx = BOARD_CAN_TX_GPIO,
			.rx = BOARD_CAN_RX_GPIO,
			.quanta_clk_out = -1,
			.bus_off_indicator = -1,
		},
		.bit_timing = {
			.bitrate = CAN_BITRATE,
		},
		.fail_retry_cnt = -1,		// retransmit until acked, like the legacy driver
		.tx_queue_depth = TWAI_TX_SLOTS,
		.intr_priority = 1,
	};
	ESP_ERROR_CHECK(twai_new_node_onchip(&node_config, &twai_node));

	// Filters start disabled on the TWAI-FD controller; open filter 0 fully.
	twai_mask_filter_config_t accept_all = {
		.id = 0,
		.mask = 0,
	};
	ESP_ERROR_CHECK(twai_node_config_mask_filter(twai_node, 0, &accept_all));

	twai_event_callbacks_t cbs = {
		.on_rx_done = twai_on_rx_done,
		.on_tx_done = twai_on_tx_done,
		.on_state_change = twai_on_state_change,
	};
	ESP_ERROR_CHECK(twai_node_register_event_callbacks(twai_node, &cbs, NULL));
	ESP_LOGI(TWAI_TAG, "Driver installed (%d bit/s)", CAN_BITRATE);

	ESP_ERROR_CHECK(twai_node_enable(twai_node));
	ESP_LOGI(TWAI_TAG, "Driver started");
}

static void twai_driver_uninstall()
{
	if (twai_node) {
		// twai_node_disable refuses while bus-off, which would leak the
		// driver's power-management lock on delete. Recover first (bounded).
		twai_node_status_t status;
		if (twai_node_get_info(twai_node, &status, NULL) == ESP_OK && status.state == TWAI_ERROR_BUS_OFF) {
			twai_node_recover(twai_node);
			for (int i = 0; i < 20; i++) {
				vTaskDelay(pdMS_TO_TICKS(5));
				if (twai_node_get_info(twai_node, &status, NULL) == ESP_OK && status.state != TWAI_ERROR_BUS_OFF)
					break;
			}
		}
		if (twai_node_disable(twai_node) != ESP_OK)
			ESP_LOGW(TWAI_TAG, "Node still bus-off at shutdown");
		twai_node_delete(twai_node);
		twai_node = NULL;
	}
	if (twai_rx_queue) {
		vQueueDelete(twai_rx_queue);
		twai_rx_queue = NULL;
	}
	if (twai_evt_queue) {
		vQueueDelete(twai_evt_queue);
		twai_evt_queue = NULL;
	}
	if (twai_tx_slots_sem) {
		vSemaphoreDelete(twai_tx_slots_sem);
		twai_tx_slots_sem = NULL;
	}
}

void twai_start_task()
{
	twai_stop_task();
	twai_driver_install();

	twai_set_run_task(true);

	ESP_LOGI(TWAI_TAG, "Tasks starting");
	xSemaphoreTake(sync_task_sem, 0);

	xTaskCreate(twai_alert_task, "TWAI_alert", TASK_STACK_SIZE, NULL, TWAI_TASK_PRIO, NULL);
	xSemaphoreTake(sync_task_sem, portMAX_DELAY);

	xTaskCreate(twai_receive_task, "TWAI_rx", TASK_STACK_SIZE, NULL, TWAI_TASK_PRIO, NULL);
	xSemaphoreTake(sync_task_sem, portMAX_DELAY);

	ESP_LOGI(TWAI_TAG, "Tasks started");
}

void twai_stop_task()
{
	if (twai_allow_run_task()) {
		twai_set_run_task(false);

		tMUTEX(twai_receive_task_mutex);
		rMUTEX(twai_receive_task_mutex);

		tMUTEX(twai_alert_task_mutex);
		rMUTEX(twai_alert_task_mutex);

		ESP_LOGI(TWAI_TAG, "Tasks stopped");

		twai_driver_uninstall();

		ESP_LOGI(TWAI_TAG, "Driver uninstalled");
	}
}

/* ----------------------------------- RX ------------------------------------ */

void twai_send_isotp_message(IsoTpLinkContainer* link, twai_message_t* msg)
{
	ESP_LOGD(TWAI_TAG, "twai_receive_task: link match");
	tMUTEX(link->data_mutex);
		isotp_on_can_message(&link->link, msg->data, msg->data_length_code);
	rMUTEX(link->data_mutex);

	ESP_LOGD(TWAI_TAG, "twai_receive_task: giving wait_for_isotp_data_sem");
	xSemaphoreGive(link->wait_for_isotp_data_sem);
}

void twai_receive_task(void *arg)
{
	//subscribe to WDT
	ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
	ESP_ERROR_CHECK(esp_task_wdt_status(NULL));

	tMUTEX(twai_receive_task_mutex);
		ESP_LOGI(TWAI_TAG, "Receive task started");
		xSemaphoreGive(sync_task_sem);
		twai_message_t twai_rx_msg;
		while (twai_allow_run_task())
		{
			if (xQueueReceive(twai_rx_queue, &twai_rx_msg, pdMS_TO_TICKS(TIMEOUT_LONG)) == pdTRUE) {
				ESP_LOGD(TWAI_TAG, "Received TWAI %08lX and length %08X", (unsigned long)twai_rx_msg.identifier, twai_rx_msg.data_length_code);
				ch_take_can_timer_sem();

				if (twai_rx_msg.identifier < 0x500) {
					esp_task_wdt_reset();
					continue;
				}

				IsoTpLinkContainer* isotp_link_container = &isotp_link_containers[isotp_link_container_id];
				if (twai_rx_msg.identifier == isotp_link_container->link.receive_arbitration_id) {
					twai_send_isotp_message(isotp_link_container, &twai_rx_msg);
				} else {
					for (uint16_t i = 0; i < NUM_ISOTP_LINK_CONTAINERS; i++) {
						isotp_link_container = &isotp_link_containers[i];
						if (twai_rx_msg.identifier == isotp_link_container->link.receive_arbitration_id) {
							twai_send_isotp_message(isotp_link_container, &twai_rx_msg);
							break;
						}
					}
				}
			}

			//reset the WDT and yield to tasks
			esp_task_wdt_reset();
			taskYIELD();
		}
		ESP_LOGI(TWAI_TAG, "Receive task stopped");
	rMUTEX(twai_receive_task_mutex);

	//unsubscribe to WDT and delete task
	ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
}

/* ----------------------------------- TX ------------------------------------ */

void twai_send(twai_message_t *twai_tx_msg)
{
	// Block while the bus is recovering from bus-off.
	tMUTEX(twai_bus_off_mutex);
	rMUTEX(twai_bus_off_mutex);

	// Wait for a free slot; slots are released in order by on_tx_done.
	xSemaphoreTake(twai_tx_slots_sem, portMAX_DELAY);

	tMUTEX(twai_tx_mutex);
		twai_tx_slot_t* slot = &twai_tx_slots[twai_tx_slot_next];
		twai_tx_slot_next = (twai_tx_slot_next + 1) % TWAI_TX_SLOTS;

		uint8_t len = twai_tx_msg->data_length_code > 8 ? 8 : twai_tx_msg->data_length_code;
		memset(slot, 0, sizeof(*slot));
		memcpy(slot->data, twai_tx_msg->data, len);
		slot->frame.header.id = twai_tx_msg->identifier;
		slot->frame.header.ide = twai_tx_msg->extd;
		slot->frame.header.dlc = len;
		slot->frame.buffer = slot->data;
		slot->frame.buffer_len = len;

		// Like the legacy driver, a frame the controller refuses (bus-off
		// between the gate above and here) is dropped rather than retried,
		// so the sender never spins forever. The slot goes back to the pool
		// because on_tx_done will not fire for a frame that was not queued.
		esp_err_t err = twai_node_transmit(twai_node, &slot->frame, portMAX_DELAY);
		if (err != ESP_OK) {
			twai_tx_fail_count++;
			xSemaphoreGive(twai_tx_slots_sem);
		}
	rMUTEX(twai_tx_mutex);
}

/* --------------------------------- alerts ---------------------------------- */

void twai_alert_task(void* arg)
{
	//subscribe to WDT
	ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
	ESP_ERROR_CHECK(esp_task_wdt_status(NULL));

	tMUTEX(twai_alert_task_mutex);
		ESP_LOGI(TWAI_TAG, "Alert task started");
		xSemaphoreGive(sync_task_sem);
		bool16 bus_off_held = false;
		int64_t last_recover_us = 0;
		uint32_t reported_fail_count = 0;
		twai_evt_t evt;
		while (twai_allow_run_task()) {
			// State-change events are only used for logging. Recovery is driven
			// by polling the node's real state, because the driver can report
			// BUS_OFF more than once per bus-off and the event queue can overflow.
			if (xQueueReceive(twai_evt_queue, &evt, pdMS_TO_TICKS(TIMEOUT_NORMAL)) == pdTRUE) {
				if (evt.new_state == TWAI_ERROR_WARNING)
					ESP_LOGI(TWAI_TAG, "Surpassed Error Warning Limit");
				else if (evt.new_state == TWAI_ERROR_PASSIVE)
					ESP_LOGI(TWAI_TAG, "Entered Error Passive state");
			}

			twai_node_status_t status;
			if (twai_node_get_info(twai_node, &status, NULL) == ESP_OK) {
				if (status.state == TWAI_ERROR_BUS_OFF) {
					// Hold the gate so twai_send blocks instead of failing.
					if (!bus_off_held && xSemaphoreTake(twai_bus_off_mutex, pdMS_TO_TICKS(TIMEOUT_NORMAL)) == pdTRUE) {
						bus_off_held = true;
						ESP_LOGW(TWAI_TAG, "Bus Off state");
					}
					// Recovery needs 128 x 11 recessive bits; re-issue it if the
					// bus is still off a second later. INVALID_STATE means the
					// node already left bus-off, which is fine.
					int64_t now = esp_timer_get_time();
					if (last_recover_us == 0 || now - last_recover_us > 1000000) {
						esp_err_t err = twai_node_recover(twai_node);
						if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
							ESP_LOGE(TWAI_TAG, "Bus recovery failed: %s", esp_err_to_name(err));
						else
							ESP_LOGW(TWAI_TAG, "Initiate bus recovery");
						last_recover_us = now;
					}
				} else if (bus_off_held) {
					xSemaphoreGive(twai_bus_off_mutex);
					bus_off_held = false;
					last_recover_us = 0;
					ESP_LOGW(TWAI_TAG, "Bus Recovered");
				}
			}

			uint32_t fail_count = twai_tx_fail_count;
			if (fail_count != reported_fail_count) {
				ESP_LOGW(TWAI_TAG, "%lu CAN frame(s) failed to send", (unsigned long)(fail_count - reported_fail_count));
				reported_fail_count = fail_count;
			}

			//reset the WDT and yield to tasks
			esp_task_wdt_reset();
			taskYIELD();
		}
		if (bus_off_held)
			xSemaphoreGive(twai_bus_off_mutex);
		ESP_LOGI(TWAI_TAG, "Alert task stopped");
	rMUTEX(twai_alert_task_mutex);

	//unsubscribe to WDT and delete task
	ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
	vTaskDelete(NULL);
}
