/* SPDX-License-Identifier: GPL-2.0-only */
/* 测 WB（/dev/mem O_SYNC 映射）写入速度：先读出整屏 WB，再原样写回（内容不变）。
 * 对比 64 位逐组写、NEON 32 字节连续写、普通内存 memcpy。EPDC 空闲时运行。 */
#include <arm_neon.h>
#include <linux/fb.h>
#include <linux/mxcfb.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define EPDC_BASE 0x41010000
#define NPIX (800 * 600)

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int fbfd;
static int32_t old_delay;

static void restore(void)
{
	ioctl(fbfd, MXCFB_SET_PWRDOWN_DELAY, &old_delay);
}

int main(void)
{
	int mem = open("/dev/mem", O_RDWR | O_SYNC);
	/* 先禁用自动关电并做一次 8x8 PARTIAL 唤醒 EPDC：时钟关闭时读寄存器会挂住总线 */
	int32_t no = -1;
	fbfd = open("/dev/fb0", O_RDWR);
	if (fbfd < 0 || ioctl(fbfd, MXCFB_GET_PWRDOWN_DELAY, &old_delay))
		return perror("fb"), 1;
	ioctl(fbfd, MXCFB_SET_PWRDOWN_DELAY, &no);
	atexit(restore);
	struct mxcfb_update_data u = {
		.update_region = { .top = 0, .left = 0, .width = 8, .height = 8 },
		.waveform_mode = 1, .update_mode = UPDATE_MODE_PARTIAL,
		.update_marker = 1, .temp = TEMP_USE_AMBIENT,
	};
	struct mxcfb_update_marker_data m = { .update_marker = 1 };
	if (ioctl(fbfd, MXCFB_SEND_UPDATE, &u) || ioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m))
		return perror("wake"), 1;
	setvbuf(stdout, NULL, _IONBF, 0);
	volatile uint32_t *r = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, mem, EPDC_BASE);
	uint16_t *wb = mmap(NULL, NPIX * 2, PROT_READ | PROT_WRITE, MAP_SHARED, mem, r[0x30 / 4]);
	static uint16_t copy[NPIX] __attribute__((aligned(32)));
	static uint16_t ram[NPIX] __attribute__((aligned(32)));
	double t;

	t = now();
	memcpy(copy, wb, sizeof(copy));
	printf("read  whole WB (memcpy from uncached): %.2fms\n", now() - t);

	t = now();
	volatile uint64_t *w64 = (volatile uint64_t *)wb;
	const uint64_t *c64 = (const uint64_t *)copy;
	for (int i = 0; i < NPIX / 4; i++)
		w64[i] = c64[i];
	printf("write 64-bit x %d groups: %.2fms\n", NPIX / 4, now() - t);

	t = now();
	for (int i = 0; i < NPIX / 4; i += 8)
		w64[i] = c64[i];
	printf("write 64-bit, every 8th group (%d): %.2fms\n", NPIX / 32, now() - t);

	t = now();
	for (int i = 0; i < NPIX; i += 8)
		vst1q_u16(wb + i, vld1q_u16(copy + i));
	printf("write NEON 16B x %d: %.2fms\n", NPIX / 8, now() - t);

	t = now();
	for (int i = 0; i < NPIX; i += 16) {
		uint16_t *d = wb + i;
		const uint16_t *s2 = copy + i;
		__asm__ volatile("vld1.64 {d0-d3}, [%1:128]\n\tvst1.64 {d0-d3}, [%0:128]"
				 : : "r"(d), "r"(s2) : "d0", "d1", "d2", "d3", "memory");
	}
	printf("write NEON 32B x %d: %.2fms\n", NPIX / 16, now() - t);

	t = now();
	memcpy(ram, copy, sizeof(ram));
	printf("memcpy normal RAM whole: %.2fms\n", now() - t);

	printf("unchanged: %s\n", memcmp(copy, wb, sizeof(copy)) ? "NO" : "yes");
	return 0;
}
