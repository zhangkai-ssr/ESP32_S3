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

#define PACKET_HEADER       0xAA
#define PACKET_FOOTER       0x55
#define PACKET_VERSION      0x02
#define TIMESTAMP_BYTES     4
#define PAYLOAD_SIZE        (ADS1298_CHAIN_DEVICES * ADS1298_DATA_FRAME_SIZE)
#define PACKET_SIZE         (1 + 1 + 2 + TIMESTAMP_BYTES + 1 + PAYLOAD_SIZE + 1)

/* TCP-client mode: connect to the Orange Pi host (AP gateway). */
#define HOST_IP             "10.42.0.1"
#define HOST_PORT           3333
#define RECONNECT_DELAY_MS  1000
#define BATCH_FRAMES        25
#define BATCH_BYTES         (PACKET_SIZE * BATCH_FRAMES)
#define DRDY_TIMEOUT_MS     50

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
    uint32_t ts  = (uint32_t)esp_timer_get_time();

    packet[0] = PACKET_HEADER;
    packet[1] = PACKET_VERSION;
    packet[2] = (uint8_t)(seq & 0xFF);
    packet[3] = (uint8_t)((seq >> 8) & 0xFF);
    packet[4] = (uint8_t)(ts & 0xFF);
    packet[5] = (uint8_t)((ts >> 8) & 0xFF);
    packet[6] = (uint8_t)((ts >> 16) & 0xFF);
    packet[7] = (uint8_t)((ts >> 24) & 0xFF);
    packet[8] = (uint8_t)ADS1298_CHAIN_DEVICES;

    /* raw[0..26]  = U17 frame; raw[27..53] = U3 frame */
    memcpy(packet + 9, raw, PAYLOAD_SIZE);

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

/* Connect to the host. Blocks (with retry) until we have a usable socket. */
static int connect_to_host(void)
{
    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(HOST_PORT),
        .sin_addr.s_addr = inet_addr(HOST_IP),
    };

    while (1) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }
        if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
            ESP_LOGW(TAG, "connect %s:%d failed: errno=%d, retry in %d ms",
                     HOST_IP, HOST_PORT, errno, RECONNECT_DELAY_MS);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }
        int flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
        ESP_LOGI(TAG, "connected to %s:%d, streaming ADS1298 @ 2000 SPS",
                 HOST_IP, HOST_PORT);
        log_stream_memory("connected");
        return sock;
    }
}

static void tcp_client_task(void *pvParameters)
{
    ESP_LOGI(TAG, "EMG TCP client target: %s:%d", HOST_IP, HOST_PORT);

    while (1) {
        int sock = connect_to_host();

        while (1) {
            size_t len = build_batch(s_batch_buf, sizeof(s_batch_buf), BATCH_FRAMES);
            if (len == 0) {
                ESP_LOGE(TAG, "batch build failed");
                break;
            }
            if (send(sock, (const char *)s_batch_buf, (int)len, 0) < 0) {
                ESP_LOGW(TAG, "send failed: errno=%d, reconnecting", errno);
                log_stream_memory("send_failed");
                break;
            }
        }

        shutdown(sock, 0);
        close(sock);
        log_stream_memory("disconnected");
        ESP_LOGI(TAG, "EMG connection dropped, will reconnect");
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
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
    /* Pin to CPU1: WiFi runs on CPU0 by default and aggressively preempts
     * tasks on the same core during TX/RX bursts. Putting the EMG loop on
     * CPU1 isolates the SPI/DRDY-driven sample collection path. Empirically
     * this is the single highest-impact change for sustained EMG throughput. */
    xTaskCreatePinnedToCore(tcp_client_task, "ads1298_tcp", 8192, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "EMG TCP client task started on CPU1");
}
