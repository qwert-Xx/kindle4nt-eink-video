/* SPDX-License-Identifier: GPL-2.0-only */
/* 按键控制播放：确认键（K4 五向键中键，KEY_F24）在停止时从头播放，播放中暂停（画面停在
 * 当前帧），暂停时继续；主页键（KEY_HOME）停止播放、清屏并回到开头。播完后回到停止状态。
 * 视频为 gzip 压缩的 800x600 monob 原始帧（每帧 60000 字节，30 fps），由 zcat 送入
 * k4-wb 标准输入。暂停冻结解压进程组，播放器收不到新帧而保持当前画面；停止时给播放器
 * SIGTERM，等它停止 WB 会话并刷白。
 * 用法：k4-keyplay <video.raw.gz> <k4-wb 路径> */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/mxcfb.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PLAY_KEY KEY_F24
#define STOP_KEY KEY_HOME
#define MAX_INPUTS 8

/* 整屏写白并以 GC16 刷新 */
static void clear_white(void)
{
	int fd = open("/dev/fb0", O_RDWR);
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;

	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
		perror("fb0");
		if (fd >= 0)
			close(fd);
		return;
	}
	uint8_t *p = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p != MAP_FAILED) {
		memset(p, 0xff, (size_t)fix.line_length * var.yres);
		struct mxcfb_update_data u = {
			.update_region = { 0, 0, var.xres, var.yres }, .waveform_mode = 2,
			.update_mode = UPDATE_MODE_FULL, .update_marker = 1, .temp = TEMP_USE_AMBIENT,
		};
		struct mxcfb_update_marker_data m = { .update_marker = 1 };
		if (!ioctl(fd, MXCFB_SEND_UPDATE, &u))
			ioctl(fd, MXCFB_WAIT_FOR_UPDATE_COMPLETE, &m);
		munmap(p, fix.smem_len);
	}
	close(fd);
}

static int open_inputs(struct pollfd *pfd)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	int n = 0;

	while (d && (e = readdir(d)) && n < MAX_INPUTS) {
		char path[300];
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			pfd[n++] = (struct pollfd){ .fd = fd, .events = POLLIN };
	}
	if (d)
		closedir(d);
	return n;
}

static pid_t feeder = -1, player = -1;
static volatile sig_atomic_t quit;

static void on_term(int sig)
{
	(void)sig;
	quit = 1;
}

/* 解压进程组写管道，播放器从管道读 */
static void start(const char *video, const char *bin)
{
	int pp[2];

	if (pipe(pp))
		return perror("pipe");
	feeder = fork();
	if (feeder == 0) {
		char cmd[512];
		setpgid(0, 0);
		dup2(pp[1], STDOUT_FILENO);
		close(pp[0]);
		close(pp[1]);
		snprintf(cmd, sizeof(cmd), "exec zcat '%s'", video);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	player = fork();
	if (player == 0) {
		dup2(pp[0], STDIN_FILENO);
		close(pp[0]);
		close(pp[1]);
		execl(bin, bin, "-w", "-", (char *)NULL);
		_exit(127);
	}
	close(pp[0]);
	close(pp[1]);
}

static void stop(void)
{
	if (player > 0) {
		kill(player, SIGTERM);
		waitpid(player, NULL, 0);
	}
	if (feeder > 0) {
		/* 暂停中的进程组先恢复，才能处理 SIGTERM */
		kill(-feeder, SIGTERM);
		kill(-feeder, SIGCONT);
		waitpid(feeder, NULL, 0);
	}
	player = feeder = -1;
}

int main(int argc, char **argv)
{
	if (argc != 3)
		return fprintf(stderr, "usage: k4-keyplay <video.raw.gz> <k4-wb>\n"), 2;
	struct pollfd pfd[MAX_INPUTS];
	int n = open_inputs(pfd);
	int paused = 0;

	if (!n)
		return fprintf(stderr, "no input devices\n"), 1;
	signal(SIGPIPE, SIG_IGN);
	/* 服务停止时先结束播放，播放器才能停止 WB 会话并恢复设置 */
	struct sigaction sa = { .sa_handler = on_term };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	clear_white();
	fprintf(stderr, "ready: %d input devices\n", n);
	for (;;) {
		if (poll(pfd, n, 500) < 0 && errno != EINTR)
			return perror("poll"), 1;
		if (quit) {
			stop();
			return 0;
		}
		/* 播完：回到停止状态 */
		if (player > 0 && waitpid(player, NULL, WNOHANG) == player) {
			player = -1;
			stop();
			paused = 0;
			fprintf(stderr, "finished\n");
		}
		for (int i = 0; i < n; i++) {
			struct input_event ev;
			if (!(pfd[i].revents & POLLIN) || read(pfd[i].fd, &ev, sizeof(ev)) != sizeof(ev))
				continue;
			if (ev.type != EV_KEY || ev.value != 1)
				continue;
			if (ev.code == PLAY_KEY) {
				if (player < 0) {
					fprintf(stderr, "play\n");
					start(argv[1], argv[2]);
				} else {
					kill(-feeder, paused ? SIGCONT : SIGSTOP);
					paused = !paused;
					fprintf(stderr, paused ? "pause\n" : "resume\n");
				}
			} else if (ev.code == STOP_KEY) {
				if (player > 0)
					stop();
				else
					clear_white();
				paused = 0;
				fprintf(stderr, "stop\n");
			}
		}
	}
}
