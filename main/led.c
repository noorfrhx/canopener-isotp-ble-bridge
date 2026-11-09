#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "twai.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "isotp.h"
#include "constants.h"
#include "ws2812_control.h"
#include "led.h"

#define LED_TAG 			"LED"

uint32_t led_color = 0;

void led_init()
{
	// CAN Opener SE: the RGB LED is always powered, no enable pin.
	ws2812_control_init(LED_GPIO_NUM);
	led_setcolor(LED_RED_QRT);
}

void led_deinit()
{
	led_setcolor(LED_OFF);
}

void led_setcolor(uint32_t color)
{
	led_color = color;
	ws2812_write_leds(led_color & 0xFFFFFF);
}

uint32_t led_getcolor()
{
	return led_color;
}
