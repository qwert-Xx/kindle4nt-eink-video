<!-- SPDX-License-Identifier: CC-BY-4.0 -->
# k4-eink-video

Kindle 4 墨水屏动画播放的实验与优化：以工作缓冲（WB）逐像素直驱播放器为主要方案，
提供 A2 播放器作为对照，以及探针、主机脚本、主机模型测试和实验记录。
输入是 800×600、30 fps 的黑白 monob 原始帧，每帧 60000 字节。

## 与主线的关系

内核来自独立的 Kindle 主线 Git 仓库，`kernel/mainline.lock` 锁定
`refactor/k4-modularization` 分支上的提交 `5fec5b15368aad94c0434c2e18588a0593a72159`。
分支名用于说明来源，构建始终以完整提交为准。`kernel/patches/series` 顺序列出
本仓库的 `git format-patch` 补丁，当前系列为空。实验代码与记录在本仓库独立维护。

构建脚本在指定输出目录的 `src` 创建 detached Git 工作树，以 `git am` 应用系列，
随后调用该源码树的 `project/build.py`。相同提交与补丁系列可复用工作树；
HEAD、已应用系列或已跟踪文件不匹配时重建该工作树。
源码工作树与产物保留在输出目录供继续构建，脚本会打印保留位置。
清理源码树用 `git -C "$KERNEL_SRC" worktree remove "$OUT/src"`，随后删除自己的输出目录。

## 目录

| 目录 | 内容 |
| --- | --- |
| `kernel/` | 主线锁、补丁系列、离线构建入口 |
| `player/` | `k4-wb` 和 `k4-a2`、交叉编译 Makefile |
| `tools/` | WB/波形探针、写入基准、扫描时序测量 |
| `host/` | 测试片生成、SSH 播放、相机录制、录像对比 |
| `tests/` | 实际 WB 计算/写入函数与标量模型对拍、NEON 步进对拍 |
| `docs/WB-DIRECT.md` | 现行 WB 机制、历史性能数据及限制 |
| `docs/experiments/` | 按日期保存的实验依据与结论 |
| `LICENSES/` | GPL-2.0-only 与 CC-BY-4.0 许可文本 |

## 依赖

主机脚本需要 bash、Python 3 标准库、ffmpeg（含 libx264、lavfi、v4l2）、ssh 和
支持 `-N` 的 nc；构建用户态需要 `arm-linux-gnueabihf-gcc`、make 和主线的
`include/uapi/linux/mxcfb.h`；测试需要 `qemu-arm-static`。
内核还使用主线构建入口本身所需的 make、交叉 binutils、bison、flex、bc、
开发头文件等主机构建依赖，详见主线 `project/docs/BUILD.md`。
源码与工具应事先在本地准备，构建脚本没有下载步骤。

设备需要匹配的 mxcfb 接口、已加载的面板波形、root 权限、`/dev/fb0`；
WB 播放器和内存探针还需要 `/dev/mem` 可访问驱动的 DMA 内存。
固件、波形、相机原录像、密钥和设备身份由使用者在仓库外管理。

## 快速开始

以下变量由使用者填写，源码路径和设备路径均为参数。

```bash
KERNEL_SRC="<主线源码目录>"
OUT="$PWD/build"
python3 kernel/build.py --kernel-src "$KERNEL_SRC" --out "$OUT"
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
  --remote-player '<设备播放器路径>' --raw build/demo.raw --port 5000 -- 2 0
# 自带参数：k4-wb PORT [extra=2] [LUT帧数=0(最大)]
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

## 当前限制

锁定主线只支持 QUEUE/QUEUE_AND_MERGE，SNAPSHOT 返回 `-EINVAL`；
因此 `k4-a2` 的 SNAPSHOT 对照需要对应内核补丁才能实现该实验语义。
锁定提交没有研究分支的 Y8 快速路径和温度缓存，WB 重启延迟应以实际设备为准。
补丁系列为空表示构建主线本身，不含研究分支的这些优化。

WB 方案直接改写驱动 DMA 内存，驱动不知情；模式 3 数据区在实验后需要重启恢复。
它没有直流平衡，温度推动参数只在启动时读取，长期残影和面板寿命影响未验证。
`wvinfo` 直接读寄存器，调用前必须先保持 EPDC 上电；其它探针中的唤醒流程见源码。
`epdc-timing` 按 32 位画布访问，必须在对应画布格式下使用。

QEMU 对拍与交叉构建属于主机验证，历史设备数据见实验记录。
本仓库初始化未执行设备播放、相机采集、冷启动或长时间面板验证。

代码与脚本采用 GPL-2.0-only，文档采用 CC-BY-4.0；许可文本见 `LICENSES/`。
