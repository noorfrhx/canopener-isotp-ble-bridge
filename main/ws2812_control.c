#include "ws2812_control.h"
#include "led_strip.h"
#include "esp_err.h"

// The original firmware drove the WS2812 with the legacy RMT API, which is
// gone in ESP-IDF 5.x. Use Espressif's led_strip component instead.

static led_strip_handle_t led_strip = NULL;

void ws2812_control_init(gpio_num_t led_gpio)
{
	led_strip_config_t strip_config = {
		.strip_gpio_num = led_gpio,
		.max_leds = NUM_LEDS,
		.led_model = LED_MODEL_WS2812,
		.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
	};
	led_strip_rmt_config_t rmt_config = {
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = 10 * 1000 * 1000,
	};
	ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
}

// led_state uses the original firmware's packing: byte 2 = green,
// byte 1 = red, byte 0 = blue (see LED_RED / LED_GREEN in led.h).
void ws2812_write_leds(uint32_t led_state)
{
	if (!led_strip)
		return;
	uint8_t g = (led_state >> 16) & 0xFF;
	uint8_t r = (led_state >> 8) & 0xFF;
	uint8_t b = led_state & 0xFF;
	ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, r, g, b));
	ESP_ERROR_CHECK(led_strip_refresh(led_strip));
}
