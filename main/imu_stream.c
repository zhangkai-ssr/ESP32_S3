/*
 * imu_stream.c – LSM9DS1TR 9-axis raw data streaming over WiFi TCP (CLIENT mode)
 *
 * Architecture:
 *   imu_sample_task   – 200 Hz, driven by esp_timer ISR + binary semaphore.
 *                       Reads 9-axis raw counts and pushes to a FreeRTOS queue.
 *   tcp_client_task   – Connects to the host (Orange Pi AP gateway), drains
 *                       the queue in batches of IMU_BATCH_SAMPLES, sends one
 *                       TCP packet per batch. Reconnects on failure.
 *
 * TCP Packet format (211 bytes for IMU_BATCH_SAMPLES = 10):
 *   [0]       0xBB          header  (IMU frame, distinct from EMG 0xAA)
 *   [1]       0x01          version
 *   [2]       0x20          type  (IMU)
 *   [3..4]    uint16_t LE   sequence number
 *   [5..8]    uint32_t LE   timestamp of first sample (µs, esp_timer_get_time)
 *   [9]       uint8_t       n_samples (= IMU_BATCH_SAMPLES)
 *   [10..]    n × 20 bytes  per-sample layout (doc §5.2):
 *                             ax ay az gx gy gz mx my mz rsvd  (int16_t LE each)
 *   [last]    0x55          footer
 *
 * Packet rate: 200 Hz / 10 = 20 packets/s
 * Host: 10.42.0.1:3334  (ADS1298 EMG uses :3333)
 *
 * Sensor config: AG 238 Hz ±4g / ±500 dps,  M 40 Hz ±8 gauss
 */

#include "imu_stream.h"
#include "lsm9ds1.h"

#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "imu_stream";

/* ---- Protocol constants ---- */
#define HOST_IP             "10.42.0.1"
#define HOST_PORT           3334
#define RECONNECT_DELAY_MS  1000
#define IMU_SAMPLE_HZ       200
#define IMU_BATCH_SAMPLES   10        /* samples per TCP packet → 20 pkt/s */

#define PACKET_HEADER       0xBB
#define PACKET_FOOTER       0x55
#define PACKET_VERSION      0x01
#define PACKET_TYPE_IMU     0x20

#define SAMPLE_BYTES        20        /* ax ay az gx gy gz mx my mz rsvd  × int16_t (doc §5.2) */
#define PACKET_OVERHEAD     (1+1+1+2+4+1+1)  /* hdr+ver+type+seq+ts+n+ftr */
#define PACKET_SIZE         (PACKET_OVERHEAD + IMU_BATCH_SAMPLES * SAMPLE_BYTES)

/* ---- Internal types ---- */
typedef struct {
    int16_t  ax, ay, az;
    int16_t  gx, gy, gz;
    int16_t  mx, my, mz;
    uint32_t ts_us;
} imu_sample_t;

/* ---- Module state ---- */
#define QUEUE_DEPTH    (IMU_BATCH_SAMPLES * 8)

static SemaphoreHandle_t s_timer_sem;
static QueueHandle_t     s_queue;
static uint16_t          s_seq;
static uint8_t           s_packet[PACKET_SIZE];
static volatile bool     s_client_active;
static volatile uint32_t s_read_fail_count;

static void log_imu_memory(const char *stage)
{
    UBaseType_t queued = s_queue ? uxQueueMessagesWaiting(s_queue) : 0;

    ESP_LOGI(TAG,
             "MEM[%s] free=%uB min=%uB stack=%uB queue=%lu/%u packet=%uB",
             stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned long)queued,
             (unsigned)QUEUE_DEPTH,
             (unsigned)sizeof(s_packet));
}

/* ---- 200 Hz timer callback (task context, ESP_TIMER_TASK) ---- */
static void imu_timer_cb(void *arg)
{
    xSemaphoreGive(s_timer_sem);
}

/* ---- Sampling task: blocks on semaphore, reads IMU, enqueues ---- */
static void imu_sample_task(void *pv)
{
    /* Create and start a periodic esp_timer (task dispatch, IDF v4/v5 compatible) */
    esp_timer_handle_t timer;
    const esp_timer_create_args_t timer_args = {
        .callback        = imu_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "imu_200hz",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, (uint64_t)(1000000 / IMU_SAMPLE_HZ)));

    uint32_t dropped = 0;
    while (1) {
        if (xSemaphoreTake(s_timer_sem, pdMS_TO_TICKS(20)) != pdTRUE) {
            ESP_LOGW(TAG, "timer sem timeout");
            continue;
        }

        lsm9ds1_data_t raw;
        if (lsm9ds1_read(&raw) != ESP_OK) {
            s_read_fail_count++;
            if (s_read_fail_count == 1 || s_read_fail_count % 500 == 0) {
                ESP_LOGE(TAG, "lsm9ds1_read failed (count=%lu)",
                         (unsigned long)s_read_fail_count);
            }
            continue;
        }

        imu_sample_t s = {
            .ax    = raw.ax, .ay = raw.ay, .az = raw.az,
            .gx    = raw.gx, .gy = raw.gy, .gz = raw.gz,
            .mx    = raw.mx, .my = raw.my, .mz = raw.mz,
            .ts_us = (uint32_t)esp_timer_get_time(),
        };

        if (!s_client_active) {
            continue;   /* no consumer – discard silently */
        }

        if (xQueueSend(s_queue, &s, 0) != pdTRUE) {
            dropped++;
            if (dropped % 200 == 1) {
                ESP_LOGW(TAG, "queue full, dropped %lu samples", (unsigned long)dropped);
            }
        }
    }
}

/* ---- Build one packet from IMU_BATCH_SAMPLES queued samples ---- */
static size_t build_packet(uint8_t *buf)
{
    imu_sample_t samples[IMU_BATCH_SAMPLES];
    for (int i = 0; i < IMU_BATCH_SAMPLES; i++) {
        if (xQueueReceive(s_queue, &samples[i], pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGW(TAG, "queue receive timeout at sample %d", i);
            return 0;
        }
    }

    uint16_t seq = s_seq++;
    uint32_t ts  = samples[0].ts_us;
    size_t   off = 0;

    buf[off++] = PACKET_HEADER;
    buf[off++] = PACKET_VERSION;
    buf[off++] = PACKET_TYPE_IMU;
    buf[off++] = (uint8_t)(seq & 0xFF);
    buf[off++] = (uint8_t)(seq >> 8);
    buf[off++] = (uint8_t)(ts & 0xFF);
    buf[off++] = (uint8_t)((ts >>  8) & 0xFF);
    buf[off++] = (uint8_t)((ts >> 16) & 0xFF);
    buf[off++] = (uint8_t)((ts >> 24) & 0xFF);
    buf[off++] = (uint8_t)IMU_BATCH_SAMPLES;

    for (int i = 0; i < IMU_BATCH_SAMPLES; i++) {
        buf[off++] = (uint8_t)(samples[i].ax & 0xFF);
        buf[off++] = (uint8_t)(samples[i].ax >> 8);
        buf[off++] = (uint8_t)(samples[i].ay & 0xFF);
        buf[off++] = (uint8_t)(samples[i].ay >> 8);
        buf[off++] = (uint8_t)(samples[i].az & 0xFF);
        buf[off++] = (uint8_t)(samples[i].az >> 8);
        buf[off++] = (uint8_t)(samples[i].gx & 0xFF);
        buf[off++] = (uint8_t)(samples[i].gx >> 8);
        buf[off++] = (uint8_t)(samples[i].gy & 0xFF);
        buf[off++] = (uint8_t)(samples[i].gy >> 8);
        buf[off++] = (uint8_t)(samples[i].gz & 0xFF);
        buf[off++] = (uint8_t)(samples[i].gz >> 8);
        buf[off++] = (uint8_t)(samples[i].mx & 0xFF);
        buf[off++] = (uint8_t)(samples[i].mx >> 8);
        buf[off++] = (uint8_t)(samples[i].my & 0xFF);
        buf[off++] = (uint8_t)(samples[i].my >> 8);
        buf[off++] = (uint8_t)(samples[i].mz & 0xFF);
        buf[off++] = (uint8_t)(samples[i].mz >> 8);
        buf[off++] = 0x00;  /* reserved [+18] */
        buf[off++] = 0x00;  /* reserved [+19] */
    }

    buf[off++] = PACKET_FOOTER;
    return off; /* should equal PACKET_SIZE */
}

/* Connect to the host. Blocks (with retry) until socket is up. */
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
        ESP_LOGI(TAG, "IMU connected to %s:%d, streaming @ %d Hz (9-axis)",
                 HOST_IP, HOST_PORT, IMU_SAMPLE_HZ);
        log_imu_memory("connected");
        return sock;
    }
}

/* ---- TCP client task ---- */
static void tcp_client_task(void *pv)
{
    ESP_LOGI(TAG, "IMU TCP client target: %s:%d (%dB/pkt, %d pkt/s, LSM9DS1TR 9-axis)",
             HOST_IP, HOST_PORT, PACKET_SIZE, IMU_SAMPLE_HZ / IMU_BATCH_SAMPLES);

    while (1) {
        int sock = connect_to_host();

        /* Flush stale queued samples so host always gets fresh data */
        xQueueReset(s_queue);
        s_client_active = true;

        while (1) {
            size_t len = build_packet(s_packet);
            if (len == 0) {
                ESP_LOGE(TAG, "build_packet failed");
                break;
            }
            if (send(sock, (const char *)s_packet, (int)len, 0) < 0) {
                ESP_LOGW(TAG, "send() failed: errno=%d, reconnecting", errno);
                log_imu_memory("send_failed");
                break;
            }
        }

        s_client_active = false;
        shutdown(sock, 0);
        close(sock);
        log_imu_memory("disconnected");
        ESP_LOGI(TAG, "IMU connection dropped, will reconnect");
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

/* ---- Public API ---- */
void imu_stream_init(void)
{
    s_seq       = 0;
    s_timer_sem = xSemaphoreCreateBinary();
    s_queue     = xQueueCreate(QUEUE_DEPTH, sizeof(imu_sample_t));
    configASSERT(s_timer_sem);
    configASSERT(s_queue);
    ESP_LOGI(TAG,
             "STATIC[init] queue_item=%uB queue_depth=%uB queue_storage~=%uB packet=%uB",
             (unsigned)sizeof(imu_sample_t),
             (unsigned)QUEUE_DEPTH,
             (unsigned)(sizeof(imu_sample_t) * QUEUE_DEPTH),
             (unsigned)sizeof(s_packet));
}

void imu_stream_start(void)
{
    /* Priority 7: higher than TCP task (5) so sampling is not starved */
    xTaskCreate(imu_sample_task, "imu_sample", 4096, NULL, 7, NULL);
    xTaskCreate(tcp_client_task, "imu_tcp",    4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "IMU stream tasks started");
}
