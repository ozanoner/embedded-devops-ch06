#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t AppEnroll_init(void);
bool AppEnroll_is_enrolled(void);
esp_err_t AppEnroll_ensure(void);
const char *AppEnroll_device_id(void);
esp_err_t AppEnroll_get_device_cert(char **pem_out, int *len_out); /* malloc'd copy; caller frees */
