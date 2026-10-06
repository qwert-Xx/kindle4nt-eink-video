/* SPDX-License-Identifier: GPL-2.0-only */
#define main k4main
#include "../player/k4-wb.c"
#undef main
/* 参考模型：逐像素标量一步，比较每帧写入 WB 的目标色和位置 */
static uint8_t rlv[NPIX], rtl[NPIX], rout[NPIX];
static uint16_t fakewb[NPIX];
int main(void)
{
	extra = 2; nps = 9; top = nps + 2 * extra; lo = extra; hi = top - extra;
	wb = fakewb; stride = W; wbbase = 0x0008;
	memset(st, top | S_TW, sizeof(st));
	memset(target_bits, 0xff, sizeof(target_bits));
	for (int i = 0; i < NPIX; i++) { rlv[i] = top; rtl[i] = top; fakewb[i] = 0x88; }
	long bad = 0, badlv = 0;
	unsigned long ls = 0; int done;
	srand(1);
	for (int f = 0; f < 400; f++) {
		if (f % 3 == 0) {
			/* 新视频帧：随机矩形翻转，偶尔整屏翻转 */
			if (f % 60 == 0)
				for (int i = 0; i < FRAME; i++) pending[i] = ~pending[i];
			int x0 = rand() % 700, y0 = rand() % 500, w = 8 + rand() % 100, h = 1 + rand() % 100;
			for (int y = y0; y < y0 + h; y++)
				for (int x = x0; x < x0 + w; x++)
					pending[(y * W + x) / 8] ^= 0x80 >> (x % 8);
			seq_in++;
			for (int i = 0; i < NPIX; i++)
				rtl[i] = (pending[i / 8] & (0x80 >> (i % 8))) ? top : 0;
		}
		take_target(&ls, &done);
		compute();
		if (f % 50 == 49) {
			/* 模拟 LUT 重启：处理把 WB 全写成不动 */
			for (int i = 0; i < NPIX; i++) fakewb[i] = 0x88;
			resync();
		}
		flush_list();
		for (int i = 0; i < NPIX; i++) {
			uint8_t m = -(uint8_t)(rtl[i] != 0), p = rlv[i];
			uint8_t pw = p < lo ? lo : p, pb = p > hi ? hi : p;
			p = (pw & m) | (pb & ~m);
			uint8_t up = -(uint8_t)(p < rtl[i]), down = -(uint8_t)(p > rtl[i]);
			rlv[i] = p + (up & 1) - (down & 1);
			rout[i] = (up & C_WHITE) | (~(up | down) & C_HOLD);
			bad += (fakewb[i] & 0xf0) != rout[i];
			badlv += (st[i] & S_LV) != rlv[i];
		}
	}
	printf("frames 400, action mismatches %ld, level mismatches %ld, writes %lu\n", bad, badlv, writes);
	return bad || badlv;
}
