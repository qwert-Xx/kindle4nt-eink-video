/* SPDX-License-Identifier: GPL-2.0-only */
/* 只读测量 EPDC 时序：提交一次更新后轮询 EPDC_STATUS/STATUS_LUTS/DEBUG_LUTn。
 * 测试期间禁用自动关电，保证读寄存器时 EPDC 有时钟；结束恢复原值。
 * 用法：epdc-timing <x> <y> <w> <h> <waveform> */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/mxcfb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define EPDC_BASE 0x41010000
#define STATUS_LUTS 0x440
#define STATUS 0x4a0
#define DEBUG_LUT(i) (0x540 + 16 * (i))

static double now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static void wait_marker(int fd, unsigned int marker)
{
	struct mxcfb_update_marker_data m = { .update_marker = marker };
	ioctl(fd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m);
}

int main(int argc, char **argv)
{
	if (argc != 6)
		return fprintf(stderr, "usage: %s x y w h waveform\n", argv[0]), 2;
	unsigned int x = atoi(argv[1]), y = atoi(argv[2]), w = atoi(argv[3]), h = atoi(argv[4]);
	unsigned int wf = atoi(argv[5]), mode = AUTO_UPDATE_MODE_REGION_MODE;
	int fd = open("/dev/fb0", O_RDWR), mem = open("/dev/mem", O_RDONLY | O_SYNC);
	struct fb_fix_screeninfo fix;
	int32_t old_delay, no_delay = -1;

	if (fd < 0 || mem < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix))
		return perror("open"), 1;
	uint32_t *fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	volatile uint32_t *r = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, mem, EPDC_BASE);
	if (fb == MAP_FAILED || r == MAP_FAILED)
		return perror("mmap"), 1;
	ioctl(fd, MXCFB_GET_PWRDOWN_DELAY, &old_delay);
	ioctl(fd, MXCFB_SET_PWRDOWN_DELAY, &no_delay);
	ioctl(fd, MXCFB_SET_AUTO_UPDATE_MODE, &mode);

	/* 预热：一次小更新完成后 EPDC 保持通电（自动关电已禁用）。 */
	struct mxcfb_update_data u = {
		.update_region = { .top = 0, .left = 0, .width = 8, .height = 8 },
		.waveform_mode = 1, .update_mode = UPDATE_MODE_PARTIAL,
		.update_marker = 1, .temp = TEMP_USE_AMBIENT,
	};
	ioctl(fd, MXCFB_SEND_UPDATE, &u);
	wait_marker(fd, 1);

	/* 区域内容整体反相，保证所有像素都变化。 */
	uint32_t v = (fb[y * (fix.line_length / 4) + x] & 0xff) > 0x80 ? 0 : 0xffffff;
	for (unsigned int j = y; j < y + h; j++)
		for (unsigned int i = x; i < x + w; i++)
			fb[j * (fix.line_length / 4) + i] = v;

	u.update_region = (struct mxcfb_rect){ .top = y, .left = x, .width = w, .height = h };
	u.waveform_mode = wf;
	u.update_marker = 2;
	double t0 = now_us();
	if (ioctl(fd, MXCFB_SEND_UPDATE, &u))
		perror("SEND_UPDATE");
	double t_ret = now_us() - t0;

	uint32_t prev_st = r[STATUS / 4] & 1, prev_luts = r[STATUS_LUTS / 4] & 0xffff;
	double wb_on = -1, wb_off = -1, lut_on = -1, lut_off = -1, ft[1024];
	int lut = -1, nft = 0, prev_frame = -1, max_frame = 0;
	for (double t = 0; t < 5e6; t = now_us() - t0) {
		uint32_t st = r[STATUS / 4] & 1, luts = r[STATUS_LUTS / 4] & 0xffff;
		if (st && !prev_st && wb_on < 0) wb_on = t;
		if (!st && prev_st && wb_on >= 0 && wb_off < 0) wb_off = t;
		if (luts & ~prev_luts && lut < 0) {
			lut = __builtin_ctz(luts & ~prev_luts);
			lut_on = t;
		}
		if (lut >= 0) {
			int frame = (r[DEBUG_LUT(lut) / 4] >> 5) & 0x3ff;
			if (frame != prev_frame) {
				if (frame > max_frame) max_frame = frame;
				if (nft < 1024) ft[nft++] = t;
				prev_frame = frame;
			}
			if (!(luts & (1u << lut))) {
				lut_off = t;
				break;
			}
		}
		prev_st = st;
		prev_luts = luts;
	}
	wait_marker(fd, 2);
	double t_done = now_us() - t0;

	printf("region %ux%u wf=%u: ioctl %.0fus, WB busy %.0f..%.0fus (%.0fus), LUT%d %.0f..%.0fus (%.1fms), "
	       "frames(max remaining)=%d, frame changes=%d, mean frame period %.2fms, marker done %.1fms\n",
	       w, h, wf, t_ret, wb_on, wb_off, wb_on >= 0 && wb_off >= 0 ? wb_off - wb_on : -1,
	       lut, lut_on, lut_off, (lut_off - lut_on) / 1e3, max_frame, nft,
	       nft > 2 ? (ft[nft - 1] - ft[1]) / (nft - 2) / 1e3 : -1, t_done / 1e3);

	mode = AUTO_UPDATE_MODE_AUTOMATIC_MODE;
	ioctl(fd, MXCFB_SET_AUTO_UPDATE_MODE, &mode);
	ioctl(fd, MXCFB_SET_PWRDOWN_DELAY, &old_delay);
	return 0;
}
