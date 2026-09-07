#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_wifi.h"

#define TAG "app_wifi"

#define APP_WIFI_NVS_NS   "wifi"
#define APP_WIFI_KEY_SSID "ssid"
#define APP_WIFI_KEY_PWD  "pwd"

static char s_ssid[128];
static char s_pwd[128];
static bool s_loaded = false;

static void load(void)
{
    s_ssid[0] = '\0';
    s_pwd[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(APP_WIFI_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi credentials NOT provisioned in NVS (namespace '%s' absent) - "
                      "run scripts/factory-nvs.sh", APP_WIFI_NVS_NS);
        s_loaded = true;
        return;
    }

    size_t len = sizeof(s_ssid);
    if (nvs_get_str(h, APP_WIFI_KEY_SSID, s_ssid, &len) != ESP_OK) {
        s_ssid[0] = '\0';
    }
    len = sizeof(s_pwd);
    if (nvs_get_str(h, APP_WIFI_KEY_PWD, s_pwd, &len) != ESP_OK) {
        s_pwd[0] = '\0';
    }
    nvs_close(h);

    ESP_LOGI(TAG, "wifi credentials from NVS (ssid='%s')", s_ssid);
    s_loaded = true;
}

const char *AppWifi_ssid(void)
{
    if (!s_loaded) {
        load();
    }
    return s_ssid;
}

const char *AppWifi_pwd(void)
{
    if (!s_loaded) {
        load();
    }
    return s_pwd;
}
