/*
 * led_ctrl.c – LED pattern controller with software PWM
 *
 * Timing:
 *   esp_timer periodic callback at 1 ms → gives binary semaphore
 *   led_task blocks on semaphore → calls pwm_tick() on every 1 ms event
 *   PWM period = PWM_STEPS × 1 ms = 16 ms  (~62.5 Hz, above flicker threshold)
 *
 * Per-tick logic in pwm_tick():
 *   1. Detect mode change → reset phase counter
 *   2. Compute desired r/g/b booleans from current mode + phase
 *   3. Write to PMIC only when the computed state differs from last written
 *      state (minimises I2C transactions)
 */

#include "led_ctrl.h"
#include "npm1300_led.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "led_ctrl";

/* ---- software-PWM constants ---- */
#define PWM_STEPS       16          /* steps per PWM period                 */
#define PWM_TICK_US     1000        /* 1 ms per tick → 16 ms period         */

/* Colour-corrected max duty cycles (out of PWM_STEPS)
 * Derived from hardware calibration: R:G:B = 8:2:4 so that white looks white. */
#define DUTY_R_MAX      8           /* 50 %    */
#define DUTY_G_MAX      2           /* 12.5 %  */
#define DUTY_B_MAX      4           /* 25 %    */

/* Orange = dominant red + a hint of green, no blue */
#define DUTY_G_ORANGE   1           /* 6.25 %  */

/* ---- module-level state (all written/read from led_task only, except
 *      s_mode which is volatile-shared with callers; 32-bit aligned R/W
 *      is naturally atomic on Xtensa LX7) ---- */
/* volatile: 32-bit aligned R/W is naturally atomic on Xtensa LX7 */
static volatile int         s_mode      = LED_MODE_OFF;
static int                  s_cur_mode  = -1;       /* -1 forces first update   */
static uint32_t             s_tick      = 0;        /* absolute ms counter      */
static uint32_t             s_phase     = 0;        /* ms since last mode entry */
static bool                 s_last_r    = false;
static bool                 s_last_g    = false;
static bool                 s_last_b    = false;
static bool                 s_led_ok    = false;    /* false until init succeeds */

static SemaphoreHandle_t    s_sem       = NULL;
static esp_timer_handle_t   s_timer     = NULL;

/* ---- helpers ---- */

/* Return true when the PWM duty cycle is in the ON phase this tick */
static inline bool pwm_on(uint8_t duty)
{
    return duty && ((s_tick % PWM_STEPS) < duty);
}

/* Compute per-channel booleans for a solid colour (on_phase=false → all off) */
static void rgb_solid(uint8_t dr, uint8_t dg, uint8_t db,
                      bool on_phase,
                      bool *r, bool *g, bool *b)
{
    if (!on_phase) {
        *r = *g = *b = false;
        return;
    }
    *r = pwm_on(dr);
    *g = pwm_on(dg);
    *b = pwm_on(db);
}

/* ---- core PWM tick, called every 1 ms from led_task ---- */

static void pwm_tick(void)
{
    int mode = s_mode;

    /* Detect mode change → reset phase counter */
    if (mode != s_cur_mode) {
        s_cur_mode = mode;
        s_phase    = 0;
    }

    bool r = false, g = false, b = false;

    switch ((led_mode_t)mode) {

    case LED_MODE_OFF:
        /* r/g/b stay false */
        break;

    case LED_MODE_PAIRING_WHITE_PULSE_FAST:
        /* 250 ms ON / 250 ms OFF, white */
        rgb_solid(DUTY_R_MAX, DUTY_G_MAX, DUTY_B_MAX,
                  (s_phase % 500) < 250,
                  &r, &g, &b);
        break;

    case LED_MODE_SHUTDOWN_WHITE_FADE_OUT: {
        /* Linear brightness ramp: full white → off over 2000 ms.
         * After 2000 ms the LED stays off (animation complete). */
        if (s_phase < 2000) {
            uint8_t dr = (uint8_t)(DUTY_R_MAX * (2000 - s_phase) / 2000);
            uint8_t dg = (uint8_t)(DUTY_G_MAX * (2000 - s_phase) / 2000);
            uint8_t db = (uint8_t)(DUTY_B_MAX * (2000 - s_phase) / 2000);
            r = pwm_on(dr);
            g = pwm_on(dg);
            b = pwm_on(db);
        }
        break;
    }

    case LED_MODE_FACTORY_RESET:
        /* 250 ms orange / 250 ms white alternating */
        if ((s_phase % 500) < 250) {
            /* white phase */
            rgb_solid(DUTY_R_MAX, DUTY_G_MAX, DUTY_B_MAX, true, &r, &g, &b);
        } else {
            /* orange phase: full R, hint of G, no B */
            rgb_solid(DUTY_R_MAX, DUTY_G_ORANGE, 0, true, &r, &g, &b);
        }
        break;

    case LED_MODE_POWER_YELLOW_ON:
        rgb_solid(DUTY_R_MAX, DUTY_G_MAX, 0, true, &r, &g, &b);
        break;

    case LED_MODE_POWER_YELLOW_BLINK:
        /* 500 ms ON / 500 ms OFF, yellow */
        rgb_solid(DUTY_R_MAX, DUTY_G_MAX, 0,
                  (s_phase % 1000) < 500,
                  &r, &g, &b);
        break;

    case LED_MODE_POWER_GREEN_ON:
        rgb_solid(0, DUTY_G_MAX, 0, true, &r, &g, &b);
        break;

    case LED_MODE_POWER_RED_ON:
        rgb_solid(DUTY_R_MAX, 0, 0, true, &r, &g, &b);
        break;

    case LED_MODE_POWER_RED_BLINK:
        /* 500 ms ON / 500 ms OFF, red */
        rgb_solid(DUTY_R_MAX, 0, 0,
                  (s_phase % 1000) < 500,
                  &r, &g, &b);
        break;

    default:
        break;
    }

    /* Write to PMIC only when state actually changes (saves I2C bandwidth) */
    if (r != s_last_r || g != s_last_g || b != s_last_b) {
        npm1300_led_set_rgb(r, g, b);
        s_last_r = r;
        s_last_g = g;
        s_last_b = b;
    }

    s_tick++;
    s_phase++;
}

/* ---- FreeRTOS task: blocks on semaphore, runs one PWM tick per wakeup ---- */

static void led_task(void *arg)
{
    while (1) {
        xSemaphoreTake(s_sem, portMAX_DELAY);
        pwm_tick();
    }
}

/* ---- esp_timer callback: fires every 1 ms, unblocks led_task ---- */

static void timer_cb(void *arg)
{
    xSemaphoreGive(s_sem);
}

/* ---- Public API ---- */

void led_ctrl_init(void)
{
    esp_err_t ret = npm1300_led_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "npm1300_led_init failed (%s) – LED control disabled",
                 esp_err_to_name(ret));
        return;
    }
    s_led_ok = true;

    s_sem = xSemaphoreCreateBinary();
    if (!s_sem) {
        ESP_LOGE(TAG, "failed to create semaphore");
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = timer_cb,
        .name     = "led_pwm",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, PWM_TICK_US));

    xTaskCreate(led_task, "led_ctrl", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "LED controller ready "
             "(SW-PWM %d steps @ %.1f Hz, R:%d G:%d B:%d duty-max/16)",
             PWM_STEPS, 1000.0f / PWM_STEPS,
             DUTY_R_MAX, DUTY_G_MAX, DUTY_B_MAX);
}

void led_ctrl_set_mode(led_mode_t mode)
{
    if (!s_led_ok) return;
    s_mode = (int)mode;
}

void led_ctrl_force_off(void)
{
    if (!s_led_ok) return;
    s_mode = (int)LED_MODE_OFF;
    /* Immediate hardware write in caller's context */
    npm1300_led_set_rgb(false, false, false);
    s_last_r = s_last_g = s_last_b = false;
}
