/* SPDX-License-Identifier: GPL-2.0-only */
/* 墨水响应曲线测量：经 /dev/k4-epdc-wb 逐帧控制 22 条带（沿面板 x 方向）的推动次数。
 * 每次运行先 GC16 刷白，按模式执行推动序列，然后全部不动、打印 SHOOT 并停留 HOLD 秒供拍照。
 *   wbcurve bw   白起，第 i 条推黑 k_i 次
 *   wbcurve wb   白起，全部推黑 SAT 次到饱和黑，第 i 条推白 k_i 次
 *   wbcurve rtw  白起，第 i 条推黑 k_i 次再推白 k_i 次（往返）
 *   wbcurve rtb  白起，全部推黑 SAT 次，第 i 条推白 k_i 次再推黑 k_i 次
 * 条带 0 不推动，作参照（bw/rtw 为白，wb/rtb 为饱和黑）。 */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/mxcfb.h>
#include <linux/k4-epdc-wb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define NB 23
#define SAT 40
static const int K[NB] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 26, 30, 35, 40 };

enum { HOLD, BLACK, WHITE };

/* comp 模式：条带 0 不推（GC16 白）、1 推黑 40 再推白 21（播放器的“停在白”）、2 推黑 40；
 * 3..22 为 (k, d)：同样先到“停在白”，再推黑 k 次、推白 k+d 次。 */
static const int CK[NB] = { 0, 0, 0, 1, 1, 1, 1, 7, 7, 7, 7, 8, 8, 8, 8, 10, 10, 10, 10, 12, 12, 12, 12 };
static const int CD[NB] = { 0, 0, 0, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3 };
#define REST 21

static int comp_action(int b, int f)
{
	if (b == 0)
		return HOLD;
	if (f < SAT)
		return BLACK;
	if (b == 2)
		return HOLD;
	f -= SAT;
	if (f < REST)
		return WHITE;
	f -= REST;
	int k = CK[b], d = CD[b];
	return f < k ? BLACK : f < 2 * k + d ? WHITE : HOLD;
}

/* 第 b 条在第 f 帧（从 0 计）的动作 */
static int action(const char *mode, int b, int f)
{
	int k = K[b];

	if (!strcmp(mode, "comp"))
		return comp_action(b, f);
	if (!strcmp(mode, "bw"))
		return f < k ? BLACK : HOLD;
	if (!strcmp(mode, "refw"))
		return HOLD;
	if (!strcmp(mode, "refb"))
		return f < SAT ? BLACK : HOLD;
	if (!strcmp(mode, "rtw"))
		return f < k ? BLACK : f < 2 * k ? WHITE : HOLD;
	if (f < SAT)
		return BLACK;
	f -= SAT;
	if (!strcmp(mode, "wb"))
		return f < k ? WHITE : HOLD;
	return f < k ? WHITE : f < 2 * k ? BLACK : HOLD; /* rtb */
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return fprintf(stderr, "usage: wbcurve bw|wb|rtw|rtb\n"), 2;
	const char *mode = argv[1];
	int fb = open("/dev/fb0", O_RDWR);
	struct fb_var_screeninfo var, old;
	struct fb_fix_screeninfo fix;
	if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &old))
		return perror("fb"), 1;
	var = old;
	var.bits_per_pixel = 8;
	var.grayscale = 1;
	unsigned int am = AUTO_UPDATE_MODE_REGION_MODE;
	ioctl(fb, MXCFB_SET_AUTO_UPDATE_MODE, &am);
	if (ioctl(fb, FBIOPUT_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix))
		return perror("Y8"), 1;
	uint8_t *c = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	memset(c, 0xff, fix.line_length * var.yres);
	struct mxcfb_update_data u = { .update_region = { 0, 0, var.xres, var.yres }, .waveform_mode = 2,
		.update_mode = UPDATE_MODE_FULL, .update_marker = 7, .temp = TEMP_USE_AMBIENT };
	struct mxcfb_update_marker_data m = { .update_marker = 7 };
	ioctl(fb, MXCFB_SEND_UPDATE, &u);
	ioctl(fb, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m);

	int wbfd = open("/dev/k4-epdc-wb", O_RDWR);
	struct k4_epdc_wb_info info;
	if (wbfd < 0 || ioctl(wbfd, K4_EPDC_WB_START, &info))
		return perror("WB START"), 1;
	volatile uint16_t *wb = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, wbfd, 0);
	if (wb == MAP_FAILED)
		return perror("mmap"), 1;
	const uint16_t val[3] = { info.hold, info.black, info.white };
	int cur[NB], last = 0;
	for (int b = 0; b < NB; b++)
		cur[b] = HOLD;
	for (int b = 0; b < NB; b++) {
		int need = (strcmp(mode, "bw") && strcmp(mode, "rtw") ? SAT : 0) + 2 * K[b];
		if (!strcmp(mode, "refw") || !strcmp(mode, "refb"))
			need = SAT;
		if (!strcmp(mode, "comp"))
			need = SAT + REST + 2 * CK[b] + CD[b];
		if (need > last)
			last = need;
	}
	struct k4_epdc_wb_frame fr = { 0 };
	ioctl(wbfd, K4_EPDC_WB_WAIT_FRAME, &fr);
	uint64_t f0 = fr.frame;
	for (int f = 0; f <= last + 2; ) {
		/* 帧边界刚过：写入第 f 帧的动作 */
		for (int b = 0; b < NB; b++) {
			int a = action(mode, b, f);
			if (a == cur[b])
				continue;
			cur[b] = a;
			unsigned int x0 = b * info.width / NB + 4, x1 = (b + 1) * info.width / NB - 4;
			for (unsigned int y = 0; y < info.height; y++)
				for (unsigned int x = x0; x < x1; x++)
					wb[(size_t)y * info.stride + x] = val[a];
		}
		if (ioctl(wbfd, K4_EPDC_WB_WAIT_FRAME, &fr))
			return perror("WAIT_FRAME"), 1;
		int nf = (int)(fr.frame - f0);
		if (nf > f + 1)
			fprintf(stderr, "lost %d frames at %d\n", nf - f - 1, f);
		f = nf;
	}
	fprintf(stderr, "SHOOT %s\n", mode);
	const char *h = getenv("HOLD");
	sleep(h ? atoi(h) : 6);
	ioctl(wbfd, K4_EPDC_WB_STOP);
	ioctl(fb, FBIOPUT_VSCREENINFO, &old);
	am = AUTO_UPDATE_MODE_AUTOMATIC_MODE;
	ioctl(fb, MXCFB_SET_AUTO_UPDATE_MODE, &am);
	return 0;
}
