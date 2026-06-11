#include "wifi_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#define WIFI_PROFILE 3  /* 1 = CYZX05, 2 = S50, 3 = OrangePi_AP */

#if WIFI_PROFILE == 1
#define WIFI_SSID   "CYZX05"
#define WIFI_PASS   "Rj@cyzx005"
#elif WIFI_PROFILE == 2
#define WIFI_SSID   "S50"
#define WIFI_PASS   "12345678qwe"
#elif WIFI_PROFILE == 3
#define WIFI_SSID   "OrangePi_AP"
#define WIFI_PASS   "op_sensor_2025"
#else
#error "Unknown WIFI_PROFILE, set to 1, 2 or 3"
#endif

static const char *TAG = "wifi_manager";

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Connecting WiFi...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi disconnected, reason=%d, retrying...", disconn->reason);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_manager_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            /* Looser threshold tolerates APs that announce WPA-WPA2 mixed mode.
             * Some ESP32-S3 RF modules fail the 4-way handshake when the AP
             * advertises PMF capability but the STA also announces capable=true
             * — disabling PMF entirely on the STA side fixes it. */
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .capable  = false,
                .required = false,
            },
        },
    };
    strcpy((char *)wifi_config.sta.ssid,     WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
}

void wifi_manager_wait_connected(void)
{
    ESP_LOGI(TAG, "Waiting for WiFi...");
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(
                esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK) {
            if (ip_info.ip.addr != 0) {
                break;
            }
        }
    }
}
