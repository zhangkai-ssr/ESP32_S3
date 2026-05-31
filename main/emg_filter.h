#ifndef EMG_FILTER_H_
#define EMG_FILTER_H_

#include <stdint.h>

#define EMG_FILTER_NUM_CH   16  /* 2 × ADS1298 × 8 channels */
#define EMG_FILTER_NUM_BQ    5  /* 2 HP (4th-order, 20 Hz) + 3 notch (50,100,150 Hz) */

typedef struct {
	float b0, b1, b2, a1, a2;
} emg_bq_coeff_t;

typedef struct {
	float d1, d2;
} emg_bq_state_t;

typedef struct {
	emg_bq_coeff_t c[EMG_FILTER_NUM_BQ];                    /* shared coefficients */
	emg_bq_state_t s[EMG_FILTER_NUM_CH][EMG_FILTER_NUM_BQ]; /* per-channel state   */
	int n;                                                    /* active biquad count */
} emg_filter_bank_t;

void  emg_filter_bank_init(emg_filter_bank_t *fb);
void  emg_filter_bank_reset(emg_filter_bank_t *fb, int ch);
float emg_filter_bank_process(emg_filter_bank_t *fb, int ch, float x);

#endif /* EMG_FILTER_H_ */
