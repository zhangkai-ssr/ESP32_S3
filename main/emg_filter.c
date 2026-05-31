/*
 * EMG Digital Filter — 20 Hz HPF + 50/100/150 Hz Notch
 *
 * HPF:   4th-order Butterworth (2 biquad sections), fc = 20 Hz
 * Notch: 3 × IIR notch at 50, 100, 150 Hz, Q = 12.5 (BW ≈ 4 Hz each)
 * Form:  Direct-Form II Transposed (best numerical stability)
 * Math:  Audio EQ Cookbook (R. Bristow-Johnson) for notch;
 *        bilinear-transform Butterworth for HP.
 *
 * All design math uses double; runtime processing is float only
 * (ESP32-S3 single-precision FPU).
 */

#include "emg_filter.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "emg_filter";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- design parameters (compile-time constants) ---- */
#define FS          2000.0   /* sample rate  (Hz) — ADS1298 CONFIG1=0x84 (2000 SPS) */
#define FC_HP         20.0   /* HP cutoff    (Hz) — standard EMG band lower edge     */
#define F_NOTCH       50.0   /* notch fundamental (Hz)                                */
#define NOTCH_Q       12.5   /* notch Q (BW ≈ 4 Hz at 50 Hz)                         */
#define NOTCH_HARM     3     /* harmonics: 50, 100, 150 Hz                            */

/* ================================================================
 * 4th-order Butterworth high-pass  (2 cascaded biquads)
 *
 *   H(z) per section, bilinear transform with pre-warp:
 *     C  = cot(π·fc/fs)
 *     D  = C² + αk·C + 1          (αk = Butterworth coeff)
 *     b  = [C²/D, -2C²/D, C²/D]
 *     a  = [1, 2(1-C²)/D, (C²-αk·C+1)/D]
 * ================================================================ */
static void design_hp4(emg_bq_coeff_t *c)
{
	/* Butterworth 4th-order pole-pair coefficients:
	 *   αk = 2·sin(π·(2k-1)/(2N)),  k=1,2  N=4
	 *   α1 = 2·sin(π/8)  = 0.76536686…
	 *   α2 = 2·sin(3π/8) = 1.84775907…
	 */
	static const double ak[2] = {0.76536686473, 1.84775906502};

	double C  = 1.0 / tan(M_PI * FC_HP / FS);
	double C2 = C * C;

	for (int i = 0; i < 2; i++) {
		double D = C2 + ak[i] * C + 1.0;
		c[i].b0 = (float)(C2 / D);
		c[i].b1 = (float)(-2.0 * C2 / D);
		c[i].b2 = (float)(C2 / D);
		c[i].a1 = (float)(2.0 * (1.0 - C2) / D);
		c[i].a2 = (float)((C2 - ak[i] * C + 1.0) / D);
	}
}

/* ================================================================
 * IIR notch filter  (Audio EQ Cookbook)
 *
 *   w0    = 2π·f/fs
 *   alpha = sin(w0)/(2·Q)
 *   b = [1, -2cos(w0), 1]  / (1+alpha)
 *   a = [1, -2cos(w0), 1-alpha] / (1+alpha)
 *
 *   Note: b1 == a1 (zeros on unit circle ⇒ infinite null depth).
 * ================================================================ */
static void design_notch(emg_bq_coeff_t *c, double freq)
{
	double w0    = 2.0 * M_PI * freq / FS;
	double alpha = sin(w0) / (2.0 * NOTCH_Q);
	double a0inv = 1.0 / (1.0 + alpha);
	double cw    = cos(w0);

	c->b0 = (float)(a0inv);
	c->b1 = (float)(-2.0 * cw * a0inv);
	c->b2 = (float)(a0inv);
	c->a1 = (float)(-2.0 * cw * a0inv);   /* == b1 */
	c->a2 = (float)((1.0 - alpha) * a0inv);
}

/* ================================================================
 * Public API
 * ================================================================ */

void emg_filter_bank_init(emg_filter_bank_t *fb)
{
	memset(fb, 0, sizeof(*fb));

	int idx = 0;

	/* HP: 2 cascaded biquads (4th-order Butterworth, 20 Hz) */
	design_hp4(&fb->c[0]);
	idx = 2;

	/* Notch: 50, 100, 150 Hz */
	for (int h = 1; h <= NOTCH_HARM; h++) {
		double f = F_NOTCH * (double)h;
		if (f >= FS / 2.0) {
			break;
		}
		design_notch(&fb->c[idx++], f);
	}

	fb->n = idx;

	ESP_LOGI(TAG, "filter bank: %d biquads (HPF 20Hz + notch 50/100/150Hz @ %.0f SPS)",
		 fb->n, (double)FS);
}

void emg_filter_bank_reset(emg_filter_bank_t *fb, int ch)
{
	if (ch >= 0 && ch < EMG_FILTER_NUM_CH) {
		memset(fb->s[ch], 0, sizeof(fb->s[ch]));
	}
}

float emg_filter_bank_process(emg_filter_bank_t *fb, int ch, float x)
{
	emg_bq_state_t *st = fb->s[ch];

	for (int i = 0; i < fb->n; i++) {
		const emg_bq_coeff_t *c = &fb->c[i];
		emg_bq_state_t       *s = &st[i];

		/* Direct-Form II Transposed */
		float y  = c->b0 * x + s->d1;
		s->d1    = c->b1 * x - c->a1 * y + s->d2;
		s->d2    = c->b2 * x - c->a2 * y;
		x = y;
	}
	return x;
}
