/*
 * imu_stream.c – LSM9DS1TR 9-axis raw data streaming over WiFi TCP
 *
 * Architecture:
 *   imu_sample_task   – 200 Hz, driven by esp_timer ISR + binary semaphore.
 *                       Reads 9-axis raw counts and pushes to a FreeRTOS queue.
 *   tcp_server_task   – Waits for a client, drains the queue in batches of
 *                       IMU_BATCH_SAMPLES and sends one TCP packet per batch.
 *
 * TCP Packet format v0x02 (215 bytes for IMU_BATCH_SAMPLES = 10):
 *   [0]       0xBB          header  (IMU frame, distinct from EMG 0xAA)
 *   [1]       0x02          version
 *   [2]       0x20          type  (IMU)
 *   [3..4]    uint16_t LE   sequence number
 *   [5..12]   int64_t LE    64-bit MCU timestamp of first sample (µs, esp_timer_get_time)
 *   [13]      uint8_t       n_samples (= IMU_BATCH_SAMPLES)
 *   [14..]    n × 20 bytes  per-sample layout (doc §5.2):
 *                             ax ay az gx gy gz mx my mz rsvd  (int16_t LE each)
 *   [last]    0x55          footer
 *
 * Packet rate: 200 Hz / 10 = 20 packets/s
 * Port: 3334  (ADS1298 EMG uses 3333)
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
#include "time_sync.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "imu_stream";

/* ---- Protocol constants ---- */
#define IMU_TCP_PORT        3334
#define IMU_SAMPLE_HZ       200
#define IMU_BATCH_SAMPLES   10        /* samples per TCP packet → 20 pkt/s */

#define PACKET_HEADER       0xBB
#define PACKET_FOOTER       0x55
#define PACKET_VERSION      0x02
#define PACKET_TYPE_IMU     0x20

#define SAMPLE_BYTES        20        /* ax ay az gx gy gz mx my mz rsvd  × int16_t (doc §5.2) */
#define PACKET_OVERHEAD     (1+1+1+2+8+1+1)  /* hdr+ver+type+seq+ts64+n+ftr */
#define PACKET_SIZE         (PACKET_OVERHEAD + IMU_BATCH_SAMPLES * SAMPLE_BYTES)

/* ---- Internal types ---- */
typedef struct {
    int16_t  ax, ay, az;
    int16_t  gx, gy, gz;
    int16_t  mx, my, mz;
    int64_t  ts_us;
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

/* ---- 200 Hz timer callback (ISR context, ESP_TIMER_ISR) ---- */
static void IRAM_ATTR imu_timer_cb(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_timer_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---- Sampling task: blocks on semaphore, reads IMU, enqueues ---- */
static void imu_sample_task(void *pv)
{
    /* Create and start a periodic esp_timer (task dispatch, IDF v4/v5 compatible) */
    esp_timer_handle_t timer;
    const esp_timer_create_args_t timer_args = {
        .callback        = imu_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_ISR,
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
        /* Timestamp at timer fire = intended 200 Hz sample time, before I2C read latency */
        int64_t sample_ts = time_sync_now_us();

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
            .ts_us = sample_ts,
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
    uint64_t ts  = (uint64_t)samples[0].ts_us;
    size_t   off = 0;

    buf[off++] = PACKET_HEADER;
    buf[off++] = PACKET_VERSION;
    buf[off++] = PACKET_TYPE_IMU;
    buf[off++] = (uint8_t)(seq & 0xFF);
    buf[off++] = (uint8_t)(seq >> 8);
    /* 64-bit MCU timestamp of first sample, little-endian [5..12] */
    buf[off++] = (uint8_t)(ts & 0xFF);
    buf[off++] = (uint8_t)((ts >>  8) & 0xFF);
    buf[off++] = (uint8_t)((ts >> 16) & 0xFF);
    buf[off++] = (uint8_t)((ts >> 24) & 0xFF);
    buf[off++] = (uint8_t)((ts >> 32) & 0xFF);
    buf[off++] = (uint8_t)((ts >> 40) & 0xFF);
    buf[off++] = (uint8_t)((ts >> 48) & 0xFF);
    buf[off++] = (uint8_t)((ts >> 56) & 0xFF);
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

/* ---- TCP server task ---- */
static void tcp_server_task(void *pv)
{
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(IMU_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (lsock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
        close(lsock);
        vTaskDelete(NULL);
        return;
    }
    listen(lsock, 1);
    ESP_LOGI(TAG, "IMU TCP server listening on port %d (%dB/pkt, %d pkt/s, LSM9DS1TR 9-axis)",
             IMU_TCP_PORT, PACKET_SIZE, IMU_SAMPLE_HZ / IMU_BATCH_SAMPLES);
    log_imu_memory("listen");

    while (1) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int csock = accept(lsock, (struct sockaddr *)&cli, &cli_len);
        if (csock < 0) {
            ESP_LOGW(TAG, "accept() failed: errno=%d", errno);
            continue;
        }
        ESP_LOGI(TAG, "IMU client connected – streaming @ %d Hz (9-axis)", IMU_SAMPLE_HZ);
        log_imu_memory("client_connected");

        /* Flush stale queued samples so client always gets fresh data */
        xQueueReset(s_queue);
        s_client_active = true;

        while (1) {
            size_t len = build_packet(s_packet);
            if (len == 0) {
                ESP_LOGE(TAG, "build_packet failed");
                break;
            }
            if (send(csock, (const char *)s_packet, (int)len, 0) < 0) {
                ESP_LOGW(TAG, "send() failed: errno=%d", errno);
                log_imu_memory("send_failed");
                break;
            }
        }

        s_client_active = false;
        shutdown(csock, 0);
        close(csock);
        log_imu_memory("client_disconnected");
        ESP_LOGI(TAG, "IMU client disconnected");
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
    xTaskCreate(tcp_server_task, "imu_tcp",    4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "IMU stream tasks started");
}
