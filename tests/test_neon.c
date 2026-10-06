/* SPDX-License-Identifier: GPL-2.0-only */
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define C_WHITE 0xf0
#define C_BLACK 0x00
#define C_HOLD 0x80
static void scalar(uint8_t *l, const uint8_t *t, uint8_t *out)
{
	for (int x = 0; x < 16; x++) {
		uint8_t p = l[x];
		uint8_t up = -(uint8_t)(p < t[x]), down = -(uint8_t)(p > t[x]);
		l[x] = p + (up & 1) - (down & 1);
		out[x] = (up & C_WHITE) | (down & C_BLACK) | (~(up | down) & C_HOLD);
	}
}
static void neon(uint8_t *l, const uint8_t *tl, uint8_t *o)
{
	const uint8x16_t white = vdupq_n_u8(C_WHITE), hold = vdupq_n_u8(C_HOLD);
	uint8x16_t p = vld1q_u8(l), t = vld1q_u8(tl);
	uint8x16_t up = vcltq_u8(p, t), down = vcgtq_u8(p, t);
	vst1q_u8(l, vaddq_u8(vsubq_u8(p, up), down));
	vst1q_u8(o, vorrq_u8(vandq_u8(up, white), vbicq_u8(hold, vorrq_u8(up, down))));
}
int main(void)
{
	long bad = 0, n = 0;
	for (int extra = 0; extra <= 4; extra++) {
		int top = 9 + 2 * extra;
		for (int r = 0; r < 20000; r++) {
			uint8_t l1[16], l2[16], t[16], o1[16], o2[16];
			for (int i = 0; i < 16; i++) {
				l1[i] = l2[i] = rand() % (top + 1);
				t[i] = rand() & 1 ? top : 0;
			}
			scalar(l1, t, o1);
			neon(l2, t, o2);
			for (int i = 0; i < 16; i++, n++)
				bad += l1[i] != l2[i] || o1[i] != o2[i];
		}
	}
	printf("compared %ld pixels, mismatches %ld\n", n, bad);
	return bad != 0;
}
