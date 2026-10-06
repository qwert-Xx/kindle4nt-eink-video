/* SPDX-License-Identifier: GPL-2.0-only */
#define main k4main
#include "../player/k4-wb.c"
#undef main
/* 参考模型：逐像素标量一步，比较每帧写入 WB 的目标色和位置 */
static uint8_t rlv[NPIX], rtl[NPIX], rout[NPIX];
static uint16_t fakewb[NPIX];
int main(void)
{
	extra = 2; nps = 9; top = nps + 2 * extra;
	wb = fakewb; stride = W; wbinfo = (struct k4_epdc_wb_info){ .hold=0x5783, .black=0x5003, .white=0x50f3 };
	memset(st, top | S_TW, sizeof(st));
	memset(target_bits, 0xff, sizeof(target_bits));
	for (int i = 0; i < NPIX; i++) { rlv[i] = top; rtl[i] = top; fakewb[i] = wbinfo.hold; rout[i] = C_HOLD; }
	long bad = 0, badlv = 0;
	unsigned long ls = 0; int done;
	srand(1);
	for (int f = 0; f < 400; f++) {
		/* WAIT_FRAME 差值中的额外扫描：原 WB 动作重复，逐像素饱和。 */
		if (f && f % 17 == 0) {
			unsigned int missed = f % 37 + 1;
			catch_up(missed);
			for (int i = 0; i < NPIX; i++) {
				int p = rlv[i];
				if (rout[i] == C_WHITE) p += missed;
				if (rout[i] == C_BLACK) p -= missed;
				rlv[i] = p < 0 ? 0 : p > top ? top : p;
			}
		}
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
			for (int i = 0; i < NPIX; i++) fakewb[i] = wbinfo.hold;
			resync();
		}
		flush_list();
		for (int i = 0; i < NPIX; i++) {
			uint8_t p = rlv[i];
			uint8_t up = -(uint8_t)(p < rtl[i]), down = -(uint8_t)(p > rtl[i]);
			rlv[i] = p + (up & 1) - (down & 1);
			rout[i] = (up & C_WHITE) | (~(up | down) & C_HOLD);
			uint16_t expected = rout[i] == C_WHITE ? wbinfo.white : rout[i] == C_BLACK ? wbinfo.black : wbinfo.hold;
			bad += fakewb[i] != expected;
			badlv += (st[i] & S_LV) != rlv[i];
		}
	}
	printf("frames 400, action mismatches %ld, level mismatches %ld, writes %lu\n", bad, badlv, writes);
	return bad || badlv;
}
