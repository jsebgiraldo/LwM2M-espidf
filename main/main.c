/* Emulation example.
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <sys/param.h>
#include <esp_http_server.h>
#include "esp_netif.h"
#include "esp_netif_types.h"

#include "esp_eth.h"

#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_check.h"

#include <nvs_flash.h>

#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_openthread_cli.h"

#include "esp_ot_config.h"

#include "openthread/error.h"
#include "openthread/logging.h"
#include "openthread/tasklet.h"
#include "openthread/instance.h"
#include "openthread/logging.h"
#include "openthread/tasklet.h"
#include "openthread/cli.h"
#include <openthread/coap_secure.h>
#include <openthread/thread.h>

#include "esp_vfs_eventfd.h"

#include "anjay_config.h"

#include "utils.h"

#include "esp_ot_cli_extension.h"

#define CONFIG_ESP_WIFI_SSID      "SEBAS_LAN_AP"
#define CONFIG_ESP_WIFI_PASSWORD      "1053866507"
#define CONFIG_ESP_MAXIMUM_RETRY  5

static EventGroupHandle_t event_group; // Manejador del grupo de eventos
#define EVENT_SLEEP_MODE_ON (1 << 0) // Define un bit específico para el evento

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;

static const char *TAG = "MAIN";

static void
log_handler(avs_log_level_t level, const char *module, const char *msg) {
    esp_log_level_t esp_level = ESP_LOG_NONE;
    switch (level) {
    case AVS_LOG_QUIET:
        esp_level = ESP_LOG_NONE;
        break;
    case AVS_LOG_ERROR:
        esp_level = ESP_LOG_ERROR;
        break;
    case AVS_LOG_WARNING:
        esp_level = ESP_LOG_WARN;
        break;
    case AVS_LOG_INFO:
        esp_level = ESP_LOG_INFO;
        break;
    case AVS_LOG_DEBUG:
        esp_level = ESP_LOG_DEBUG;
        break;
    case AVS_LOG_TRACE:
        esp_level = ESP_LOG_VERBOSE;
        break;
    }
    ESP_LOG_LEVEL_LOCAL(esp_level, "anjay", "%s", msg);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == ETH_EVENT) {
        if (event_id == ETHERNET_EVENT_START) return;
        if (event_id == ETHERNET_EVENT_STOP) return;
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void register_wifi(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
         .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

static esp_netif_t *init_openthread_netif(const esp_openthread_platform_config_t *config)
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&cfg);
    assert(netif != NULL);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(config)));

    return netif;
}

#define TIMER_WAKEUP_TIME_US    (20 * 1000 * 1000)
void trigger_event_task(void *pvParameter) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20*1000));
        ESP_LOGI(TAG, "Triggering event...");
        xEventGroupSetBits(event_group, EVENT_SLEEP_MODE_ON); 
    }
}

// Tarea que espera el evento
void event_listener_task(void *pvParameter) {

    // Crea el grupo de eventos
    event_group = xEventGroupCreate();
    if (event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return;
    }

    while (1) {
        ESP_LOGI(TAG, "Waiting for event..."); // xEventGroupSetBits(event_group, EVENT_SLEEP_MODE_ON); // Event triggered
        xEventGroupWaitBits(
            event_group,        // Grupo de eventos
            EVENT_SLEEP_MODE_ON,          // Bit que esperamos
            pdTRUE,             // Limpia el bit después de manejarlo
            pdFALSE,            // No espera todos los bits
            portMAX_DELAY       // Tiempo de espera infinito
        );

        ESP_LOGI(TAG, "Event received! Executing task...");

        printf("Entering light sleep\n");
        /* Get timestamp before entering sleep */
        int64_t t_before_us = esp_timer_get_time();
        /* Enter sleep mode */
        esp_light_sleep_start();
        /* Get timestamp after waking up from sleep */
        int64_t t_after_us = esp_timer_get_time();

        /* Determine wake up reason */
        const char* wakeup_reason;
        switch (esp_sleep_get_wakeup_cause()) {
            case ESP_SLEEP_WAKEUP_TIMER:
                wakeup_reason = "timer";
                break;
            default:
                wakeup_reason = "other";
                break;
        }
        printf("Returned from light sleep, reason: %s, t=%lld ms, slept for %lld ms\n",wakeup_reason, t_after_us / 1000, (t_after_us - t_before_us) / 1000);
    
        esp_system_abort("Rebooting ...");
    }
}

static void ot_task_worker(void *aContext)
{
        // Used eventfds:
    // * netif
    // * ot task queue
    // * radio driver
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3,
    };

    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };

    ESP_ERROR_CHECK(esp_openthread_init(&config));

    (void)otLoggingSetLevel(CONFIG_LOG_DEFAULT_LEVEL);

    esp_netif_t *openthread_netif;
    // Initialize the esp_netif bindings
    openthread_netif = init_openthread_netif(&config);
    esp_netif_set_default_netif(openthread_netif);

    otThreadSetNetworkName(esp_openthread_get_instance(),CONFIG_OPENTHREAD_NETWORK_NAME);
    otLinkSetPanId(esp_openthread_get_instance(),CONFIG_OPENTHREAD_NETWORK_PANID);
    otLinkSetChannel(esp_openthread_get_instance(),CONFIG_OPENTHREAD_NETWORK_CHANNEL);

    otMeshLocalPrefix meshlocalprefix;
    datahex(CONFIG_OPENTHREAD_NETWORK_EXTPANID, &meshlocalprefix.m8[0], 8);
    otThreadSetMeshLocalPrefix(esp_openthread_get_instance(),(const otMeshLocalPrefix *)&meshlocalprefix);

    otExtendedPanId extendedPanid;
    datahex(CONFIG_OPENTHREAD_NETWORK_EXTPANID, &extendedPanid.m8[0], 8);
	otThreadSetExtendedPanId(esp_openthread_get_instance(), (const otExtendedPanId *)&extendedPanid);

    otNetworkKey masterKey;
	datahex(CONFIG_OPENTHREAD_NETWORK_PSKC, &masterKey.m8[0], 16);
    otThreadSetNetworkKey(esp_openthread_get_instance(), (const otNetworkKey *)&masterKey);

    otIp6SetEnabled(esp_openthread_get_instance(), true);
    otThreadSetEnabled(esp_openthread_get_instance(), true);

    esp_openthread_cli_init();
    esp_cli_custom_command_init();
    esp_openthread_cli_create_task();

    esp_openthread_launch_mainloop();

        // Clean up
    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(openthread_netif);

    esp_vfs_eventfd_unregister();

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //register_wifi();

    xTaskCreate(ot_task_worker, "ot_cli_main", 10240, xTaskGetCurrentTaskHandle(), 5, NULL);

    avs_log_set_handler(log_handler);
    avs_log_set_default_level(AVS_LOG_TRACE);

    anjay_init();
    xTaskCreate(&anjay_task, "anjay_task", 16384, NULL, 5, NULL);
    

    //esp_sleep_enable_timer_wakeup(TIMER_WAKEUP_TIME_US);
    //xTaskCreate(trigger_event_task, "Trigger Event Task", 2048, NULL, 5, NULL);
    //xTaskCreate(event_listener_task, "Event Listener Task", 2048, NULL, 5, NULL);
}
