/*
 * led_ctrl.h – LED pattern controller
 *
 * Implements all LED modes defined in docs/开关机与状态机逻辑.md §8.
 *
 * Architecture:
 *   esp_timer fires every 1 ms → unblocks led_task via binary semaphore
 *   led_task runs software PWM (16 steps, ~62.5 Hz) and writes RGB on/off
 *   state to the nPM1300 via npm1300_led only when the state changes.
 *
 * Colour-correction max duty (hardware calibration):
 *   R : 8 / 16 = 50 %
 *   G : 2 / 16 = 12.5 %
 *   B : 4 / 16 = 25 %
 *
 * Thread safety:
 *   led_ctrl_set_mode() and led_ctrl_force_off() are safe to call from
 *   any task or ISR; they perform a single atomic store.
 */

#ifndef LED_CTRL_H_
#define LED_CTRL_H_

typedef enum {
    LED_MODE_OFF = 0,

    /* --- button / pairing / shutdown (docs §8.0b) --- */
    LED_MODE_PAIRING_WHITE_PULSE_FAST,   /* 250 ms on / 250 ms off, white    */
    LED_MODE_SHUTDOWN_WHITE_FADE_OUT,    /* white → off linear fade, ~2 s    */
    LED_MODE_FACTORY_RESET,              /* 250 ms orange / 250 ms white alt */

    /* --- charging / battery level (docs §8.0c) --- */
    LED_MODE_POWER_YELLOW_ON,            /* constant yellow – charging ≥20 % */
    LED_MODE_POWER_YELLOW_BLINK,         /* 500 ms on/off  – charging <20 %  */
    LED_MODE_POWER_GREEN_ON,             /* constant green – fully charged    */
    LED_MODE_POWER_RED_ON,               /* constant red   – low battery ≤20%*/
    LED_MODE_POWER_RED_BLINK,            /* 500 ms on/off  – critical  ≤10 % */
} led_mode_t;

/**
 * @brief  Initialise the nPM1300 LED driver, create the software-PWM timer
 *         and the led_task.  Must be called once from app_main before any
 *         led_ctrl_set_mode() call.
 *         If the PMIC is unreachable (I2C error) the function logs an error
 *         and returns without creating the timer/task; all subsequent calls
 *         become no-ops.
 */
void led_ctrl_init(void);

/**
 * @brief  Switch to a new LED mode.  Takes effect on the next PWM tick.
 *         Safe to call from any context.
 */
void led_ctrl_set_mode(led_mode_t mode);

/**
 * @brief  Immediately force all LED channels off and switch to LED_MODE_OFF.
 *         Performs a direct I2C write in the caller's context; use only from
 *         task context (not from an ISR).
 */
void led_ctrl_force_off(void);

#endif /* LED_CTRL_H_ */
