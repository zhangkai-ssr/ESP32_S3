#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>

#define EXAMPLE_ESP_WIFI_SSID      "S50"
#define EXAMPLE_ESP_WIFI_PASS      "12345678qwe"

#define ADS1298_DEVICE_COUNT        2
#define ADS1298_FRAME_BYTES         27
#define ADS1298_PACKET_HEADER       0xAA
#define ADS1298_PACKET_FOOTER       0x55
#define ADS1298_PACKET_VERSION      0x02
#define ADS1298_TIMESTAMP_BYTES     4
#define ADS1298_PACKET_PAYLOAD_SIZE (ADS1298_DEVICE_COUNT * ADS1298_FRAME_BYTES)
#define ADS1298_PACKET_SIZE         (1 + 1 + 2 + ADS1298_TIMESTAMP_BYTES + 1 + ADS1298_PACKET_PAYLOAD_SIZE + 1)
#define ADS1298_TCP_PORT            3333
#define ADS1298_BATCH_FRAMES        50
#define ADS1298_BATCH_BYTES         (ADS1298_PACKET_SIZE * ADS1298_BATCH_FRAMES)
#define ADS1298_TARGET_RATE_BPS     500000

static const char *TAG = "ads1298_tcp";
static uint16_t ads1298_packet_seq = 0;
static uint8_t ads1298_batch_buf[ADS1298_BATCH_BYTES];

static void ads1298_tcp_server_task(void *pvParameters);
static size_t build_ads1298_packet(uint8_t *packet, size_t capacity);
static size_t build_ads1298_batch(uint8_t *buffer, size_t capacity, uint16_t frame_count);

static size_t build_ads1298_packet(uint8_t *packet, size_t capacity)
{
    if (capacity < ADS1298_PACKET_SIZE) {
        return 0;
    }

    uint16_t seq = ads1298_packet_seq++;
    uint32_t timestamp_us = (uint32_t)esp_timer_get_time();
    packet[0] = ADS1298_PACKET_HEADER;
    packet[1] = ADS1298_PACKET_VERSION;
    packet[2] = (uint8_t)(seq & 0xFF);
    packet[3] = (uint8_t)((seq >> 8) & 0xFF);
    packet[4] = (uint8_t)(timestamp_us & 0xFF);
    packet[5] = (uint8_t)((timestamp_us >> 8) & 0xFF);
    packet[6] = (uint8_t)((timestamp_us >> 16) & 0xFF);
    packet[7] = (uint8_t)((timestamp_us >> 24) & 0xFF);
    packet[8] = (uint8_t)ADS1298_DEVICE_COUNT;

    for (int dev = 0; dev < ADS1298_DEVICE_COUNT; dev++) {
        size_t base = 9 + dev * ADS1298_FRAME_BYTES;
        packet[base + 0] = 0xC0;
        packet[base + 1] = 0x00;
        packet[base + 2] = (uint8_t)dev;

        for (int ch = 0; ch < 8; ch++) {
            int32_t sample = (int32_t)((seq * 37) + (dev * 1000) + (ch * 83));
            sample = (sample & 0x7FFFFF) - 0x3FFFFF;
            size_t idx = base + 3 + ch * 3;
            packet[idx + 0] = (uint8_t)((sample >> 16) & 0xFF);
            packet[idx + 1] = (uint8_t)((sample >> 8) & 0xFF);
            packet[idx + 2] = (uint8_t)(sample & 0xFF);
        }
    }

    packet[ADS1298_PACKET_SIZE - 1] = ADS1298_PACKET_FOOTER;
    return ADS1298_PACKET_SIZE;
}

static size_t build_ads1298_batch(uint8_t *buffer, size_t capacity, uint16_t frame_count)
{
    size_t total = (size_t)frame_count * ADS1298_PACKET_SIZE;
    if (capacity < total) {
        return 0;
    }

    for (uint16_t i = 0; i < frame_count; i++) {
        if (build_ads1298_packet(buffer + i * ADS1298_PACKET_SIZE, ADS1298_PACKET_SIZE) == 0) {
            return 0;
        }
    }

    return total;
}

static void ads1298_tcp_server_task(void *pvParameters)
{
    int listen_sock = -1;
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ADS1298_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "TCP socket create failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "TCP bind failed: errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "TCP listen failed: errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "ADS1298 TCP stream server started on port %d", ADS1298_TCP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            ESP_LOGW(TAG, "TCP accept failed: errno=%d", errno);
            continue;
        }

        ESP_LOGI(TAG, "TCP client connected, streaming batched data at %d B/s", ADS1298_TARGET_RATE_BPS);

        int64_t next_send_us = esp_timer_get_time();
        const int64_t batch_interval_us = ((int64_t)ADS1298_BATCH_BYTES * 1000000LL) / ADS1298_TARGET_RATE_BPS;

        while (1) {
            size_t batch_len = build_ads1298_batch(ads1298_batch_buf, sizeof(ads1298_batch_buf), ADS1298_BATCH_FRAMES);
            if (batch_len == 0) {
                ESP_LOGE(TAG, "Build batch failed");
                break;
            }

            int sent = send(client_sock, (const char *)ads1298_batch_buf, (int)batch_len, 0);
            if (sent < 0) {
                ESP_LOGW(TAG, "TCP send failed: errno=%d", errno);
                break;
            }

            next_send_us += batch_interval_us;
            int64_t now_us = esp_timer_get_time();
            if (next_send_us > now_us) {
                int64_t sleep_us = next_send_us - now_us;
                vTaskDelay(pdMS_TO_TICKS((sleep_us + 999) / 1000));
            } else {
                next_send_us = now_us;
            }
        }

        shutdown(client_sock, 0);
        close(client_sock);
        ESP_LOGI(TAG, "TCP client disconnected");
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Connecting WiFi...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi disconnected, reason=%d, retrying...", disconn->reason);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    strcpy((char *)wifi_config.sta.ssid, EXAMPLE_ESP_WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, EXAMPLE_ESP_WIFI_PASS);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();

    ESP_LOGI(TAG, "Waiting for WiFi...");
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK) {
            if (ip_info.ip.addr != 0) {
                break;
            }
        }
    }

    xTaskCreate(ads1298_tcp_server_task, "ads1298_tcp", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "System ready: TCP-only mode");

    while (1) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}
