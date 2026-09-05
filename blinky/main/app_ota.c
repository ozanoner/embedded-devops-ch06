#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include <string.h>

#include "app_ota.h"

#define TAG "app_ota"
#define APP_OTA_FIRMWARE_URL "https://github.com/ozanoner/embedded-devops-ch06/releases/latest/download/blinky.bin"
#define APP_OTA_HTTP_BUFFER_SIZE 16384

static esp_https_ota_handle_t ota_handle;
static bool ota_update_available;

void AppOTA_validate_running_app(bool (*diagnostics)(void))
{
    if (diagnostics())
    {
        ESP_LOGI(TAG, "Diagnostics passed, marking app as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
    else
    {
        ESP_LOGE(TAG, "Diagnostics failed, marking app as invalid and rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

bool AppOTA_check_for_update()
{
    static esp_http_client_config_t http_config = {
        .url = APP_OTA_FIRMWARE_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = APP_OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = APP_OTA_HTTP_BUFFER_SIZE,
    };
    static esp_https_ota_config_t ota_config = {.http_config = &http_config};
    esp_app_desc_t new_app_info;

    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_https_ota_get_img_desc(ota_handle, &new_app_info);
    if (err != ESP_OK)
    {
        esp_https_ota_abort(ota_handle);
        ESP_LOGE(TAG, "OTA version check failed: %s", esp_err_to_name(err));
        return false;
    }

    if (strcmp(new_app_info.version, esp_app_get_description()->version) == 0)
    {
        esp_https_ota_abort(ota_handle);
        ESP_LOGI(TAG, "Firmware is already up to date");
        return false;
    }

    ota_update_available = true;
    ESP_LOGI(TAG, "Firmware update available: %s", new_app_info.version);
    return true;
}

esp_err_t AppOTA_perform_update()
{
    if (!ota_update_available)
        return ESP_ERR_INVALID_STATE;

    esp_err_t err;
    while ((err = esp_https_ota_perform(ota_handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        ;

    if (err != ESP_OK)
    {
        esp_https_ota_abort(ota_handle);
        ota_update_available = false;
        return err;
    }

    err = esp_https_ota_finish(ota_handle);
    ota_update_available = false;
    return err;
}