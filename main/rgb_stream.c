/*
 * rgb_stream.c – 500 KB/s fake RGB pixel data over TCP for time-sync validation.
 *
 * Packet format (RGB_PACKET_SIZE = 525 bytes):
 *   [0]       0xCC        header
 *   [1]       0x01        version
 *   [2]       0x30        type  (RGB test stream)
 *   [3..4]    uint16 LE   sequence number
 *   [5..12]   int64  LE   mcu_ts_us  (time_sync_now_us(), µs)
 *   [13]      uint8       n_pixels   (= RGB_PIXELS_PER_PKT = 170)
 *   [14..]    170 × 3 B   R G B fake pattern
 *   [last]    0x55        footer
 *
 * Target rate: 500 000 B/s  →  ~952 pkt/s  →  interval ≈ 1050 µs/pkt
 * TCP port: 3335
 *
 * Rate control: esp_timer_get_time() token-bucket.  Yields CPU (vTaskDelay(1))
 * when more than 1 ms remains; busy-waits only the final sub-ms slice so the
 * FreeRTOS scheduler is not starved.
 */

#include "rgb_stream.h"
#include "time_sync.h"

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "rgb_stream";

/* ---- Protocol constants ---- */
#define RGB_TCP_PORT        3335
#define RGB_PIXELS_PER_PKT  170          /* 170 × 3 = 510 B payload */
#define RGB_TARGET_BPS      500000UL     /* target throughput in bytes/s */

#define PKT_HEADER   0xCC
#define PKT_FOOTER   0x55
#define PKT_VERSION  0x01
#define PKT_TYPE_RGB 0x30

/* 1(hdr)+1(ver)+1(type)+2(seq)+8(ts64)+1(n)+1(ftr) = 15 bytes overhead */
#define PKT_OVERHEAD     15
#define PKT_PAYLOAD      (RGB_PIXELS_PER_PKT * 3)   /* 510 bytes */
#define RGB_PACKET_SIZE  (PKT_OVERHEAD + PKT_PAYLOAD) /* 525 bytes */

/* µs between packets to hit RGB_TARGET_BPS */
#define SEND_INTERVAL_US  ((uint32_t)((uint64_t)1000000UL * RGB_PACKET_SIZE / RGB_TARGET_BPS))

/* ---- Module state ---- */
static uint8_t  s_packet[RGB_PACKET_SIZE];
static uint16_t s_seq;

/* ---- Build one packet ---- */
static void build_packet(void)
{
    int64_t  ts  = time_sync_now_us();
    uint16_t seq = s_seq++;
    uint64_t ts64 = (uint64_t)ts;
    size_t   off  = 0;

    s_packet[off++] = PKT_HEADER;
    s_packet[off++] = PKT_VERSION;
    s_packet[off++] = PKT_TYPE_RGB;
    s_packet[off++] = (uint8_t)(seq & 0xFF);
    s_packet[off++] = (uint8_t)(seq >> 8);

    /* 64-bit MCU timestamp, little-endian [5..12] */
    for (int i = 0; i < 8; i++) {
        s_packet[off++] = (uint8_t)(ts64 & 0xFF);
        ts64 >>= 8;
    }

    s_packet[off++] = (uint8_t)RGB_PIXELS_PER_PKT;

    /* Fake RGB: R = seq_lo, G = pixel_index, B = ~pixel_index */
    uint8_t r_base = (uint8_t)(seq & 0xFF);
    for (int i = 0; i < RGB_PIXELS_PER_PKT; i++) {
        s_packet[off++] = r_base;
        s_packet[off++] = (uint8_t)(i & 0xFF);
        s_packet[off++] = (uint8_t)((~i) & 0xFF);
    }

    s_packet[off++] = PKT_FOOTER;
    /* off == RGB_PACKET_SIZE */
}

/* ---- TCP server task ---- */
static void rgb_tcp_task(void *pv)
{
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RGB_TCP_PORT),
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
    ESP_LOGI(TAG,
             "RGB test TCP server on port %d  pkt=%dB  target=%lu B/s  interval=%lu µs",
             RGB_TCP_PORT, RGB_PACKET_SIZE,
             (unsigned long)RGB_TARGET_BPS, (unsigned long)SEND_INTERVAL_US);

    while (1) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int csock = accept(lsock, (struct sockaddr *)&cli, &cli_len);
        if (csock < 0) {
            ESP_LOGW(TAG, "accept() failed: errno=%d", errno);
            continue;
        }
        ESP_LOGI(TAG, "RGB test client connected – streaming ~500 KB/s fake RGB");

        s_seq = 0;
        int64_t next_us = esp_timer_get_time();
        uint32_t sent_pkts = 0;

        while (1) {
            /* Rate limiter: yield CPU when > 1 ms away; busy-wait the last slice */
            int64_t now     = esp_timer_get_time();
            int64_t wait_us = next_us - now;
            if (wait_us > 1000) {
                vTaskDelay(1);
            } else if (wait_us > 0) {
                while (esp_timer_get_time() < next_us) { /* sub-ms busy-wait */ }
            }
            next_us += (int64_t)SEND_INTERVAL_US;

            build_packet();
            if (send(csock, (const char *)s_packet, RGB_PACKET_SIZE, 0) < 0) {
                ESP_LOGW(TAG, "send() failed after %lu pkts: errno=%d",
                         (unsigned long)sent_pkts, errno);
                break;
            }
            sent_pkts++;

            if (sent_pkts % 10000 == 0) {
                ESP_LOGI(TAG, "RGB test: sent %lu packets", (unsigned long)sent_pkts);
            }
        }

        shutdown(csock, 0);
        close(csock);
        ESP_LOGI(TAG, "RGB test client disconnected (sent %lu pkts)", (unsigned long)sent_pkts);
    }
}

/* ---- Public API ---- */
void rgb_stream_init(void)
{
    s_seq = 0;
    ESP_LOGI(TAG,
             "rgb_stream: pkt=%dB  target=%lu B/s  interval=%lu µs  pixels=%d",
             RGB_PACKET_SIZE, (unsigned long)RGB_TARGET_BPS,
             (unsigned long)SEND_INTERVAL_US, RGB_PIXELS_PER_PKT);
}

void rgb_stream_start(void)
{
    xTaskCreate(rgb_tcp_task, "rgb_tcp", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "RGB stream test task started  TCP:%d", RGB_TCP_PORT);
}
