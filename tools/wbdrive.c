/* SPDX-License-Identifier: GPL-2.0-only */
/* 实验：长 LUT + 每帧改写 WB 目标色，实现逐像素逐帧驱动。
 * 只改内存（波形表与 WB），重启即恢复。
 * wbdrive <frames> <ramp|rev>
 *   改写模式 3 的所有温度档为同一张 <frames> 帧的“只看目标色”表：
 *   目标 0/1 推黑，14/15 推白，其他不动。区域内容为 0x88（保持），FULL 提交模式 3，
 *   然后按 LUT 帧计数逐帧改写 WB 目标色：
 *   ramp: 8 条带，第 k 条推黑 2k+2 帧后保持（从白起，得到 8 级灰）
 *   rev:  全部推黑 12 帧，然后第 k 条推白 2k+2 帧（黑 -> 中途掉头）
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/mxcfb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define EPDC_BASE 0x41010000
#define RX 100
#define RY 200
#define RW 600
#define RH 200
#define BANDS 8
#define HOLD 8

static volatile uint32_t *r;
static uint8_t *wv;
static volatile uint16_t *wb;
static unsigned int stride;
static int fbfd;
static int32_t old_delay;

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static uint64_t le64(size_t off)
{
	uint64_t v;
	memcpy(&v, wv + off, 8);
	return v;
}

static void set64(size_t off, uint64_t v)
{
	memcpy(wv + off, &v, 8);
}

static size_t tt2;
static uint64_t saved2[64];
static unsigned int ntemps;

static void restore(void)
{
	for (unsigned int t = 0; t < ntemps; t++)
		set64(tt2 + t * 8, saved2[t]);
	ioctl(fbfd, MXCFB_SET_PWRDOWN_DELAY, &old_delay);
}

/* 第 k 条带在第 f 帧（从 0 计）的目标色 */
static int kind;
static unsigned int target(int rev, unsigned int k, unsigned int f)
{
	if (kind == 2)
		return HOLD;
	if (kind == 3)
		return 0;
	if (kind == 4)
		return k & 1 ? HOLD : 0;
	if (!rev)
		return f < 2 * k + 2 ? 0 : HOLD;
	if (f < 12)
		return 0;
	return f < 12 + 2 * k + 2 ? 15 : HOLD;
}

int main(int argc, char **argv)
{
	if (argc != 3)
		return fprintf(stderr, "usage: wbdrive <frames> <ramp|rev>\n"), 2;
	unsigned int frames = atoi(argv[1]);
	int rev = !strcmp(argv[2], "rev");
	kind = !strcmp(argv[2], "hold") ? 2 : !strcmp(argv[2], "black") ? 3 : !strcmp(argv[2], "alt") ? 4 : 0;
	int mem = open("/dev/mem", O_RDWR | O_SYNC);
	fbfd = open("/dev/fb0", O_RDWR);
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	if (mem < 0 || fbfd < 0 || ioctl(fbfd, FBIOGET_FSCREENINFO, &fix) ||
	    ioctl(fbfd, FBIOGET_VSCREENINFO, &var) || ioctl(fbfd, MXCFB_GET_PWRDOWN_DELAY, &old_delay))
		return perror("open"), 1;
	int32_t no = -1;
	unsigned int amode = AUTO_UPDATE_MODE_REGION_MODE;
	ioctl(fbfd, MXCFB_SET_PWRDOWN_DELAY, &no);
	atexit(restore);
	ioctl(fbfd, MXCFB_SET_AUTO_UPDATE_MODE, &amode);
	uint8_t *fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);

	/* 唤醒 EPDC 后再读寄存器 */
	struct mxcfb_update_data u = {
		.update_region = { .top = 0, .left = 0, .width = 8, .height = 8 },
		.waveform_mode = 1, .update_mode = UPDATE_MODE_PARTIAL,
		.update_marker = 1, .temp = TEMP_USE_AMBIENT,
	};
	struct mxcfb_update_marker_data m = { .update_marker = 1 };
	ioctl(fbfd, MXCFB_SEND_UPDATE, &u);
	ioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m);

	r = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, mem, EPDC_BASE);
	unsigned int res_w = r[0x40 / 4] & 0x1fff, res_h = (r[0x40 / 4] >> 16) & 0x1fff;
	stride = (res_w + 3) & ~3u;
	wv = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, mem, r[0x20 / 4]);
	wb = mmap(NULL, ((size_t)stride * res_h * 2 + 4095) & ~4095ul, PROT_READ | PROT_WRITE,
		  MAP_SHARED, mem, r[0x30 / 4]);
	if (wv == MAP_FAILED || wb == MAP_FAILED)
		return perror("mmap"), 1;

	/* 从 A2（模式 4）当前温度档取推黑/推白的动作码 */
	unsigned int temp = r[0x1a0 / 4], temps = (le64(le64(0)) - le64(0)) / 8;
	size_t a2 = le64(le64(4 * 8) + temp * 8);
	uint8_t black = 0, white = 0;
	for (uint64_t f = 0; f < le64(a2); f++) {
		const uint8_t *act = wv + a2 + 8 + f * 256;
		if (!black)
			black = act[0x0f];
		if (!white)
			white = act[0xf0];
	}
	/* 模式 3 的数据区（温度档 0 的条目起点到模式 4 的温度表）给合成表用 */
	size_t tt3 = le64(3 * 8), base = le64(tt3), end = le64(4 * 8);
	if (base + 8 + (size_t)frames * 256 > end)
		return fprintf(stderr, "frames too many: room %zu\n", (end - base - 8) / 256), 1;
	uint8_t act[256];
	for (unsigned int i = 0; i < 256; i++) {
		unsigned int np = i >> 4;
		act[i] = getenv("ZERO") ? 0 : np <= 1 ? black : np >= 14 ? white : 0;
	}
	set64(base, frames);
	for (unsigned int f = 0; f < frames; f++)
		memcpy(wv + base + 8 + (size_t)f * 256, act, 256);
	/* 驱动把用户波形 2（GC16）映射到硬件模式 2：临时把它的温度表指向合成表，退出时还原 */
	tt2 = le64(2 * 8);
	for (unsigned int t = 0; t < temps; t++) {
		saved2[t] = le64(tt2 + t * 8);
		set64(tt2 + t * 8, base);
	}
	ntemps = temps;
	printf("temp=%u black=%u white=%u synthesized %u frames at +%zx\n", temp, black, white, frames, base);

	/* 区域画布写 0x88，FULL 提交模式 3 */
	unsigned int bpp = var.bits_per_pixel / 8;
	for (unsigned int j = 0; j < RH; j++)
		memset(fb + (size_t)(RY + j) * fix.line_length + RX * bpp, HOLD * 17, RW * bpp);
	u.update_region = (struct mxcfb_rect){ .top = RY, .left = RX, .width = RW, .height = RH };
	u.waveform_mode = 2;
	u.update_mode = UPDATE_MODE_FULL;
	u.update_marker = 2;
	uint32_t before = r[0x440 / 4] & 0xffff;
	double t0 = now_ms();
	if (ioctl(fbfd, MXCFB_SEND_UPDATE, &u))
		return perror("SEND_UPDATE"), 1;
	int lut = -1;
	while (now_ms() - t0 < 2000) {
		uint32_t l = r[0x440 / 4] & 0xffff & ~before;
		if (l) {
			lut = __builtin_ctz(l);
			break;
		}
	}
	if (lut < 0)
		return fprintf(stderr, "LUT never started\n"), 1;
	/* 记下每个像素 WB 的高字节与当前色，之后只写不读 */
	static uint16_t keep[RH][RW];
	for (unsigned int j = 0; j < RH; j++)
		for (unsigned int i = 0; i < RW; i++)
			keep[j][i] = wb[(size_t)(RY + j) * stride + RX + i] & ~0xf0;
	printf("LUT%d started at %.1fms, wb sample %04x\n", lut, now_ms() - t0, keep[RH / 2][RW / 2]);

	unsigned int cur[BANDS], f = 0;
	int prev = (r[(0x540 + 16 * lut) / 4] >> 5) & 0x3ff;
	double tf[64];
	uint16_t s0[64], s7[64];
	for (unsigned int k = 0; k < BANDS; k++)
		cur[k] = HOLD;
	tf[0] = now_ms() - t0;
	for (;;) {
		/* 帧计数变化时写入这一帧的目标色；扫描已开始的行按上一帧的值走 */
		for (unsigned int k = 0; k < BANDS; k++) {
			unsigned int t = target(rev, k, f);
			if (t == cur[k])
				continue;
			cur[k] = t;
			unsigned int x0 = k * RW / BANDS, x1 = (k + 1) * RW / BANDS;
			for (unsigned int j = 0; j < RH; j++) {
				volatile uint16_t *p = &wb[(size_t)(RY + j) * stride + RX];
				for (unsigned int i = x0; i < x1; i++)
					p[i] = keep[j][i] | t << 4;
			}
		}
		if (f < 64) {
			s0[f] = wb[(size_t)(RY + RH / 2) * stride + RX + RW / 16];
			s7[f] = wb[(size_t)(RY + RH / 2) * stride + RX + RW - RW / 16];
		}
		int frame;
		while ((frame = (r[(0x540 + 16 * lut) / 4] >> 5) & 0x3ff) == prev &&
		       (r[0x440 / 4] & (1u << lut)))
			;
		if (!(r[0x440 / 4] & (1u << lut)))
			break;
		prev = frame;
		if (++f < 64)
			tf[f] = now_ms() - t0;
	}
	printf("LUT done at %.1fms after %u frame changes; first frames:", now_ms() - t0, f);
	for (unsigned int i = 0; i < 8 && i <= f; i++)
		printf(" %.1f", tf[i]);
	printf("\n");
	for (unsigned int i = 0; i < 64 && i <= f; i++)
		printf("f%u %04x %04x\n", i, s0[i], s7[i]);
	m.update_marker = 2;
	ioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m);
	return 0;
}
