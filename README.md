<!-- SPDX-License-Identifier: CC-BY-4.0 -->
# k4-eink-video

Kindle 4 墨水屏动画播放的实验与优化：以工作缓冲（WB）逐像素直驱播放器为主要方案，
提供 A2 播放器作为对照，以及探针、主机脚本、主机模型测试和实验记录。
输入是 800×600、30 fps 的黑白 monob 原始帧，每帧 60000 字节。

## 与主线的关系

内核基于 Linux v6.6.157 与公开仓库
[kindle4nt-linux](https://github.com/qwert-Xx/kindle4nt-linux) 的 Kindle 4 补丁系列。
`kernel/upstream.lock` 锁定 Linux 版本、下载地址、SHA256 和 kindle4nt-linux 的完整提交。
本仓库的 `kernel/patches/series` 提供 EPDC WB 直驱会话接口，补丁采用
`git format-patch` 格式。实验代码与记录在本仓库独立维护。

构建脚本将指定 Linux 源码复制到输出目录的 `src`，依次应用 kindle4nt-linux 的
`kernel/patches/series` 和本仓库的补丁系列，再调用 kindle4nt-linux 的
`project/build.py`。相同锁与补丁系列可复用源码树；锁或补丁系列变化时重建。
源码与产物保留在输出目录，清理时删除该输出目录即可。

## 目录

| 目录 | 内容 |
| --- | --- |
| `kernel/` | 公开源码锁、补丁系列、离线构建入口 |
| `player/` | `k4-wb` 和 `k4-a2`、交叉编译 Makefile |
| `tools/` | WB/波形探针、写入基准、扫描时序测量、墨水响应曲线测量（wbcurve） |
| `host/` | 测试片生成、SSH 播放、相机录制、录像对比 |
| `tests/` | 实际 WB 计算/写入函数与标量模型对拍、NEON 步进对拍 |
| `docs/WB-DIRECT.md` | 现行 WB 机制、历史性能数据及限制 |
| `docs/experiments/` | 按日期保存的实验依据与结论 |
| `LICENSES/` | GPL-2.0-only 与 CC-BY-4.0 许可文本 |

## 依赖

主机脚本需要 bash、Python 3 标准库、ffmpeg（含 libx264、lavfi、v4l2）、ssh 和
支持 `-N` 的 nc；构建用户态需要 `arm-linux-gnueabihf-gcc`、make 和公开 Kindle 补丁提供的
`include/uapi/linux/mxcfb.h` 与补丁提供的 `include/uapi/linux/k4-epdc-wb.h`；测试需要 `qemu-arm-static`。
内核还使用 kindle4nt-linux 构建入口所需的 make、交叉 binutils、bison、flex、bc、
开发头文件等主机构建依赖，详见 kindle4nt-linux 的 `project/docs/BUILD.md`。
源码与工具应事先在本地准备，构建脚本没有下载步骤。

设备需要匹配的 mxcfb 接口、已加载的面板波形、root 权限、`/dev/fb0`；
WB 播放器需要补丁提供的 `/dev/k4-epdc-wb`；内存探针需要 `/dev/mem`。
固件、波形、相机原录像、密钥和设备身份由使用者在仓库外管理。

## 快速开始

从 [kernel.org](https://www.kernel.org/pub/linux/kernel/v6.x/linux-6.6.157.tar.xz)
取得 Linux v6.6.157 压缩包，按 `kernel/upstream.lock` 中的 SHA256 校验后解压。
检出公开 Kindle 补丁仓库的锁定提交，再指定这两个源码目录；构建脚本离线运行。
以下变量由使用者填写，源码路径和设备路径均为参数。

```bash
LINUX_SRC="<Linux v6.6.157 解压目录>"
KINDLE4NT="<kindle4nt-linux 检出目录>"
git clone https://github.com/qwert-Xx/kindle4nt-linux "$KINDLE4NT"
git -C "$KINDLE4NT" checkout d179b232a51351ac3cad5b1acdbfda9917530ad0
OUT="$PWD/build"
python3 kernel/build.py --linux "$LINUX_SRC" --kindle4nt "$KINDLE4NT" --out "$OUT"
make -C player KERNEL_SRC="$OUT/src"
make -C tools KERNEL_SRC="$OUT/src"
make -C player test KERNEL_SRC="$OUT/src"
```

`CC` 可覆盖交叉编译器。两个 Makefile 默认 `KERNEL_SRC=../build/src`。
产物是静态 ARM 程序，默认针对 Cortex-A8/NEON，使用 hard-float ABI 与
`-mno-unaligned-access`。内核输出在 `$OUT/kernel`，其中有 zImage、板级 DTB、
安装后的模块及 `build-report.json`。设备部署由使用者另行安排。

生成三段拼接测试片（移动形状 30 秒、生命游戏 10 秒、Mandelbrot 10 秒）：

```bash
bash host/make-demo.sh build/demo.raw
# 单段：anim / life / mandelbrot
bash host/make-demo.sh build/anim.raw anim
```

完整测试片 1500 帧、90,000,000 字节。生命游戏使用随机初态，各次片段可不同。

播放时指定 SSH 目标、上传的本地二进制、设备上的完整可执行路径、原片与 TCP 端口。
`--` 后参数在端口后原样传给播放器。`--ssh-option` 每次传一个 SSH 参数；
若 SSH 使用别名，可用 `--stream-host` 指定 nc 实际连接的主机。

```bash
bash host/play.sh --target root@host --player player/k4-wb \
  --remote-player '<设备播放器路径>' --raw build/demo.raw --port 5000 -- 2
# 自带参数：k4-wb [-w] PORT|- [extra=2] [bias=40]（- 为标准输入、按 30 fps 定节奏，-w 退出时刷白）
# A2 对照（须内核支持 SNAPSHOT）：k4-a2 PORT [depth=8] [waveform=4]
bash host/play.sh --target root@host --player player/k4-a2 \
  --remote-player '<设备播放器路径>' --raw build/demo.raw --port 5000 \
  --ssh-option -i --ssh-option '<私钥路径>' -- 8 4
```

脚本经 SSH 上传程序、启动并等待 `listening`，然后以 `ffmpeg -re | nc` 推流。
设备日志保存在指定播放器路径加 `.log` 的文件中，结束后完整打印；播放器正常退出会
执行自身的显示恢复。连接参数遵循 SSH 的正常主机密钥检查规则。

相机录制与播放可在两个终端同时运行：

```bash
bash host/record.sh '<相机设备>' 1920x1080 60 build/camera.mp4
python3 host/compare.py --video build/camera.mp4 --raw build/demo.raw \
  --crop '<X:Y:WIDTH:HEIGHT>' --rotation 180 --transpose 2 \
  --output build/compare.mp4
```

裁剪先于录像旋转；角度为顺时针。`--transpose` 是原片的 ffmpeg transpose 编号，
默认不转置。例子的方向来自历史 A 机录像，应按实际取景修改。
互相关在 24×18 灰度图、默认 5 fps 上搜索，精度 0.2 秒，可用 `--sample-fps` 调整。
输出 `offset_seconds` 满足 `camera[t + offset]` 对应 `raw[t]`；正值代表录像较晚。
并排输出左为录像、右为原片，按偏移裁去开头并在较短输入结束时停止。

## 按键播放（设备端）

`player/k4-keyplay` 监听输入设备：确认键（K4 五向键中键，`KEY_F24`）在停止时从头播放、
播放中暂停（画面停在当前帧）、暂停时继续；主页键（`KEY_HOME`）停止播放并清屏，回到开头。
视频为 gzip 压缩的 800×600 monob 原始帧（30 fps），由 `zcat` 送入 `k4-wb -w -`；暂停冻结
解压进程，播放器按输入节奏停在当前画面。停止需等当前 LUT 走完（最长约 12 秒）才能清屏。
`device/k4-keyplay.initd` 与 `device/k4-keyplay.confd` 是对应的 OpenRC 服务及配置，安装为
`/etc/init.d/k4-keyplay`、`/etc/conf.d/k4-keyplay` 后用 `rc-update add k4-keyplay default` 启用。

## 当前限制

锁定的公开 Kindle 补丁包含 Y8 快速路径、按原厂每 60 秒缓存的 Papyrus 温度和 UPDATE_SCHEME_SNAPSHOT，
`k4-a2` 使用 mxcfb SNAPSHOT；`k4-wb` 使用补丁提供的 START/WAIT_FRAME/STOP 与 WB mmap，
驱动自动重启固定 LUT，以 EOF 帧边界同步写入。

WB 会话独占面板，普通 SEND_UPDATE 与前端自动刷新返回 EBUSY。STOP、最后一个 fd
关闭及挂起会结束会话；STOP 撤销旧 WB 映射，访问它会产生 SIGBUS，下一次普通更新
按整屏恢复历史。它没有直流平衡，温度推动参数只在启动时读取，长期残影和面板寿命
影响未验证。
`wvinfo` 直接读寄存器，调用前必须先保持 EPDC 上电；其它探针中的唤醒流程见源码。
`epdc-timing` 按 32 位画布访问，必须在对应画布格式下使用。

QEMU 对拍与交叉构建属于主机验证，设备数据见实验记录。

代码与脚本采用 GPL-2.0-only，文档采用 CC-BY-4.0；许可文本见 `LICENSES/`。
