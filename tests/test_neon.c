/* SPDX-License-Identifier: GPL-2.0-only */
/* 播放器 NEON 步进与标量步进对拍：只朝目标端推，越过目标端的过冲保持。 */
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define C_WHITE 0xf0
#define C_BLACK 0x00
#define C_HOLD 0x80
#define S_LV 0x1f
static void scalar(uint8_t *l, const uint8_t *tw, uint8_t *out, uint8_t black, uint8_t white)
{
	for (int x = 0; x < 16; x++) {
		uint8_t p = l[x];
		uint8_t up = -(uint8_t)(tw[x] && p < white), down = -(uint8_t)(!tw[x] && p > black);
		l[x] = p + (up & 1) - (down & 1);
		out[x] = (up & C_WHITE) | (down & C_BLACK) | (~(up | down) & C_HOLD);
	}
}
static void neon(uint8_t *l, const uint8_t *twv, uint8_t *o, uint8_t black, uint8_t white)
{
	const uint8x16_t vwhite = vdupq_n_u8(white), vblack = vdupq_n_u8(black);
	const uint8x16_t cw = vdupq_n_u8(C_WHITE), hold = vdupq_n_u8(C_HOLD);
	uint8x16_t p = vld1q_u8(l), tm = vtstq_u8(vld1q_u8(twv), vld1q_u8(twv));
	uint8x16_t up = vandq_u8(vcltq_u8(p, vwhite), tm);
	uint8x16_t down = vbicq_u8(vcgtq_u8(p, vblack), tm);
	vst1q_u8(l, vaddq_u8(vsubq_u8(p, up), down));
	vst1q_u8(o, vorrq_u8(vandq_u8(up, cw), vbicq_u8(hold, vorrq_u8(up, down))));
}
int main(void)
{
	long bad = 0, n = 0;
	for (int extra = 0; extra <= 4; extra++) {
		int top = 9 + 2 * extra, margin = (S_LV - top) / 2;
		for (int r = 0; r < 20000; r++) {
			uint8_t l1[16], l2[16], tw[16], o1[16], o2[16];
			for (int i = 0; i < 16; i++) {
				l1[i] = l2[i] = rand() % (S_LV + 1);
				tw[i] = rand() & 1;
			}
			scalar(l1, tw, o1, margin, margin + top);
			neon(l2, tw, o2, margin, margin + top);
			for (int i = 0; i < 16; i++, n++)
				bad += l1[i] != l2[i] || o1[i] != o2[i];
		}
	}
	printf("compared %ld pixels, mismatches %ld\n", n, bad);
	return bad != 0;
}
