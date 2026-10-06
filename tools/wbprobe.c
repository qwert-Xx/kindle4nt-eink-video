/* SPDX-License-Identifier: GPL-2.0-only */
/* 只读观察 EPDC 工作缓冲（WB）。
 * wbprobe regs                         打印 WVADDR/WB_ADDR/RES/FORMAT
 * wbprobe dump x y n                   打印 (x,y) 起 n 个像素的 WB 16 位值
 * wbprobe fill x y w h gray wf mode    画布写灰度后提交更新，同时每 0.5ms 采样
 *                                      (x+w/2,y+h/2) 的 WB 值，打印变化 */
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
static volatile uint32_t *r;
static volatile uint16_t *wb;
static unsigned int res_w, res_h, stride;

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void map(int mem)
{
	r = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, mem, EPDC_BASE);
	if (r == MAP_FAILED)
		perror("mmap regs"), exit(1);
	uint32_t res = r[0x40 / 4];
	res_w = res & 0x1fff;
	res_h = (res >> 16) & 0x1fff;
	stride = (res_w + 3) & ~3u;
	uint32_t addr = r[0x30 / 4];
	size_t len = (size_t)stride * res_h * 2;
	wb = mmap(NULL, (len + 4095) & ~4095ul, PROT_READ | PROT_WRITE, MAP_SHARED, mem, addr);
	if (wb == MAP_FAILED)
		perror("mmap wb"), exit(1);
}

static int fbfd;
static int32_t old_delay;

static void restore(void)
{
	ioctl(fbfd, MXCFB_SET_PWRDOWN_DELAY, &old_delay);
}

/* 读寄存器前保证 EPDC 有时钟：禁用自动关电并做一次 8x8 PARTIAL（内容不变，不驱动像素）。 */
static void wake(void)
{
	int32_t no = -1;
	unsigned int mode = AUTO_UPDATE_MODE_REGION_MODE;
	fbfd = open("/dev/fb0", O_RDWR);
	if (fbfd < 0 || ioctl(fbfd, MXCFB_GET_PWRDOWN_DELAY, &old_delay))
		perror("fb"), exit(1);
	ioctl(fbfd, MXCFB_SET_PWRDOWN_DELAY, &no);
	atexit(restore);
	ioctl(fbfd, MXCFB_SET_AUTO_UPDATE_MODE, &mode);
	struct mxcfb_update_data u = {
		.update_region = { .top = 0, .left = 0, .width = 8, .height = 8 },
		.waveform_mode = 1, .update_mode = UPDATE_MODE_PARTIAL,
		.update_marker = 1, .temp = TEMP_USE_AMBIENT,
	};
	struct mxcfb_update_marker_data m = { .update_marker = 1 };
	ioctl(fbfd, MXCFB_SEND_UPDATE, &u);
	ioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m);
}

int main(int argc, char **argv)
{
	int mem = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem < 0)
		return perror("/dev/mem"), 1;
	wake();
	map(mem);
	if (argc >= 2 && !strcmp(argv[1], "regs")) {
		printf("WVADDR=%08x WB_ADDR=%08x RES=%08x (%ux%u) FORMAT=%08x TCE_CTRL=%08x STATUS_LUTS=%08x\n",
		       r[0x20 / 4], r[0x30 / 4], r[0x40 / 4], res_w, res_h, r[0x50 / 4], r[0x200 / 4],
		       r[0x440 / 4]);
		return 0;
	}
	if (argc == 5 && !strcmp(argv[1], "dump")) {
		unsigned int x = atoi(argv[2]), y = atoi(argv[3]), n = atoi(argv[4]);
		for (unsigned int i = 0; i < n; i++)
			printf("%04x%c", wb[(size_t)y * stride + x + i], i % 16 == 15 ? '\n' : ' ');
		printf("\n");
		return 0;
	}
	if ((argc == 9 || argc == 11) && !strcmp(argv[1], "fill")) {
		/* 可选 delay_ms nib：提交后 delay_ms 把方块左半像素 WB 的目标色（bits 7:4）改为 nib */
		int poke = argc == 11;
		double delay = poke ? atof(argv[9]) : 0;
		unsigned int nib = poke ? strtoul(argv[10], NULL, 0) : 0;
		unsigned int x = atoi(argv[2]), y = atoi(argv[3]), w = atoi(argv[4]), h = atoi(argv[5]);
		unsigned int gray = atoi(argv[6]), wf = atoi(argv[7]), full = atoi(argv[8]);
		int fd = fbfd;
		struct fb_fix_screeninfo fix;
		struct fb_var_screeninfo var;
		if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) || ioctl(fd, FBIOGET_VSCREENINFO, &var))
			return perror("fb"), 1;
		uint8_t *fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		unsigned int bpp = var.bits_per_pixel / 8;
		for (unsigned int j = 0; j < h; j++)
			for (unsigned int i = 0; i < w; i++)
				memset(fb + (size_t)(y + j) * fix.line_length + (x + i) * bpp, gray, bpp);
		struct mxcfb_update_data u = {
			.update_region = { .top = y, .left = x, .width = w, .height = h },
			.waveform_mode = wf, .update_mode = full ? UPDATE_MODE_FULL : UPDATE_MODE_PARTIAL,
			.update_marker = 77, .temp = TEMP_USE_AMBIENT,
		};
		size_t at = (size_t)(y + h / 2) * stride + x + w / 2;
		uint16_t last = wb[at];
		uint32_t lastl = r[0x440 / 4];
		double t0 = now_ms();
		printf("%8.2f wb=%04x luts=%08x\n", 0.0, last, lastl);
		if (ioctl(fd, MXCFB_SEND_UPDATE, &u))
			return perror("SEND_UPDATE"), 1;
		while (poke && now_ms() - t0 < delay)
			;
		if (poke) {
			double tp = now_ms() - t0;
			for (unsigned int j = 0; j < h; j++)
				for (unsigned int i = 0; i < w / 2; i++) {
					volatile uint16_t *p = &wb[(size_t)(y + j) * stride + x + i];
					*p = (*p & ~0xf0) | nib << 4;
				}
			printf("%8.2f poked left half -> %04x (took %.2fms)\n", tp, wb[(size_t)(y + h / 2) * stride + x],
			       now_ms() - t0 - tp);
		}
		while (now_ms() - t0 < 1500) {
			uint16_t v = wb[at];
			uint32_t l = r[0x440 / 4];
			if (v != last || l != lastl)
				printf("%8.2f wb=%04x luts=%08x\n", now_ms() - t0, v, l);
			last = v, lastl = l;
			usleep(300);
		}
		return 0;
	}
	fprintf(stderr, "usage: see source\n");
	return 2;
}
