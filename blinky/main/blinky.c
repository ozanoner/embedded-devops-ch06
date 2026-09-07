
#include "app_bsp.h"
#include "app_ota.h"
#include "FreeAct.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "devops_easy_connect.h"
#include "app_enroll.h"
#include "app_wifi.h"

#define TAG "app"

#define BLINKY_STACK_SIZE 8192   /* Blinky AO stack depth, in words */
#define BLINKY_QUEUE_LEN 10            /* Blinky AO event queue capacity */
#define BLINKY_PRIO 1                  /* Blinky AO priority (1-based) */
#define BLINKY_OPTIONS 0               /* options passed to Active_start() */
#define BLINKY_TIMEOUT_MS 3000         /* red/blue toggle period, in ms */

#define OTA_TASK_STACK_SIZE 8192 /* OTA check task stack depth, in words */
#define OTA_TASK_PRIO 5                /* OTA check task priority */
#define OTA_TASK_NAME "ota_check"     /* OTA check task name */

typedef struct
{
    Active super;
    TimeEvent te;
    bool green_is_on;
} Blinky;

typedef enum
{
    TIMEOUT_SIG = USER_SIG,
    BUTTON_CLICKED_SIG,
} BlinkySignal_t;

static Blinky blinky;
static StackType_t blinky_stack[BLINKY_STACK_SIZE];
static Event *blinky_queue[BLINKY_QUEUE_LEN];
static StaticTask_t ota_task_buf;
static StackType_t ota_task_stack[OTA_TASK_STACK_SIZE];

static void Blinky_ctor(Blinky *const me);
static void Blinky_dispatch(Blinky *const me, Event const *const e);
static void handle_button_click();
static bool run_diagnostics(void);
static void on_connected(void *arg);
static void run_ota_check(void *arg);

static void print_memory_info()
{
    size_t free_dram = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Free 8-bit accessible DRAM: %u bytes", free_dram);
}

void app_main()
{
    ESP_LOGI(TAG, "Blinky application (new feature added)");
    print_memory_info();

    AppEnroll_init();

    app_wifi_init(AppWifi_ssid(), AppWifi_pwd(), on_connected, NULL);
    AppBSP_init();
    AppBSPButton_set_handler(handle_button_click);

    AppOTA_validate_running_app(run_diagnostics);

    app_wifi_connect();

    Blinky_ctor(&blinky);
    Active_start(&blinky.super,
                 BLINKY_PRIO, // priority
                 blinky_queue, sizeof(blinky_queue) / sizeof(blinky_queue[0]),
                 blinky_stack, sizeof(blinky_stack),
                 BLINKY_OPTIONS // options
    );
}

static void Blinky_ctor(Blinky *const me)
{
    Active_ctor(&me->super, (DispatchHandler)&Blinky_dispatch);
    me->te.type = TYPE_PERIODIC;
    TimeEvent_ctor(&me->te, USER_SIG, &me->super);
    me->green_is_on = false;
}

static void Blinky_dispatch(Blinky *const me, Event const *const e)
{
    // ESP_LOGI(TAG, "Blinky_dispatch: sig=%d", e->sig);

    switch (e->sig)
    {
    case INIT_SIG:
        TimeEvent_arm(&me->te, BLINKY_TIMEOUT_MS);
        break;

    case TIMEOUT_SIG:
        AppBSP_toggle_red();
        AppBSP_toggle_blue();
        break;

    case BUTTON_CLICKED_SIG:
        AppBSP_toggle_green();
        me->green_is_on = !me->green_is_on;
        ESP_LOGI(TAG, "Green LED: %s", me->green_is_on ? "ON" : "OFF");
        break;

    default:
        break;
    }
}

static void handle_button_click()
{
    // ESP_LOGI(TAG, "Button clicked!");

    static Event e = {.sig = BUTTON_CLICKED_SIG};
    Active_post((Active *)&blinky.super, (Event *)&e);
}

static bool run_diagnostics(void)
{
    return true;
}

static void run_ota_check(void *arg)
{
    ESP_LOGI(TAG, "WiFi connected, checking enrollment and OTA ...");

    esp_err_t err = AppEnroll_ensure();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Enrollment OK (CN=%s)", AppEnroll_device_id());
    }
    else if (err == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Factory identity not provisioned, cannot enroll");
    }
    else
    {
        ESP_LOGW(TAG, "Enrollment deferred (err=%s)", esp_err_to_name(err));
    }

    bool update_available = AppOTA_check_for_update();
    if (update_available)
    {
        err = AppOTA_perform_update();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
        }
        else
        {
            ESP_LOGI(TAG, "OTA update complete, restarting");
            esp_restart();
        }
    }

    vTaskDelete(NULL);
}

static void on_connected(void *arg)
{
    TaskHandle_t created = xTaskCreateStatic(
        run_ota_check,
        OTA_TASK_NAME,
        OTA_TASK_STACK_SIZE,
        NULL,
        OTA_TASK_PRIO,
        ota_task_stack,
        &ota_task_buf);

    if (created == NULL)
    {
        ESP_LOGE(TAG, "Failed to create OTA check task");
    }
}