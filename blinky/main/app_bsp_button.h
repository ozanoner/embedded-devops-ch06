
#pragma once

typedef void (*AppBSPButtonCallback_t)(void);

void AppBSPButton_init(int btn_gpio);
void AppBSPButton_set_handler(AppBSPButtonCallback_t cb);