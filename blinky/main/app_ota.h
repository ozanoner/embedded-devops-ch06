#pragma once

#include <stdbool.h>
#include "esp_err.h"

bool AppOTA_check_for_update();
esp_err_t AppOTA_perform_update();
void AppOTA_validate_running_app(bool (*diagnostics)(void));