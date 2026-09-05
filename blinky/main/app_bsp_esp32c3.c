
#include <stdbool.h>
#include "app_bsp.h"
#include "led_strip.h"

#define BUTTON_GPIO 9

static led_strip_handle_t led_strip;
static uint8_t red_val = 0;
static uint8_t blue_val = 255;
static uint8_t green_val = 0;

static void update_led()
{
    led_strip_set_pixel(led_strip, 0, red_val, green_val, blue_val);
    led_strip_refresh(led_strip);
}

void AppBSP_init()
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = 8,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB};

    led_strip_rmt_config_t rmt_config = {};

    led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);

    update_led();

    AppBSPButton_init(BUTTON_GPIO);
}

void AppBSP_toggle_red()
{
    red_val = (red_val == 0) ? 255 : 0;
    update_led();
}

void AppBSP_toggle_blue()
{
    blue_val = (blue_val == 0) ? 255 : 0;
    update_led();
}

void AppBSP_toggle_green()
{
    green_val = (green_val == 0) ? 255 : 0;
    update_led();
}