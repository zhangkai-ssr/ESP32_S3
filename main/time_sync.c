/*
 * time_sync.c – UDP four-timestamp time synchronisation (NTP-lite style)
 *
 * Protocol (UDP port 3332):
 *
 *   Request  (18 B):  magic(4) ver(1) type=0x01(1) device_id(1) rsvd(1)
 *                     seq(2 LE) t1_us(8 LE signed)
 *
 *   Response (34 B):  magic(4) ver(1) type=0x02(1) device_id(1) rsvd(1)
 *                     seq(2 LE) t1_us(8 LE) t2_us(8 LE) t3_us(8 LE)
 *
 * Four-timestamp algorithm:
 *   T1 = MCU sends request        (esp_timer_get_time)
 *   T2 = host receives request    (host monotonic, returned by server)
 *   T3 = host sends response      (host monotonic, returned by server)
 *   T4 = MCU receives response    (esp_timer_get_time)
 *
 *   rtt_us    = (T4 - T1) - (T3 - T2)
 *   offset_us = ((T2 - T1) + (T3 - T4)) / 2
 *
 *   synced_time_us = esp_timer_get_time() + offset_us
 *
 * Startup: 20 burst rounds, keep the best (min-RTT) offset as initial lock.
 * Running: re-sync every CONFIG_TIME_SYNC_PERIOD_MS (default 2 s), apply
 *          a low-pass filter so the offset cannot jump suddenly.
 */

#include "time_sync.h"

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "sdkconfig.h"

/* ---- Kconfig defaults ---- */
#ifndef CONFIG_TIME_SYNC_SERVER_IP
#define CONFIG_TIME_SYNC_SERVER_IP  "10.245.73.12"
#endif
#ifndef CONFIG_DEVICE_ID
#define CONFIG_DEVICE_ID            1
#endif
#ifndef CONFIG_TIME_SYNC_PERIOD_MS
#define CONFIG_TIME_SYNC_PERIOD_MS  2000
#endif

/* ---- Protocol constants ---- */
#define UDP_PORT        3332
#define RECV_TIMEOUT_MS 200
#define INIT_ROUNDS     20
#define FILTER_ALPHA    0.15   /* low-pass weight for running updates */

#define MAGIC_0  'T'
#define MAGIC_1  'S'
#define MAGIC_2  'Y'
#define MAGIC_3  'N'

#define PKT_VERSION  0x01
#define REQ_TYPE     0x01
#define RSP_TYPE     0x02

#define REQ_SIZE  18
#define RSP_SIZE  34

static const char *TAG = "time_sync";

/* ---- Shared state, protected by s_mux ---- */
static portMUX_TYPE s_mux       = portMUX_INITIALIZER_UNLOCKED;
static int64_t      s_offset_us = 0;
static bool         s_locked    = false;

/* ---- Low-level helpers ---- */
static inline void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static inline void write_le64(uint8_t *p, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(u & 0xFF);
        u >>= 8;
    }
}

static inline int64_t read_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return (int64_t)v;
}

static inline uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ---- Perform one sync round; returns true on success ---- */
static bool do_sync_round(int sock, struct sockaddr_in *srv,
                          uint16_t seq,
                          int64_t *out_rtt_us, int64_t *out_offset_us)
{
    uint8_t req[REQ_SIZE];
    req[0] = MAGIC_0;
    req[1] = MAGIC_1;
    req[2] = MAGIC_2;
    req[3] = MAGIC_3;
    req[4] = PKT_VERSION;
    req[5] = REQ_TYPE;
    req[6] = (uint8_t)CONFIG_DEVICE_ID;
    req[7] = 0x00;
    write_le16(req + 8, seq);

    int64_t t1 = esp_timer_get_time();
    write_le64(req + 10, t1);

    if (sendto(sock, req, REQ_SIZE, 0, (struct sockaddr *)srv, sizeof(*srv)) < 0) {
        ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
        return false;
    }

    uint8_t rsp[RSP_SIZE];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(sock, rsp, RSP_SIZE, 0,
                     (struct sockaddr *)&from, &from_len);
    int64_t t4 = esp_timer_get_time();

    if (n < 0) {
        /* EAGAIN / timeout */
        return false;
    }
    if (n != RSP_SIZE) {
        ESP_LOGW(TAG, "short response: got %d B", n);
        return false;
    }

    /* Validate magic */
    if (rsp[0] != MAGIC_0 || rsp[1] != MAGIC_1 ||
        rsp[2] != MAGIC_2 || rsp[3] != MAGIC_3) {
        ESP_LOGW(TAG, "bad magic in response");
        return false;
    }
    if (rsp[4] != PKT_VERSION || rsp[5] != RSP_TYPE) {
        ESP_LOGW(TAG, "bad version/type in response");
        return false;
    }
    /* Sequence echo */
    if (read_le16(rsp + 8) != seq) {
        ESP_LOGW(TAG, "seq mismatch: sent %u got %u", seq, read_le16(rsp + 8));
        return false;
    }

    /* Extract T2 (host rx) and T3 (host tx) from response */
    /* [10..17] = t1_us echo, [18..25] = t2_us, [26..33] = t3_us */
    int64_t t2 = read_le64(rsp + 18);
    int64_t t3 = read_le64(rsp + 26);

    int64_t rtt    = (t4 - t1) - (t3 - t2);
    int64_t offset = ((t2 - t1) + (t3 - t4)) / 2;

    *out_rtt_us    = rtt;
    *out_offset_us = offset;
    return true;
}

/* ---- Time sync background task ---- */
static void time_sync_task(void *pv)
{
    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons(UDP_PORT),
    };
    if (inet_aton(CONFIG_TIME_SYNC_SERVER_IP, &srv.sin_addr) == 0) {
        ESP_LOGE(TAG, "invalid server IP: %s", CONFIG_TIME_SYNC_SERVER_IP);
        vTaskDelete(NULL);
        return;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {
        .tv_sec  = 0,
        .tv_usec = RECV_TIMEOUT_MS * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* ---- Initial burst: INIT_ROUNDS rounds, keep best (min RTT) ---- */
    ESP_LOGI(TAG, "starting %d-round init burst → %s:%d  device_id=%d",
             INIT_ROUNDS, CONFIG_TIME_SYNC_SERVER_IP, UDP_PORT, CONFIG_DEVICE_ID);

    int64_t  best_rtt    = INT64_MAX;
    int64_t  best_offset = 0;
    int      success     = 0;
    uint16_t seq         = 0;

    for (int i = 0; i < INIT_ROUNDS; i++) {
        int64_t rtt, offset;
        if (do_sync_round(sock, &srv, seq++, &rtt, &offset)) {
            success++;
            if (rtt < best_rtt) {
                best_rtt    = rtt;
                best_offset = offset;
            }
            ESP_LOGI(TAG, "init[%2d/%d] rtt=%7lld µs  offset=%+lld µs",
                     i + 1, INIT_ROUNDS, (long long)rtt, (long long)offset);
        } else {
            ESP_LOGW(TAG, "init[%2d/%d] no response", i + 1, INIT_ROUNDS);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (success > 0) {
        taskENTER_CRITICAL(&s_mux);
        s_offset_us = best_offset;
        s_locked    = true;
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "locked: offset=%+lld µs  best_rtt=%lld µs  (%d/%d OK)",
                 (long long)best_offset, (long long)best_rtt, success, INIT_ROUNDS);
    } else {
        ESP_LOGW(TAG, "init burst: all %d rounds timed out — streams proceed with raw MCU time",
                 INIT_ROUNDS);
    }

    /* ---- Periodic re-sync with low-pass filter ---- */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_TIME_SYNC_PERIOD_MS));

        int64_t rtt, offset;
        if (do_sync_round(sock, &srv, seq++, &rtt, &offset)) {
            taskENTER_CRITICAL(&s_mux);
            int64_t old = s_offset_us;
            /* new = old*(1-α) + new*α  (integer arithmetic) */
            s_offset_us = old + (int64_t)((double)(offset - old) * FILTER_ALPHA);
            if (!s_locked) {
                s_locked = true;
            }
            taskEXIT_CRITICAL(&s_mux);
            ESP_LOGD(TAG, "periodic: rtt=%lld µs  raw_offset=%+lld µs  filtered=%+lld µs",
                     (long long)rtt, (long long)offset, (long long)s_offset_us);
        } else {
            ESP_LOGD(TAG, "periodic sync: no response");
        }
    }
}

/* ---- Public API ---- */

void time_sync_start(void)
{
    xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "time sync task started");
}

int64_t time_sync_now_us(void)
{
    taskENTER_CRITICAL(&s_mux);
    int64_t off = s_offset_us;
    taskEXIT_CRITICAL(&s_mux);
    return esp_timer_get_time() + off;
}

int64_t time_sync_offset_us(void)
{
    taskENTER_CRITICAL(&s_mux);
    int64_t off = s_offset_us;
    taskEXIT_CRITICAL(&s_mux);
    return off;
}

bool time_sync_is_locked(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool locked = s_locked;
    taskEXIT_CRITICAL(&s_mux);
    return locked;
}
