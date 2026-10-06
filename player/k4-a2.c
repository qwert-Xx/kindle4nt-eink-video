/* SPDX-License-Identifier: GPL-2.0-only */
/* A2 播放器（对照组）：每个新视频帧把变化写进 8 位画布，按变化包围盒提交 A2 PARTIAL；
 * SNAPSHOT 下 SEND 返回即可改画布，在途更新不超过 depth 个，冲突由驱动按提交顺序重试。
 * 用法：k4-a2 [tcp_port=0(stdin)] [depth=8] [waveform=4(a2)]
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/mxcfb.h>
#include <pthread.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define W 800
#define H 600
#define FRAME (W * H / 8)

static uint8_t pending[FRAME];
static unsigned long seq_in;
static int eof;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int fbfd = -1, restore_needed;
static struct fb_var_screeninfo restore_var;
static int32_t restore_pwr;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void restore(void)
{
	unsigned int mode = AUTO_UPDATE_MODE_AUTOMATIC_MODE;

	if (!restore_needed)
		return;
	if (ioctl(fbfd, FBIOPUT_VSCREENINFO, &restore_var))
		perror("restore format");
	unsigned int scheme = UPDATE_SCHEME_QUEUE_AND_MERGE;

	ioctl(fbfd, MXCFB_SET_UPDATE_SCHEME, &scheme);
	ioctl(fbfd, MXCFB_SET_AUTO_UPDATE_MODE, &mode);
	ioctl(fbfd, MXCFB_SET_PWRDOWN_DELAY, &restore_pwr);
}


static void on_signal(int sig)
{
	(void)sig;
	exit(0);
}

static int input_fd;

/* 端口参数非 0 时监听 TCP 并接受一个连接作为输入，否则读 stdin。 */
static int open_input(int port)
{
	struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(port) };
	int one = 1, s, c;

	if (!port)
		return 0;
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


static uint8_t shown[FRAME];

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 0, depth = argc > 2 ? atoi(argv[2]) : 8;
	unsigned int wf = argc > 3 ? atoi(argv[3]) : 4;
	int fd = open("/dev/fb0", O_RDWR);
	struct fb_var_screeninfo var, old_var;
	struct fb_fix_screeninfo fix;
	unsigned int mode = AUTO_UPDATE_MODE_REGION_MODE, scheme = UPDATE_SCHEME_SNAPSHOT, marker = 0, sent = 0;
	unsigned long last_seq = 0, frames = 0;
	static double sent_at[1 << 16];
	double lat_sum = 0, lat_max = 0;
	unsigned int lat_n = 0;
	int32_t old_delay, no_delay = -1;
	pthread_t th;
	static uint8_t cur[FRAME];

	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &old_var))
		return perror("fb0"), 1;
	ioctl(fd, MXCFB_GET_PWRDOWN_DELAY, &old_delay);
	fbfd = fd;
	restore_var = old_var;
	restore_pwr = old_delay;
	restore_needed = 1;
	atexit(restore);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	var = old_var;
	var.bits_per_pixel = 8;
	var.grayscale = 1;
	if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix))
		return perror("set Y8"), 1;
	uint8_t *fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED)
		return perror("mmap"), 1;
	ioctl(fd, MXCFB_SET_PWRDOWN_DELAY, &no_delay);
	ioctl(fd, MXCFB_SET_AUTO_UPDATE_MODE, &mode);

	/* 起点：整屏 GC16 刷白（QUEUE 下，含断电后的整幅恢复），之后切 SNAPSHOT。 */
	memset(fb, 0xff, fix.line_length * H);
	struct mxcfb_update_data u = {
		.update_region = { 0, 0, W, H }, .waveform_mode = 2,
		.update_mode = UPDATE_MODE_FULL, .update_marker = ++marker,
		.temp = TEMP_USE_AMBIENT,
	};
	struct mxcfb_update_marker_data m = { .update_marker = marker };
	ioctl(fd, MXCFB_SEND_UPDATE, &u);
	ioctl(fd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m);
	if (ioctl(fd, MXCFB_SET_UPDATE_SCHEME, &scheme))
		return perror("SNAPSHOT unsupported"), 1;
	memset(shown, 0xff, FRAME);
	u.waveform_mode = wf;
	u.update_mode = UPDATE_MODE_PARTIAL;

	input_fd = open_input(port);
	if (input_fd < 0)
		return 1;
	pthread_create(&th, NULL, reader, NULL);
	double t0 = now();
	for (;;) {
		int done;

		pthread_mutex_lock(&lock);
		done = eof && seq_in == last_seq;
		int fresh = seq_in != last_seq;
		if (fresh) {
			memcpy(cur, pending, FRAME);
			last_seq = seq_in;
		}
		pthread_mutex_unlock(&lock);
		if (done)
			break;
		if (!fresh) {
			usleep(2000);
			continue;
		}
		frames++;
		int x0 = W, x1 = -1, y0 = H, y1 = -1;
		for (int y = 0; y < H; y++) {
			const uint8_t *c = cur + y * W / 8;
			uint8_t *p = shown + y * W / 8, *row = fb + y * fix.line_length;
			for (int b = 0; b < W / 8; b++) {
				if (c[b] == p[b])
					continue;
				for (int k = 0; k < 8; k++)
					row[b * 8 + k] = (c[b] & (0x80 >> k)) ? 0xff : 0x00;
				p[b] = c[b];
				if (b * 8 < x0) x0 = b * 8;
				if (b * 8 + 8 > x1) x1 = b * 8 + 8;
				if (y < y0) y0 = y;
				y1 = y + 1;
			}
		}
		if (x1 < 0)
			continue;
		if (marker - 1 >= (unsigned int)depth) {
			m.update_marker = marker - depth + 1;
			ioctl(fd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m);
			double l = now() - sent_at[m.update_marker & 0xffff];
			lat_sum += l;
			lat_n++;
			if (l > lat_max)
				lat_max = l;
		}
		u.update_region = (struct mxcfb_rect){ .top = y0, .left = x0,
			.width = x1 - x0, .height = y1 - y0 };
		u.update_marker = ++marker;
		sent_at[marker & 0xffff] = now();
		if (ioctl(fd, MXCFB_SEND_UPDATE, &u))
			perror("SEND_UPDATE");
		sent++;
	}
	for (unsigned int i = 2; i <= marker; i++) {
		m.update_marker = i;
		ioctl(fd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m);
	}
	double dt = now() - t0;
	fprintf(stderr, "video frames %lu, updates %u in %.1fs = %.1f updates/s; submit-to-complete (lower bound) mean %.0fms max %.0fms\n",
		frames, sent, dt, sent / dt, lat_n ? 1e3 * lat_sum / lat_n : 0, 1e3 * lat_max);
	return 0;
}
