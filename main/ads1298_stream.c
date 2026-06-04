#include "ads1298_stream.h"

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "ads1298.h"
#include "emg_filter.h"
#include "time_sync.h"

#define PACKET_HEADER       0xAA
#define PACKET_FOOTER       0x55
#define PACKET_VERSION      0x03
#define TIMESTAMP_BYTES     8
#define PAYLOAD_SIZE        (ADS1298_CHAIN_DEVICES * ADS1298_DATA_FRAME_SIZE)
#define PACKET_SIZE         (1 + 1 + 2 + TIMESTAMP_BYTES + 1 + PAYLOAD_SIZE + 1)
#define TCP_PORT            3333
#define BATCH_FRAMES        50
#define BATCH_BYTES         (PACKET_SIZE * BATCH_FRAMES)
#define DRDY_TIMEOUT_MS     50
#define DRDY_RECOVER_ATTEMPTS 3

static const char *TAG = "ads1298_stream";

static emg_filter_bank_t s_filter_bank;
static uint16_t          s_seq;
static uint8_t           s_batch_buf[BATCH_BYTES];

static void log_stream_memory(const char *stage)
{
    ESP_LOGI(TAG,
             "MEM[%s] free=%uB min=%uB stack=%uB batch=%uB",
             stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)sizeof(s_batch_buf));
}

/* ------------------------------------------------------------------ */

static size_t build_packet(uint8_t *packet, size_t capacity)
{
    if (capacity < PACKET_SIZE) {
        return 0;
    }

    static uint8_t raw[ADS1298_TOTAL_DATA_SIZE];

    for (int retry = 0; ; retry++) {
        if (ads1298_wait_drdy(DRDY_TIMEOUT_MS) == ESP_OK) break;
        if (retry >= 2) {
            ESP_LOGW(TAG, "DRDY timeout (3 retries)");
            return 0;
        }
        ESP_LOGW(TAG, "DRDY timeout, retry %d", retry + 1);
    }
    /* Timestamp at DRDY assertion = ADC conversion complete, before SPI transfer latency */
    int64_t sample_ts = time_sync_now_us();

    if (ads1298_read_data(raw, sizeof(raw)) != ESP_OK) {
        ESP_LOGE(TAG, "SPI read failed");
        return 0;
    }

    /* Apply 20 Hz HPF + 50/100/150 Hz notch to all 16 channels */
    for (int dev = 0; dev < ADS1298_CHAIN_DEVICES; dev++) {
        for (int ch = 0; ch < 8; ch++) {
            uint8_t *p = raw + dev * ADS1298_DATA_FRAME_SIZE + 3 + ch * 3;
            int32_t sample = (int32_t)((uint32_t)p[0] << 24 |
                                       (uint32_t)p[1] << 16 |
                                       (uint32_t)p[2] << 8) >> 8;
            float fval = emg_filter_bank_process(&s_filter_bank, dev * 8 + ch, (float)sample);
            int32_t out = (int32_t)fval;
            p[0] = (uint8_t)((out >> 16) & 0xFF);
            p[1] = (uint8_t)((out >> 8)  & 0xFF);
            p[2] = (uint8_t)( out        & 0xFF);
        }
    }

    uint16_t seq = s_seq++;
    uint64_t ts  = (uint64_t)sample_ts;

    packet[0] = PACKET_HEADER;
    packet[1] = PACKET_VERSION;
    packet[2] = (uint8_t)(seq & 0xFF);
    packet[3] = (uint8_t)((seq >> 8) & 0xFF);
    /* 64-bit MCU timestamp, little-endian [4..11] */
    packet[4]  = (uint8_t)(ts & 0xFF);
    packet[5]  = (uint8_t)((ts >>  8) & 0xFF);
    packet[6]  = (uint8_t)((ts >> 16) & 0xFF);
    packet[7]  = (uint8_t)((ts >> 24) & 0xFF);
    packet[8]  = (uint8_t)((ts >> 32) & 0xFF);
    packet[9]  = (uint8_t)((ts >> 40) & 0xFF);
    packet[10] = (uint8_t)((ts >> 48) & 0xFF);
    packet[11] = (uint8_t)((ts >> 56) & 0xFF);
    packet[12] = (uint8_t)ADS1298_CHAIN_DEVICES;

    /* raw[0..26]  = U17 frame; raw[27..53] = U3 frame */
    memcpy(packet + 13, raw, PAYLOAD_SIZE);

    packet[PACKET_SIZE - 1] = PACKET_FOOTER;
    return PACKET_SIZE;
}

static size_t build_batch(uint8_t *buf, size_t capacity, uint16_t frames)
{
    size_t total = (size_t)frames * PACKET_SIZE;
    if (capacity < total) {
        return 0;
    }
    for (uint16_t i = 0; i < frames; i++) {
        if (build_packet(buf + i * PACKET_SIZE, PACKET_SIZE) == 0) {
            return 0;
        }
    }
    return total;
}

static void tcp_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "socket create failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "listen failed: errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP stream server listening on port %d", TCP_PORT);
    log_stream_memory("listen");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            ESP_LOGW(TAG, "accept failed: errno=%d", errno);
            continue;
        }

        int flag = 1;
        setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        ESP_LOGI(TAG, "client connected, streaming ADS1298 data @ 2000 SPS");
        log_stream_memory("client_connected");

        while (1) {
            size_t len = build_batch(s_batch_buf, sizeof(s_batch_buf), BATCH_FRAMES);
            if (len == 0) {
                /* Attempt to recover ADS1298 (e.g. exited RDATAC due to SPI noise) */
                bool recovered = false;
                for (int ra = 0; ra < DRDY_RECOVER_ATTEMPTS; ra++) {
                    ESP_LOGW(TAG, "ADS1298 recovery attempt %d/%d", ra + 1, DRDY_RECOVER_ATTEMPTS);
                    if (ads1298_start_conversion() == ESP_OK) {
                        emg_filter_bank_init(&s_filter_bank); /* reset filter state after gap */
                        vTaskDelay(pdMS_TO_TICKS(5));
                        uint8_t probe[PACKET_SIZE];
                        if (build_packet(probe, sizeof(probe)) > 0) {
                            ESP_LOGI(TAG, "ADS1298 recovered after %d attempt(s)", ra + 1);
                            recovered = true;
                            break;
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (!recovered) {
                    ESP_LOGE(TAG, "ADS1298 unrecoverable — dropping client");
                    break;
                }
                continue;
            }
            if (send(client_sock, (const char *)s_batch_buf, (int)len, 0) < 0) {
                ESP_LOGW(TAG, "send failed: errno=%d", errno);
                log_stream_memory("send_failed");
                break;
            }
        }

        shutdown(client_sock, 0);
        close(client_sock);
        log_stream_memory("client_disconnected");
        ESP_LOGI(TAG, "client disconnected");
    }
}

/* ------------------------------------------------------------------ */

void ads1298_stream_init(void)
{
    s_seq = 0;
    emg_filter_bank_init(&s_filter_bank);
    ESP_LOGI(TAG, "STATIC[init] batch=%uB packet=%uB", (unsigned)sizeof(s_batch_buf), (unsigned)PACKET_SIZE);
}

void ads1298_stream_start(void)
{
    xTaskCreate(tcp_server_task, "ads1298_tcp", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "TCP stream task started");
}
