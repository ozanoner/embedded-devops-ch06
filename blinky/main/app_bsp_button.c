
#include <stddef.h>
#include "app_bsp_button.h"
#include "iot_button.h"
#include "button_gpio.h"

static button_handle_t gpio_btn = NULL;
static AppBSPButtonCallback_t button_callback = NULL;

static void button_event_cb(void *arg, void *data)
{
    button_event_t event = iot_button_get_event(arg);

    if (event == BUTTON_SINGLE_CLICK && button_callback != NULL)
    {
        button_callback();
    }
}

void AppBSPButton_init(int btn_gpio)
{

    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = btn_gpio,
        .active_level = 0,
    };

    iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &gpio_btn);
    iot_button_register_cb(gpio_btn, BUTTON_SINGLE_CLICK, NULL, button_event_cb, NULL);
}

void AppBSPButton_set_handler(AppBSPButtonCallback_t cb)
{
    button_callback = cb;
}
