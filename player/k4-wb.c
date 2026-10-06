/* SPDX-License-Identifier: GPL-2.0-only */
/* WB 直驱：驱动拥有合成波形、LUT 重启与扫描同步；用户态在每个 EOF 后
 * 改写 mmap 工作缓冲。每像素一字节位置/目标/动作状态，16 像素 NEON 步进，
 * 漏帧按驱动累计帧差补算，重启后重写仍在推动的组。
 * 用法：k4-wb [-w] <tcp_port|-> [extra=2] [bias=40]
 *   tcp_port：监听并接受一个 TCP 连接作为输入，由发送端控制节奏（如 ffmpeg -re）；
 *   -：从标准输入读，按源帧率 30 fps 自行定节奏；
 *   -w：退出时整屏刷白，否则按估计位置刷出最后画面。
 */
#include <arm_neon.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/fb.h>
#include <linux/mxcfb.h>
#include <linux/k4-epdc-wb.h>
#include <errno.h>
#include <inttypes.h>
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

#define W 800
#define H 600
#define NPIX (W * H)
#define NGRP (NPIX / 4)
#define FRAME (NPIX / 8)
#define C_BLACK 0x00
#define C_WHITE 0xf0
#define C_HOLD 0x80
#define WHEEL 64

static volatile uint16_t *wb;
static unsigned int stride;
static int fbfd = -1;
static int wbfd = -1, session;
static struct k4_epdc_wb_info wbinfo;
static volatile sig_atomic_t interrupted;
static struct fb_var_screeninfo old_var;
static int var_changed;
static uint8_t *fb;
static struct fb_fix_screeninfo fix;

static uint8_t pending[FRAME];
static unsigned long seq_in;
static int eof;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int input_fd;

/* 每像素状态 1 字节：[4:0] 位置，[5] 目标为白，[7:6] WB 里正在执行的动作
 * （0 不动，1 推白，2 推黑）。全屏 480KB，忙的部分能留在缓存里。16 像素一组。 */
#define CH 16
#define NCH (NPIX / CH)
#define S_LV 0x1f
#define S_TW 0x20
#define S_CUR 0xc0
static uint8_t st[NPIX] __attribute__((aligned(64)));
static uint8_t target_bits[FRAME];
static uint8_t busy[NCH];
static int extra, top, nps;
/* 位置 margin 为黑端、margin+top 为白端；两侧余量保存漏帧时端点外多执行的推动，
 * 目标反转时一并推回，使净推动始终等于位置变化。 */
static int margin;
#define SOURCE_FPS 30
static int pace_input, exit_white;
/* 目标反转时可跳过补推区（不推动即离开端点），这会让净推动偏离位置变化；偏离量
 * 按像素记在 bias（净推动 − 位置变化），|bias| 不超过 bias_cap，到达上限后只能
 * 逐档推回。跳过使像素在端点附近往返时被拉回端点，抵消墨水响应不对称造成的漂移。 */
static int8_t bias[NPIX] __attribute__((aligned(16)));
static int bias_cap;
/* 还账：像素停在黑端且 bias 偏白（>0）时每帧往黑推、停在白端且偏黑（<0）时每帧往白推，
 * 位置不变、bias 每帧向 0 移一格，直到还清或目标改变。饱和端多推看不出或只使黑更深
 * （A 机实测黑端 22 次后每次约深 0.35%，白端 18 次后基本不变）。连续推动使每次还账只
 * 写两次 WB；间歇推动会在大面积到达端点时每隔几帧整屏写两次。参照 E Ink US10672350B2
 * 的逐像素冲量账户：偏离后在后续驱动中主动减小账户。 */

/* 下一帧要写的组与它们的 WB 目标色（每组 16 字节） */
static uint32_t dirty[NCH];
static uint8_t staged[NCH][CH] __attribute__((aligned(16)));
static uint16_t dirty_slot[NCH]; /* 组在写表中的位置 + 1，0 表示不在 */
static unsigned int dirty_n;
static unsigned long writes;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
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

/* 播放期间每个 cpufreq 策略的最低频率设为其最高频率。等待帧边界时 CPU 空闲，
 * 按需调频会降到低档；大面积变化时升频来不及，计算和写入随之变慢而漏帧。 */
static glob_t cpufreq;
static char cpufreq_saved[16][32];

static int sysfs_rw(const char *dir, const char *name, char *buf, size_t len, int write_it)
{
	char path[256];
	int fd, n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fd = open(path, write_it ? O_WRONLY : O_RDONLY);
	if (fd < 0)
		return -1;
	n = write_it ? write(fd, buf, strlen(buf)) : read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	if (!write_it)
		buf[n] = 0;
	return 0;
}

static void cpufreq_hold_max(void)
{
	char max[32];

	if (glob("/sys/devices/system/cpu/cpufreq/policy*", 0, NULL, &cpufreq))
		return;
	for (size_t i = 0; i < cpufreq.gl_pathc && i < 16; i++) {
		const char *d = cpufreq.gl_pathv[i];

		if (sysfs_rw(d, "scaling_min_freq", cpufreq_saved[i], sizeof(cpufreq_saved[i]), 0) ||
		    sysfs_rw(d, "scaling_max_freq", max, sizeof(max), 0) ||
		    sysfs_rw(d, "scaling_min_freq", max, sizeof(max), 1)) {
			fprintf(stderr, "cpufreq: cannot hold %s at maximum\n", d);
			cpufreq_saved[i][0] = 0;
		}
	}
}

static void cpufreq_restore(void)
{
	for (size_t i = 0; i < cpufreq.gl_pathc && i < 16; i++)
		if (cpufreq_saved[i][0])
			sysfs_rw(cpufreq.gl_pathv[i], "scaling_min_freq", cpufreq_saved[i], 0, 1);
}

/* STOP 撤销 WB 映射并等待驱动停止；以估计位置画布做一次 GC16 整屏恢复。 */
static void finish(void)
{
	static int done;
	unsigned int mode = AUTO_UPDATE_MODE_AUTOMATIC_MODE;

	if (done || fbfd < 0)
		return;
	done = 1;
	if (session) {
		if (ioctl(wbfd, K4_EPDC_WB_STOP))
			perror("WB STOP");
		session = 0;
	}
	for (unsigned int y = 0; top && y < H; y++)
		for (unsigned int x = 0; x < W; x++)
			fb[(size_t)y * fix.line_length + x] =
				exit_white || (st[y * W + x] & S_LV) * 2 >= 2 * margin + top ? 0xff : 0x00;
	send_full(2);
	wait_marker(marker);
	if (var_changed && ioctl(fbfd, FBIOPUT_VSCREENINFO, &old_var))
		perror("restore format");
	ioctl(fbfd, MXCFB_SET_AUTO_UPDATE_MODE, &mode);
	if (wb && wb != MAP_FAILED)
		munmap((void *)wb, wbinfo.size);
	if (wbfd >= 0)
		close(wbfd);
	cpufreq_restore();
}

static void on_signal(int sig)
{
	(void)sig;
	interrupted = 1;
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
	struct timespec due;
	(void)arg;
	clock_gettime(CLOCK_MONOTONIC, &due);
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
		if (pace_input) {
			/* 标准输入没有发送端节奏：第 i 帧在开始后 i/SOURCE_FPS 秒发布 */
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);
			due.tv_nsec += 1000000000L / SOURCE_FPS;
			if (due.tv_nsec >= 1000000000L)
				due.tv_sec++, due.tv_nsec -= 1000000000L;
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
			uint8_t room_up = S_LV - l, u = kk < room_up ? kk : room_up, d = kk < l ? kk : l;
			a[x] = (a[x] & ~S_LV) | (uint8_t)(l + (u & up) - (d & down));
		}
	}
}

/* 算下一帧：每组一步（NEON）；动作变化的组把新目标色放入写表；整组不动且 WB 里
 * 也是不动时转为空闲。两端各 extra 档是补推区。目标反转时从当前位置逐档推回，
 * 不跳过补推区：每次推动都改变位置，净推动始终等于位置变化，直流不累积。 */
static void compute(void)
{
	const uint8x16_t vwhite = vdupq_n_u8(margin + top), vblack = vdupq_n_u8(margin);
	const uint8x16_t mlv = vdupq_n_u8(S_LV), mtw = vdupq_n_u8(S_TW), mkeep = vdupq_n_u8(S_TW | S_CUR);
	const uint8x16_t white = vdupq_n_u8(C_WHITE), hold = vdupq_n_u8(C_HOLD);
	const uint8x16_t one = vdupq_n_u8(1), two = vdupq_n_u8(2);
	const uint8x16_t vlo = vdupq_n_u8(margin + extra), vhi = vdupq_n_u8(margin + top - extra);
	const uint8x16_t vcap = vdupq_n_u8(bias_cap);
	const int8x16_t zero_s = vdupq_n_s8(0);

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
			uint8x16_t p = vandq_u8(s0, mlv), tm = vtstq_u8(s0, mtw);
			uint8x16_t cu = vshrq_n_u8(s0, 6);
			/* 跳过补推区：目标白时 p 升到 lo、目标黑时降到 hi，跳过量受 bias 余量限制。
			 * bias 以 u8 模运算表示 int8；cap ± bias 落在 [0, 2·cap]。 */
			int8_t *bp = bias + c * CH;
			uint8x16_t b = vreinterpretq_u8_s8(vld1q_s8(bp));
			{
				uint8x16_t d = vbslq_u8(tm, vqsubq_u8(vlo, p), vqsubq_u8(p, vhi));
				uint8x16_t room = vbslq_u8(tm, vaddq_u8(vcap, b), vsubq_u8(vcap, b));
				uint8x16_t j = vminq_u8(d, room);
				p = vbslq_u8(tm, vaddq_u8(p, j), vsubq_u8(p, j));
				b = vbslq_u8(tm, vsubq_u8(b, j), vaddq_u8(b, j));
			}
			/* 只朝目标端推；越过目标端的过冲保持不动，留到反转时推回 */
			uint8x16_t up = vandq_u8(vcltq_u8(p, vwhite), tm);
			uint8x16_t down = vbicq_u8(vcgtq_u8(p, vblack), tm);
			/* 停在目标端、账户方向可还的像素 */
			int8x16_t bs = vreinterpretq_s8_u8(b);
			uint8x16_t owe_b = vbicq_u8(vandq_u8(vcleq_u8(p, vblack), vcgtq_s8(bs, zero_s)), tm);
			uint8x16_t owe_w = vandq_u8(vandq_u8(vcgeq_u8(p, vwhite), vcltq_s8(bs, zero_s)), tm);
			uint8x16_t rb = owe_b, rw = owe_w;
			/* up/down 为 0xff（-1）：p - up + down 即走一步；还账推动不改位置 */
			p = vaddq_u8(vsubq_u8(p, up), down);
			b = vaddq_u8(vsubq_u8(b, rw), rb);
			vst1q_s8(bp, vreinterpretq_s8_u8(b));
			up = vorrq_u8(up, rw);
			down = vorrq_u8(down, rb);
			vst1q_u8(a, vorrq_u8(p, vandq_u8(s0, mkeep)));
			uint8x16_t owe = vorrq_u8(owe_b, owe_w);
			uint64x2_t oq = vreinterpretq_u64_u8(owe);
			uint8x16_t nk = vorrq_u8(vandq_u8(up, one), vandq_u8(down, two));
			uint64x2_t dq = vreinterpretq_u64_u8(veorq_u8(nk, cu));
			uint64x2_t rq = vreinterpretq_u64_u8(nk);
			if (vgetq_lane_u64(dq, 0) | vgetq_lane_u64(dq, 1)) {
				vst1q_u8(staged[dirty_n], vorrq_u8(vandq_u8(up, white),
							     vbicq_u8(hold, vorrq_u8(up, down))));
				dirty[dirty_n++] = c;
				dirty_slot[c] = dirty_n;
			} else if (!(vgetq_lane_u64(rq, 0) | vgetq_lane_u64(rq, 1) |
				     vgetq_lane_u64(oq, 0) | vgetq_lane_u64(oq, 1))) {
				busy[c] = 0;
			}
		}
	}
}

/* 帧计数翻转后立即写入：每组 16 像素扩成 16 位后两次 16 字节存储，并更新状态里的动作 */
static void flush_list(void)
{
	const uint16x8_t hold = vdupq_n_u16(wbinfo.hold);
	const uint16x8_t white = vdupq_n_u16(wbinfo.white), black = vdupq_n_u16(wbinfo.black);
	const uint8x16_t vw = vdupq_n_u8(C_WHITE), z = vdupq_n_u8(0), mcur = vdupq_n_u8(S_CUR);
	const uint8x16_t c1 = vdupq_n_u8(0x40), c2 = vdupq_n_u8(0x80);

	for (unsigned int i = 0; i < dirty_n; i++) {
		unsigned int c = dirty[i], o = c * CH;
		uint8x16_t v = vld1q_u8(staged[i]);
		uint16_t *d = (uint16_t *)wb + (size_t)(o / W) * stride + o % W;

		uint16x8_t low = vmovl_u8(vget_low_u8(v)), high = vmovl_u8(vget_high_u8(v));
		vst1q_u16(d, vbslq_u16(vceqq_u16(low, vdupq_n_u16(C_WHITE)), white,
			vbslq_u16(vceqq_u16(low, vdupq_n_u16(C_BLACK)), black, hold)));
		vst1q_u16(d + 8, vbslq_u16(vceqq_u16(high, vdupq_n_u16(C_WHITE)), white,
			vbslq_u16(vceqq_u16(high, vdupq_n_u16(C_BLACK)), black, hold)));
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

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "-w"))
		exit_white = 1, argc--, argv++;
	if (argc < 2)
		return fprintf(stderr, "usage: k4-wb [-w] <tcp_port|-> [extra=2] [bias=40]\n"), 2;
	pace_input = !strcmp(argv[1], "-");
	int port = pace_input ? 0 : atoi(argv[1]);
	extra = argc > 2 ? atoi(argv[2]) : 2;
	bias_cap = argc > 3 ? atoi(argv[3]) : 40;
	if (argc > 4 || extra < 0 || bias_cap < 0 || bias_cap > 127)
		return fprintf(stderr, "invalid arguments\n"), 2;
	struct fb_var_screeninfo var;
	pthread_t th;

	fbfd = open("/dev/fb0", O_RDWR);
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &old_var))
		return perror("open"), 1;
	unsigned int amode = AUTO_UPDATE_MODE_REGION_MODE;
	ioctl(fbfd, MXCFB_SET_AUTO_UPDATE_MODE, &amode);
	atexit(finish);
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	/* Y8 画布用于 GC16 起点和结束时的估计画面。 */
	var = old_var;
	var.bits_per_pixel = 8;
	var.grayscale = 1;
	if (ioctl(fbfd, FBIOPUT_VSCREENINFO, &var) || ioctl(fbfd, FBIOGET_FSCREENINFO, &fix))
		return perror("set Y8"), 1;
	var_changed = 1;
	fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fb == MAP_FAILED)
		return perror("mmap fb"), 1;

	/* 起点：真 GC16 整屏刷白。 */
	for (unsigned int y = 0; y < H; y++)
		memset(fb + (size_t)y * fix.line_length, 0xff, W);
	send_full(2);
	wait_marker(marker);

	wbfd = open("/dev/k4-epdc-wb", O_RDWR);
	double start_time = now();
	if (wbfd < 0 || ioctl(wbfd, K4_EPDC_WB_START, &wbinfo))
		return perror("WB START"), 1;
	session = 1;
	double t_start0 = now() - start_time;
	if (wbinfo.width != W || wbinfo.height != H || wbinfo.stride < W ||
	    wbinfo.size < (size_t)wbinfo.stride * H * 2)
		return fprintf(stderr, "unsupported WB geometry\n"), 1;
	stride = wbinfo.stride;
	wb = mmap(NULL, wbinfo.size, PROT_READ | PROT_WRITE, MAP_SHARED, wbfd, 0);
	if (wb == MAP_FAILED)
		return perror("mmap WB"), 1;
	nps = wbinfo.push_frames;
	if (nps > S_LV || extra > (S_LV - nps) / 2)
		return fprintf(stderr, "push/extra exceeds state bits\n"), 1;
	top = nps + 2 * extra;
	margin = (S_LV - top) / 2;

	memset(st, (margin + top) | S_TW, sizeof(st));
	memset(target_bits, 0xff, sizeof(target_bits));
	fprintf(stderr, "A2 pushes %d, extra %d, LUT %u frames\n", nps, extra, wbinfo.frames_per_lut);

	input_fd = pace_input ? STDIN_FILENO : open_input(port);
	if (input_fd < 0)
		return 1;
	pthread_create(&th, NULL, reader, NULL);
	cpufreq_hold_max();
	/* 计算线程用实时调度，只让中断打断；接收线程保持普通优先级。锁定内存避免缺页 */
	struct sched_param sp = { .sched_priority = 50 };
	if (sched_setscheduler(0, SCHED_FIFO, &sp))
		perror("SCHED_FIFO");
	if (mlockall(MCL_CURRENT | MCL_FUTURE))
		perror("mlockall");

	unsigned long last_seq = 0, used = 0, ticks = 0, lost = 0, lost_compute = 0, restarts = 0;
	int done = 0;
	double t0 = now(), tcomp = 0, tmax = 0, tw = 0, twmax = 0, t_restart = 0, t_restart_max = 0;

	/* 每次 START 后游标清零；后续 ioctl 原样传回上次的 frame/generation。 */
	struct k4_epdc_wb_frame scan = {0};
	if (ioctl(wbfd, K4_EPDC_WB_WAIT_FRAME, &scan))
		return perror("WAIT_FRAME"), 1;
	uint64_t prev = scan.frame, G = 0;
	uint32_t generation = scan.generation;
	int failed = 0;
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
		if (done || interrupted)
			break;
		double tr;
wait_frame:
		tr = now();
		/* 计算期间已有 EOF 时立即取得进度，不再多等待一个扫描帧。 */
		if (ioctl(wbfd, K4_EPDC_WB_WAIT_FRAME, &scan)) {
			if (errno != EINTR || !interrupted) {
				perror("WAIT_FRAME");
				failed = 1;
			}
			break;
		}
		uint64_t step = scan.frame - prev;
		if (step > 1) {
			lost += step - 1;
			if (tb > 0.0110)
				lost_compute += step - 1;
			catch_up(step > 32 ? 31 : step - 1);
		}
		if (scan.generation != generation) {
			restarts += scan.generation - generation;
			generation = scan.generation;
			resync();
			tr = now() - tr;
			t_restart += tr;
			if (tr > t_restart_max)
				t_restart_max = tr;
		}
		G += step;
		prev = scan.frame;
		/* 仅 generation 变化时 resync 后继续等待，不多算一次位置步。 */
		if (!step)
			goto wait_frame;
	}
	double dt = now() - t0;
	fprintf(stderr, "take_target mean %.2fms max %.2fms; compute mean %.2fms max %.2fms (busy groups at max %lu); mean busy groups %.0f of %d\n",
		1e3 * p_take / ticks, 1e3 * p_take_max, 1e3 * p_comp / ticks, 1e3 * p_comp_max, p_busy_at_max,
		(double)p_busy / ticks, NCH);
	fprintf(stderr, "compute thread cpu mean %.2fms\n", 1e3 * p_cpu / ticks);
	fprintf(stderr, "video frames used %lu, ticks %lu in %.1fs = %.1f/s, LUT frames %" PRIu64 "\n"
		"write mean %.2fms max %.2fms, write+compute mean %.2fms max %.2fms\n"
		"frames lost %lu (compute >11ms: %lu), first LUT start %.1fms, restarts %lu (boundary wait %.1fms total, max %.1fms)\n"
		"WB 16-pixel group writes %lu\n",
		used, ticks, dt, ticks / dt, G,
		1e3 * tw / ticks, 1e3 * twmax, 1e3 * tcomp / ticks, 1e3 * tmax,
		lost, lost_compute, 1e3 * t_start0, restarts, 1e3 * t_restart, 1e3 * t_restart_max,
		writes);
	return failed;
}
