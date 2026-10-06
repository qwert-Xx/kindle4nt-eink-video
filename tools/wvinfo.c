/* SPDX-License-Identifier: GPL-2.0-only */
/* 打印 EPDC 波形内存布局：每个模式、每个温度档的帧数与偏移，以及 EPDC_TEMP。 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define EPDC_BASE 0x41010000
int main(void)
{
	int mem = open("/dev/mem", O_RDONLY | O_SYNC);
	volatile uint32_t *r = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, mem, EPDC_BASE);
	uint32_t wv = r[0x20 / 4];
	const uint8_t *w = mmap(NULL, 4 << 20, PROT_READ, MAP_SHARED, mem, wv);
	if (w == MAP_FAILED)
		return perror("mmap"), 1;
	uint64_t m0, t0;
	memcpy(&m0, w, 8);
	unsigned int modes = m0 / 8;
	memcpy(&t0, w + m0, 8);
	unsigned int temps = (t0 - m0) / 8;
	printf("WVADDR=%08x modes=%u temps=%u EPDC_TEMP=%u\n", wv, modes, temps, r[0x1a0 / 4]);
	for (unsigned int m = 0; m < modes; m++) {
		uint64_t mt;
		memcpy(&mt, w + m * 8, 8);
		printf("mode %u:", m);
		for (unsigned int t = 0; t < temps; t++) {
			uint64_t e, f;
			memcpy(&e, w + mt + t * 8, 8);
			memcpy(&f, w + e, 8);
			printf(" %llu@%llx", (unsigned long long)f, (unsigned long long)e);
		}
		printf("\n");
	}
	return 0;
}
