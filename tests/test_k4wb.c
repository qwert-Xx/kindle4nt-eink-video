/* SPDX-License-Identifier: GPL-2.0-only */
#define main k4main
#include "../player/k4-wb.c"
#undef main
/* 参考模型：逐像素标量一步，比较每帧写入 WB 的目标色和位置；另累计每像素实际推动
 * （含漏帧重复执行的推动），检查净推动始终等于位置变化（直流不累积）。 */
static uint8_t rlv[NPIX], rtl[NPIX], rout[NPIX];
static int rnet[NPIX], rb[NPIX];
static uint16_t fakewb[NPIX];
int main(void)
{
	extra = 2; nps = 9; top = nps + 2 * extra; margin = (S_LV - top) / 2;
	bias_cap = 3; /* 小上限使跳过频繁触顶 */
	const int white = margin + top, black = margin, lo = margin + extra, hi = margin + top - extra;
	wb = fakewb; stride = W; wbinfo = (struct k4_epdc_wb_info){ .hold=0x5783, .black=0x5003, .white=0x50f3 };
	memset(st, white | S_TW, sizeof(st));
	memset(target_bits, 0xff, sizeof(target_bits));
	for (int i = 0; i < NPIX; i++) { rlv[i] = white; rtl[i] = white; fakewb[i] = wbinfo.hold; rout[i] = C_HOLD; }
	long bad = 0, badlv = 0, dc_bad = 0;
	unsigned long ls = 0; int done;
	srand(1);
	for (int f = 0; f < 400; f++) {
		/* WAIT_FRAME 差值中的额外扫描：原 WB 动作重复执行，位置越过端点进入余量。 */
		if (f && f % 17 == 0) {
			unsigned int missed = f % (unsigned int)margin + 1;
			catch_up(missed);
			for (int i = 0; i < NPIX; i++) {
				int p = rlv[i];
				if (rout[i] == C_WHITE) p += missed, rnet[i] += missed;
				if (rout[i] == C_BLACK) p -= missed, rnet[i] -= missed;
				rlv[i] = p < 0 ? 0 : p > S_LV ? S_LV : p;
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
				rtl[i] = (pending[i / 8] & (0x80 >> (i % 8))) ? white : black;
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
			/* 跳过补推区，跳过量受 bias 上限约束 */
			if (rtl[i] == white) {
				int d = p < lo ? lo - p : 0, room = bias_cap + rb[i], j = d < room ? d : room;
				p += j, rb[i] -= j;
			} else {
				int d = p > hi ? p - hi : 0, room = bias_cap - rb[i], j = d < room ? d : room;
				p -= j, rb[i] += j;
			}
			/* 只朝目标端推，越过目标端的过冲保持 */
			uint8_t up = -(uint8_t)(rtl[i] == white && p < white);
			uint8_t down = -(uint8_t)(rtl[i] == black && p > black);
			/* 还账：停在目标端、账户可还时每帧推一次，不改位置 */
			uint8_t rbk = -(uint8_t)(rtl[i] == black && p <= black && rb[i] > 0);
			uint8_t rwh = -(uint8_t)(rtl[i] == white && p >= white && rb[i] < 0);
			rlv[i] = p + (up & 1) - (down & 1);
			rb[i] += (rwh & 1) - (rbk & 1);
			up |= rwh, down |= rbk;
			rnet[i] += (up & 1) - (down & 1);
			dc_bad += rnet[i] != rlv[i] - white + rb[i] || rb[i] != bias[i] ||
				  rb[i] > bias_cap || rb[i] < -bias_cap;
			rout[i] = (up & C_WHITE) | (~(up | down) & C_HOLD);
			uint16_t expected = rout[i] == C_WHITE ? wbinfo.white : rout[i] == C_BLACK ? wbinfo.black : wbinfo.hold;
			bad += fakewb[i] != expected;
			badlv += (st[i] & S_LV) != rlv[i];
		}
	}
	printf("frames 400, action mismatches %ld, level mismatches %ld, net-push vs level mismatches %ld, writes %lu\n",
	       bad, badlv, dc_bad, writes);
	return bad || badlv || dc_bad;
}
