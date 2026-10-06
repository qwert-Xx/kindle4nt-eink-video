/* SPDX-License-Identifier: GPL-2.0-only */
/* WB 直驱播放器（实验）：一个长 LUT 跑整屏，每个扫描帧由软件改写 WB 目标色，
 * 逐像素逐帧决定推黑 / 推白 / 不动。
 *
 * 机制（i.MX50 RM 24.3.8）：EPDC 每帧从内存读 WB（每像素 16 位：[15:12] LUT，
 * [7:4] 目标色，[3:0] 当前色）并按 LUT 当前帧的 256 项动作表查 (目标<<4|当前)。
 * 这里把用户波形 2（GC16）在所有温度档的指针临时指向一张合成表：每帧都是
 * “目标 0 推黑、15 推白、其它不动”，帧数 F。画布整屏写 0x88，FULL 提交后整屏
 * 都在这个 LUT 里；之后不再提交更新，只写 WB。LUT 走完 F 帧后立即再提交一次。
 * 退出时还原指针并用真正的 GC16 刷一次当前画面。
 *
 * 每个像素有位置 0..top（0=黑，top=白，top=n+2*extra，n 为当前温度 A2 推动帧数），
 * 每帧朝目标走一步。以 16 像素为一组用 NEON 计算，只算还在动的组；
 * 帧计数翻转时把变化的组按 32 字节写进 WB。漏帧时按实际经过的帧数补算位置。
 *
 * 用法：k4-wb <tcp_port> [extra=2] [frames=0(最大)]
 * 只改内存（波形表、WB、画布），不改驱动；需要 /dev/mem 可访问 RAM。 */
#include <arm_neon.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/mxcfb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define EPDC_BASE 0x41010000
#define W 800
#define H 600
#define NPIX (W * H)
#define NGRP (NPIX / 4)
#define FRAME (NPIX / 8)
#define C_BLACK 0x00
#define C_WHITE 0xf0
#define C_HOLD 0x80
#define WHEEL 64
#define LUT_FRAMES_MAX 1023 /* EPDC_DEBUG_LUTn 帧计数 10 位 */

static volatile uint32_t *reg;
static uint8_t *wv;
static volatile uint16_t *wb;
static unsigned int stride;
static int fbfd = -1;
static int32_t old_delay;
static struct fb_var_screeninfo old_var;
static int var_changed;
static size_t tt2;
static uint64_t saved2[64];
static unsigned int ntemps, patched;
static uint8_t *fb;
static struct fb_fix_screeninfo fix;

static uint8_t pending[FRAME];
static unsigned long seq_in;
static int eof;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int input_fd;

/* 每像素状态 1 字节：[4:0] 位置 0..top，[5] 目标为白，[7:6] WB 里正在执行的动作
 * （0 不动，1 推白，2 推黑）。全屏 480KB，忙的部分能留在缓存里。16 像素一组。 */
#define CH 16
#define NCH (NPIX / CH)
#define S_LV 0x1f
#define S_TW 0x20
#define S_CUR 0xc0
static uint8_t st[NPIX] __attribute__((aligned(64)));
static uint8_t target_bits[FRAME];
static uint8_t busy[NCH];
static int extra, top, lo, hi, nps;

/* 下一帧要写的组与它们的 WB 目标色（每组 16 字节） */
static uint32_t dirty[NCH];
static uint8_t staged[NCH][CH] __attribute__((aligned(16)));
static uint16_t dirty_slot[NCH]; /* 组在写表中的位置 + 1，0 表示不在 */
static unsigned int dirty_n;
static uint16_t wbbase;
static unsigned long writes;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
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

static void unpatch(void)
{
	for (unsigned int t = 0; patched && t < ntemps; t++)
		set64(tt2 + t * 8, saved2[t]);
	patched = 0;
}

static unsigned int marker = 10;

static void send_full(int wf)
{
	struct mxcfb_update_data u = {
		.update_region = { 0, 0, W, H }, .waveform_mode = wf,
		.update_mode = UPDATE_MODE_FULL, .update_marker = ++marker,
		.temp = TEMP_USE_AMBIENT,
	};
	if (ioctl(fbfd, MXCFB_SEND_UPDATE, &u))
		perror("SEND_UPDATE");
}

static void wait_marker(unsigned int mk)
{
	struct mxcfb_update_marker_data m = { .update_marker = mk };
	ioctl(fbfd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m);
}

static int active_lut = -1;

/* 结束：等长 LUT 走完，还原波形指针，把位置画到画布上用真 GC16 刷一次 */
static void finish(void)
{
	static int done;
	unsigned int mode = AUTO_UPDATE_MODE_AUTOMATIC_MODE;

	if (done || fbfd < 0)
		return;
	done = 1;
	if (active_lut >= 0)
		for (unsigned int y = 0; y < H; y++)
			for (unsigned int x = 0; x < W; x++)
				wb[(size_t)y * stride + x] = (active_lut << 12) | C_HOLD | 8;
	wait_marker(marker);
	unpatch();
	for (unsigned int y = 0; top && y < H; y++)
		for (unsigned int x = 0; x < W; x++)
			fb[(size_t)y * fix.line_length + x] =
				(st[y * W + x] & S_LV) * 2 >= top ? 0xff : 0x00;
	send_full(2);
	wait_marker(marker);
	if (var_changed && ioctl(fbfd, FBIOPUT_VSCREENINFO, &old_var))
		perror("restore format");
	ioctl(fbfd, MXCFB_SET_AUTO_UPDATE_MODE, &mode);
	ioctl(fbfd, MXCFB_SET_PWRDOWN_DELAY, &old_delay);
}

static void on_signal(int sig)
{
	(void)sig;
	exit(0);
}

static int open_input(int port)
{
	struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(port) };
	int one = 1, s, c;

	s = socket(AF_INET6, SOCK_STREAM, 0);
	if (s < 0 || setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
	    bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 1))
		return perror("listen"), -1;
	fprintf(stderr, "listening on port %d\n", port);
	c = accept(s, NULL, NULL);
	close(s);
	return c;
}

static void *reader(void *arg)
{
	static uint8_t buf[FRAME];
	(void)arg;
	for (;;) {
		size_t got = 0;
		while (got < FRAME) {
			ssize_t n = read(input_fd, buf + got, FRAME - got);
			if (n <= 0) {
				pthread_mutex_lock(&lock);
				eof = 1;
				pthread_mutex_unlock(&lock);
				return NULL;
			}
			got += n;
		}
		pthread_mutex_lock(&lock);
		memcpy(pending, buf, FRAME);
		seq_in++;
		pthread_mutex_unlock(&lock);
	}
}

/* 漏帧补算：WB 里的动作比预计多执行了 k 帧 */
static void catch_up(unsigned int k)
{
	uint8_t kk = k > 31 ? 31 : k;

	for (unsigned int c = 0; c < NCH; c++) {
		if (!busy[c])
			continue;
		uint8_t *a = st + c * CH;
		for (int x = 0; x < CH; x++) {
			uint8_t l = a[x] & S_LV, cur = a[x] >> 6;
			uint8_t up = -(uint8_t)(cur == 1), down = -(uint8_t)(cur == 2);
			uint8_t room_up = top - l, u = kk < room_up ? kk : room_up, d = kk < l ? kk : l;
			a[x] = (a[x] & ~S_LV) | (uint8_t)(l + (u & up) - (d & down));
		}
	}
}

/* 算下一帧：每组一步（NEON）；动作变化的组把新目标色放入写表；整组不动且 WB 里
 * 也是不动时转为空闲。两端各 extra 档是补推区；目标反转时先跳出补推区，从端点立即反向。 */
static void compute(void)
{
	const uint8x16_t vlo = vdupq_n_u8(lo), vhi = vdupq_n_u8(hi), vtop = vdupq_n_u8(top);
	const uint8x16_t mlv = vdupq_n_u8(S_LV), mtw = vdupq_n_u8(S_TW), mkeep = vdupq_n_u8(S_TW | S_CUR);
	const uint8x16_t white = vdupq_n_u8(C_WHITE), hold = vdupq_n_u8(C_HOLD);
	const uint8x16_t one = vdupq_n_u8(1), two = vdupq_n_u8(2);

	dirty_n = 0;
	const uint32_t *b4 = (const uint32_t *)busy;
	for (unsigned int i = 0; i < NCH / 4; i++) {
		if (!b4[i])
			continue;
		for (unsigned int c = i * 4; c < i * 4 + 4; c++) {
			if (!busy[c])
				continue;
			uint8_t *a = st + c * CH;
			uint8x16_t s0 = vld1q_u8(a);
			uint8x16_t p = vandq_u8(s0, mlv), tm = vtstq_u8(s0, mtw), t = vandq_u8(tm, vtop);
			uint8x16_t cu = vshrq_n_u8(s0, 6);
			p = vbslq_u8(tm, vmaxq_u8(p, vlo), vminq_u8(p, vhi));
			uint8x16_t up = vcltq_u8(p, t), down = vcgtq_u8(p, t);
			/* up/down 为 0xff（-1）：p - up + down 即走一步 */
			p = vaddq_u8(vsubq_u8(p, up), down);
			vst1q_u8(a, vorrq_u8(p, vandq_u8(s0, mkeep)));
			uint8x16_t nk = vorrq_u8(vandq_u8(up, one), vandq_u8(down, two));
			uint64x2_t dq = vreinterpretq_u64_u8(veorq_u8(nk, cu));
			uint64x2_t rq = vreinterpretq_u64_u8(nk);
			if (vgetq_lane_u64(dq, 0) | vgetq_lane_u64(dq, 1)) {
				vst1q_u8(staged[dirty_n], vorrq_u8(vandq_u8(up, white),
							     vbicq_u8(hold, vorrq_u8(up, down))));
				dirty[dirty_n++] = c;
				dirty_slot[c] = dirty_n;
			} else if (!(vgetq_lane_u64(rq, 0) | vgetq_lane_u64(rq, 1))) {
				busy[c] = 0;
			}
		}
	}
}

/* 帧计数翻转后立即写入：每组 16 像素扩成 16 位后两次 16 字节存储，并更新状态里的动作 */
static void flush_list(void)
{
	const uint16x8_t base = vdupq_n_u16(wbbase);
	const uint8x16_t vw = vdupq_n_u8(C_WHITE), z = vdupq_n_u8(0), mcur = vdupq_n_u8(S_CUR);
	const uint8x16_t c1 = vdupq_n_u8(0x40), c2 = vdupq_n_u8(0x80);

	for (unsigned int i = 0; i < dirty_n; i++) {
		unsigned int c = dirty[i], o = c * CH;
		uint8x16_t v = vld1q_u8(staged[i]);
		uint16_t *d = (uint16_t *)wb + (size_t)(o / W) * stride + o % W;

		vst1q_u16(d, vorrq_u16(vmovl_u8(vget_low_u8(v)), base));
		vst1q_u16(d + 8, vorrq_u16(vmovl_u8(vget_high_u8(v)), base));
		uint8x16_t k = vorrq_u8(vandq_u8(vceqq_u8(v, vw), c1), vandq_u8(vceqq_u8(v, z), c2));
		uint8x16_t s0 = vld1q_u8(st + o);
		vst1q_u8(st + o, vorrq_u8(vbicq_u8(s0, mcur), k));
		dirty_slot[c] = 0;
	}
	writes += dirty_n;
	dirty_n = 0;
}

/* 新视频帧：变化的字节查表展开成 8 个目标位置，所在组标记为忙 */
static int take_target(unsigned long *last_seq, int *done)
{
	static uint8_t local[FRAME];
	int got = 0;

	pthread_mutex_lock(&lock);
	*done = eof && seq_in == *last_seq;
	if (seq_in != *last_seq) {
		memcpy(local, pending, FRAME);
		*last_seq = seq_in;
		got = 1;
	}
	pthread_mutex_unlock(&lock);
	if (!got)
		return 0;
	const uint64_t *a = (const uint64_t *)local;
	uint64_t *b = (uint64_t *)target_bits;
	for (int i = 0; i < FRAME / 8; i++) {
		if (a[i] == b[i])
			continue;
		b[i] = a[i];
		/* 8 字节 = 64 像素 = 4 组：每 2 字节展开成 16 个“目标为白”位 */
		static const uint8_t bitsel[16] = { 0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1,
						    0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1 };
		const uint8x16_t sel = vld1q_u8(bitsel), mtw = vdupq_n_u8(S_TW);
		for (int k = 0; k < 4; k++) {
			const uint8_t *bb = local + i * 8 + k * 2;
			uint8x16_t bits = vcombine_u8(vdup_n_u8(bb[0]), vdup_n_u8(bb[1]));
			uint8x16_t tw = vandq_u8(vtstq_u8(bits, sel), mtw);
			uint8_t *a = st + (i * 4 + k) * CH;
			vst1q_u8(a, vorrq_u8(vbicq_u8(vld1q_u8(a), mtw), tw));
		}
		memset(busy + i * 4, 1, 4);
	}
	return 1;
}

static int lut_frame(void)
{
	return (reg[(0x540 + 16 * active_lut) / 4] >> 5) & 0x3ff;
}

static int lut_running(void)
{
	return reg[0x440 / 4] & (1u << active_lut);
}

/* LUT 重启后 WB 目标色全是不动：下一帧要执行的动作（写表里的，或状态里正在执行的）
 * 不是不动的组全部写入 */
static void resync(void)
{
	for (unsigned int c = 0; c < NCH; c++) {
		if (!busy[c] || dirty_slot[c])
			continue;
		uint8_t *a = st + c * CH, codes[CH];
		int any = 0;
		for (int x = 0; x < CH; x++) {
			uint8_t k = a[x] >> 6;
			codes[x] = k == 1 ? C_WHITE : k == 2 ? C_BLACK : C_HOLD;
			any |= k;
		}
		if (any) {
			memcpy(staged[dirty_n], codes, CH);
			dirty[dirty_n++] = c;
			dirty_slot[c] = dirty_n;
		}
	}
	flush_list();
}

/* 提交一次整屏长 LUT 并等它开始；处理把 WB 目标色全部写成画布的 8（不动），
 * 再把所有正在推动的像素重新写入 */
static int start_lut(void)
{
	uint32_t before = reg[0x440 / 4] & 0xffff;
	double t0 = now();

	send_full(2);
	active_lut = -1;
	while (now() - t0 < 1.0) {
		uint32_t l = reg[0x440 / 4] & 0xffff & ~before;
		if (l) {
			active_lut = __builtin_ctz(l);
			break;
		}
		/* 提交由驱动的工作线程完成；实时线程须让出 CPU */
		usleep(200);
	}
	if (active_lut < 0)
		return -1;
	wbbase = (active_lut << 12) | 8;
	resync();
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return fprintf(stderr, "usage: k4-wb <tcp_port> [extra=2] [frames=0(max)]\n"), 2;
	int port = atoi(argv[1]);
	extra = argc > 2 ? atoi(argv[2]) : 2;
	unsigned int frames = argc > 3 ? atoi(argv[3]) : 0;
	int mem = open("/dev/mem", O_RDWR | O_SYNC);
	struct fb_var_screeninfo var;
	pthread_t th;

	fbfd = open("/dev/fb0", O_RDWR);
	if (mem < 0 || fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &old_var) ||
	    ioctl(fbfd, MXCFB_GET_PWRDOWN_DELAY, &old_delay))
		return perror("open"), 1;
	int32_t no = -1;
	unsigned int amode = AUTO_UPDATE_MODE_REGION_MODE;
	ioctl(fbfd, MXCFB_SET_PWRDOWN_DELAY, &no);
	ioctl(fbfd, MXCFB_SET_AUTO_UPDATE_MODE, &amode);
	atexit(finish);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	/* Y8 画布：驱动 Y8 快速路径下整屏提交只需拷贝，LUT 重启快 */
	var = old_var;
	var.bits_per_pixel = 8;
	var.grayscale = 1;
	if (ioctl(fbfd, FBIOPUT_VSCREENINFO, &var) || ioctl(fbfd, FBIOGET_FSCREENINFO, &fix))
		return perror("set Y8"), 1;
	var_changed = 1;
	fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fb == MAP_FAILED)
		return perror("mmap fb"), 1;

	/* 起点：真 GC16 整屏刷白（同时唤醒 EPDC，之后才能读寄存器） */
	for (unsigned int y = 0; y < H; y++)
		memset(fb + (size_t)y * fix.line_length, 0xff, W);
	send_full(2);
	wait_marker(marker);

	reg = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, mem, EPDC_BASE);
	stride = ((reg[0x40 / 4] & 0x1fff) + 3) & ~3u;
	wv = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, mem, reg[0x20 / 4]);
	wb = mmap(NULL, ((size_t)stride * H * 2 + 4095) & ~4095ul, PROT_READ | PROT_WRITE,
		  MAP_SHARED, mem, reg[0x30 / 4]);
	if (reg == MAP_FAILED || wv == MAP_FAILED || wb == MAP_FAILED)
		return perror("mmap"), 1;

	/* 当前温度档 A2（硬件模式 4）的推动帧数与动作码 */
	unsigned int temp = reg[0x1a0 / 4];
	ntemps = (le64(le64(0)) - le64(0)) / 8;
	size_t a2 = le64(le64(4 * 8) + temp * 8);
	uint8_t black = 0, white = 0;
	for (uint64_t f = 0; f < le64(a2); f++) {
		const uint8_t *act = wv + a2 + 8 + f * 256;
		if (!black)
			black = act[0x0f];
		if (!white)
			white = act[0xf0];
		nps += !!act[0xf0];
	}
	top = nps + 2 * extra;
	lo = extra;
	hi = top - extra;

	/* 合成表放在硬件模式 3 的数据区（用户波形号映射不到它） */
	size_t base = le64(le64(3 * 8)), end = le64(4 * 8), room = (end - base - 8) / 256;
	if (!frames)
		frames = room < LUT_FRAMES_MAX ? room : LUT_FRAMES_MAX;
	if (frames > LUT_FRAMES_MAX || frames > room)
		return fprintf(stderr, "frames too many (room %zu)\n", room), 1;
	uint8_t act[256];
	for (unsigned int i = 0; i < 256; i++)
		act[i] = (i >> 4) == 0 ? black : (i >> 4) == 15 ? white : 0;
	set64(base, frames);
	for (unsigned int f = 0; f < frames; f++)
		memcpy(wv + base + 8 + (size_t)f * 256, act, 256);
	tt2 = le64(2 * 8);
	for (unsigned int t = 0; t < ntemps; t++)
		saved2[t] = le64(tt2 + t * 8);
	patched = 1;
	for (unsigned int t = 0; t < ntemps; t++)
		set64(tt2 + t * 8, base);

	if (top > S_LV)
		return fprintf(stderr, "top %d exceeds state bits\n", top), 1;
	memset(st, top | S_TW, sizeof(st));
	memset(target_bits, 0xff, sizeof(target_bits));
	for (unsigned int y = 0; y < H; y++)
		memset(fb + (size_t)y * fix.line_length, 0x88, W);
	fprintf(stderr, "temp %u: A2 pushes %d, extra %d, LUT %u frames\n", temp, nps, extra, frames);

	input_fd = open_input(port);
	if (input_fd < 0)
		return 1;
	pthread_create(&th, NULL, reader, NULL);
	/* 计算线程用实时调度，只让中断打断；接收线程保持普通优先级。锁定内存避免缺页 */
	struct sched_param sp = { .sched_priority = 50 };
	if (sched_setscheduler(0, SCHED_FIFO, &sp))
		perror("SCHED_FIFO");
	if (mlockall(MCL_CURRENT | MCL_FUTURE))
		perror("mlockall");

	unsigned long last_seq = 0, used = 0, ticks = 0, lost = 0, lost_compute = 0, restarts = 0;
	int done = 0;
	double t0 = now(), tcomp = 0, tmax = 0, tw = 0, twmax = 0, t_restart = 0, t_restart_max = 0;

	if (start_lut())
		return fprintf(stderr, "LUT did not start\n"), 1;
	double t_start0 = now() - t0;
	int prev = lut_frame();
	uint32_t G = 0;
	double p_cpu = 0, p_take = 0, p_comp = 0, p_comp_max = 0, p_take_max = 0;
	unsigned long p_busy = 0, p_busy_at_max = 0;
	used += take_target(&last_seq, &done);
	compute();
	for (;;) {
		/* 帧计数刚翻转：立即写入这一帧的变化，再算下一帧 */
		double ta = now();
		flush_list();
		double tf = now() - ta;
		tw += tf;
		if (tf > twmax)
			twmax = tf;
		double t1 = now();
		used += take_target(&last_seq, &done);
		double t2 = now();
		struct timespec cs, ce;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cs);
		compute();
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ce);
		p_cpu += (ce.tv_sec - cs.tv_sec) + (ce.tv_nsec - cs.tv_nsec) / 1e9;
		double tb = now() - ta;
		{
			unsigned int nb = 0;
			for (unsigned int c = 0; c < NCH; c++)
				nb += busy[c];
			p_take += t2 - t1;
			p_comp += now() - t2;
			p_busy += nb;
			if (now() - t2 > p_comp_max) {
				p_comp_max = now() - t2;
				p_busy_at_max = nb;
			}
			if (t2 - t1 > p_take_max)
				p_take_max = t2 - t1;
		}
		tcomp += tb;
		if (tb > tmax)
			tmax = tb;
		ticks++;
		if (done)
			break;
		if (tb < 0.0095)
			usleep((unsigned int)((0.0100 - tb) * 1e6));
		int f;
		while ((f = lut_frame()) == prev && lut_running())
			;
		if (!lut_running()) {
			double tr = now();
			wait_marker(marker);
			restarts++;
			if (start_lut())
				return fprintf(stderr, "LUT restart failed\n"), 1;
			prev = lut_frame();
			tr = now() - tr;
			t_restart += tr;
			if (tr > t_restart_max)
				t_restart_max = tr;
			continue;
		}
		int d = (prev - f + 1024) % 1024, e = (f - prev + 1024) % 1024;
		int step = d < e ? d : e;
		if (step > 1) {
			lost += step - 1;
			if (tb > 0.0110)
				lost_compute += step - 1;
			/* WB 里的动作多执行了 step-1 帧，补进位置；已算好的下一帧动作照写，
			 * 到端点的像素最多多推一帧 */
			catch_up(step - 1);
		}
		G += step;
		prev = f;
	}
	double dt = now() - t0;
	fprintf(stderr, "take_target mean %.2fms max %.2fms; compute mean %.2fms max %.2fms (busy groups at max %lu); mean busy groups %.0f of %d\n",
		1e3 * p_take / ticks, 1e3 * p_take_max, 1e3 * p_comp / ticks, 1e3 * p_comp_max, p_busy_at_max,
		(double)p_busy / ticks, NCH);
	fprintf(stderr, "compute thread cpu mean %.2fms\n", 1e3 * p_cpu / ticks);
	fprintf(stderr, "video frames used %lu, ticks %lu in %.1fs = %.1f/s, LUT frames %u\n"
		"write mean %.2fms max %.2fms, write+compute mean %.2fms max %.2fms\n"
		"frames lost %lu (compute >11ms: %lu), first LUT start %.1fms, restarts %lu (%.1fms total, max %.1fms)\n"
		"WB 16-pixel group writes %lu\n",
		used, ticks, dt, ticks / dt, G,
		1e3 * tw / ticks, 1e3 * twmax, 1e3 * tcomp / ticks, 1e3 * tmax,
		lost, lost_compute, 1e3 * t_start0, restarts, 1e3 * t_restart, 1e3 * t_restart_max,
		writes);
	return 0;
}
