<!-- SPDX-License-Identifier: CC-BY-4.0 -->
# K4 EPDC DIRECT 驱动

## 指令与波形

DIRECT 的规范编号为 7，UAPI 名称为 `WAVEFORM_MODE_K4_DIRECT`，属于
K4 扩展；`k4-epd-update --waveform direct` 通过现有 mxcfb 请求提交。
前端只读 state 中的 `waveform_mode` 名称表包含 `direct`。
显式 ioctl 请求使用自身的波形字段。
AUTO 只选择 DU/GC16。
用户态用 REGION 自动刷新模式、8 位灰度画布、无旋转和 PARTIAL 提交
指令帧。高四位为 0/1 的像素推黑，14/15 推白，其它值不动；连续同向
驱动可交替写 0x00/0x10 或 0xE0/0xF0，让 PARTIAL 检出像素变化。
这些值进入面板工作历史，应用自行管理后续指令与普通图像刷新。
QUEUE 类更新在异步处理阶段读取画布，SEND 成功表示已排队；
SNAPSHOT 更新在 SEND 成功返回前捕获区域，播放器可立即改写画布。

WRF 每帧是 256 个动作字节，下标为 `(np << 4) | cp`，其中 np 是
目标灰度，cp 是当前灰度。依据是提供的原厂 DU 表观察：`T[15][8]`
有驱动序列而 `T[8][15]` 全零；DU 只驱动到黑/白，因此高四位表示
目标。提供的 A2 观察中 `T[0][15]` 连续动作 1，`T[15][0]` 连续动作 2，
分别推向黑和白。这些观察是下标与动作方向的依据，加载器按文件内容
推导动作值。

加载器按文件 mode map 定位 A2。每个温度档从 `T[0][15]` 帧序列取
首个非零动作作为推黑值，从 `T[15][0]` 取首个非零动作作为推白值。
合成 DIRECT 对所有 cp 使用同一规则：np 为 0/1 用推黑值，14/15
用推白值，其它 np 用 0；每档恰好一帧。没有 A2 或所选温度档任一
极端序列全零时，请求在异步处理阶段返回 `EOPNOTSUPP`，其它模式
继续按原文件内容运行。等待中的 marker 可收到该错误，owner 的
flush 也反映队列处理结果；已完成并移除的 marker 遵循现有等待语义。

WBF 解码和外部 WRF 加载均通过大小计算与写入两遍布局，生成
`modes+1` 个 LE64 模式表项及温度表、帧数和动作数据。原有模式的帧数、
动作字节及模式编号保持一致。控制器模式字段为 8 位，源文件已经有
256 个模式时使用原有 256 项表，DIRECT 返回 `EOPNOTSUPP`。
DIRECT 与其它波形的请求分别排队，合并算法不会将 DIRECT 改为 GC16。

owner 与 framebuffer 只读 sysfs state 的 `direct_push_frames=N` 是最近一次成功硬件
提交所用温度档中，A2 `T[15][0]` 的非零帧数，包含初始化提交；首次
提交前为 0。它表示原 A2 推白持续帧数，DIRECT 本身始终只有一帧。
缺少 A2 时该计数为 0；拒绝或未提交的请求不更新计数。

## SNAPSHOT 区域捕获

`MXCFB_SET_UPDATE_SCHEME` 与 owner SET/GET 接口支持 SNAPSHOT（0）、
QUEUE（1）和 QUEUE_AND_MERGE（2），默认值为 QUEUE_AND_MERGE。
当前 mxcfb UAPI 提供 SET_UPDATE_SCHEME，scheme 的读取由 owner getter 完成。
切换在 owner mutex 下立即生效，已排队请求保持自身的内容来源。

SNAPSHOT 的 ioctl 和前端自动刷新均通过 `imx50_epdc_send_update()`，
在发布队列和 marker 前，以源格式逐行复制请求区域到该更新私有的
`kvmalloc` 缓冲。source 指向紧凑副本，保留格式、旋转及反相属性；
owner 单独保存按原画布尺寸旋转后的面板区域，buffer prepare 使用该
区域定位输出。源大小、步长和乘加溢出在拷贝前校验。
分配失败同步返回 `-ENOMEM`，请求和 marker 不进入队列。

后续 prepare、AUTO 直方图及碰撞重处理均读副本。碰撞保留更新对象
及图像槽，marker 和顺序沿用 QUEUE 的路径；正常完成、失败或队列
取消经统一 finish 释放副本。不同副本的 source 指针不同，因此即使
scheme 切到 QUEUE_AND_MERGE，捕获的请求也各自独立。
历史恢复时捕获区域使用 FULL 更新模式，区域内容及位置保持捕获值。

原厂依据为 原厂 Linux 2.6.31 源码中的
`drivers/video/mxc/mxc_epdc_fb.c:1569-1593` 的三种 scheme，
`:2425-2436` 的 SNAPSHOT 中间缓冲取得和 `-ENOMEM`，
`:2493-2501` 的 QUEUE 异步分支，以及 `:2508-2545` 的同步处理、
面板坐标调整和处理后排队。此实现同步捕获原始区域，转换和硬件
提交由现有异步管线完成。原厂 SET 等待当前更新完成（`:1576-1580`）；
本实现即时切换，内容来源保存在更新对象中，以支持队列内 scheme 切换。

## Y8 快速路径

`imx50_epdc_prepare_update` 先执行现有格式、源大小、区域、目标大小
校验，再将 source invert 与请求反相 flag 异或。仅在源为 Y8、旋转为
0、生效 flags 为 0、histogram 指针为 NULL 时，将请求区域按行 memcpy
到目标缓冲。该路径没有中间分配，也不调用 PxP。owner 仅对 AUTO
传入直方图指针；其它请求不读取直方图。

等价性来自 PxP 灰度系数 `77 + 150 + 29 = 256`：Y8 扩展得到
R=G=B，转换后的字节等于输入。现有中间缓冲将宽高按 8 对齐、填黑，
无旋转时 crop_x/crop_y 为 0，只拷出区域宽高，所以填充不进入输出。
直接复制复用该布局的校验和 update 矩形，只写每行区域宽度，保留
目标行间空隙；不对齐的 left/width 不改变这个关系。需要直方图、
旋转或 LUT 变换的请求使用 PxP 路径。

## 环境温度缓存

EPDC 提交通过 `k4_papyrus_cached_temperature()` 取得最近成功转换的
毫摄氏度温度并求波形档。Papyrus 在 probe 完成和恢复后立即调度
采样，随后每次采样结束后延迟 60 秒重排队，挂起和关闭时取消。
采样睡眠中的芯片时按现有驱动方式唤醒并恢复原状态。首次尚无
有效样本时，提交同步测量一次并填充缓存；失败沿用现有错误返回。
hwmon 保持每次现场测量，显示电源 monitor 保持现有过热检查。

原厂依据为 原厂 Linux 2.6.31 源码中的
`drivers/regulator/papyrus-regulator.c:65`、`:1014-1054` 的周期和缓存，
`:1033-1037` 的临时唤醒，`:716`、`:1151` 的立即调度，`:700`、
`:1169` 的取消；`drivers/video/mxc/mxc_epdc_fb.c:2273-2274`、
`:2586-2587`、`:3448-3449` 的 ambient 更新直接使用缓存温度求档。

`test-papyrus-power.py` 编译实际 EPDC 温度 accessor 与 Papyrus
驱动，在 499 个电源/故障/缓存场景中验证缓存命中零 I²C、无缓存
同步转换、60 秒重排队、挂起取消及恢复立即采样。该覆盖与下面的
EPDC 主机测试及 ARM 构建均不证明真实设备逐帧延迟或冷启动行为。

## 验证范围

主机 image 套件比较直接复制与强制 PxP 路径的完整目标字节及 update
矩形，覆盖多种区域、所有宽高对齐余数、不对齐 left/width、完整画布、
行间空隙和反相异或抵消，并检查快速路径零分配、零 PxP 调用。
waveform 套件用合成 WBF 检查不同温度、多帧 A2、延迟首个非零动作、
不同动作值、单帧 DIRECT、其它模式内容一致、无 A2 和全零序列，
同时覆盖外部 WRF。pipeline 套件检查 DIRECT 提交、拒绝、诊断计数、
异模式合并隔离及 DMA_DEBUG 两种配置；SNAPSHOT 覆盖提交后改写、
排队和碰撞重试、双向 scheme 切换、独立处理、三种格式、四种旋转、
自动刷新、分配失败及正常/故障/取消释放。fb 套件检查编号 7 的 ioctl
和三种 scheme。
主机套件使用 ASan/UBSan；ptrace 环境以 `ASAN_OPTIONS=detect_leaks=0`
关闭不支持的 LeakSanitizer，各套件仍检查自身的分配释放计数。

五个指定套件均 PASS：image 为 7008 图像，pipeline 为 DMA_DEBUG=0 的
48 场景及 DMA_DEBUG=1 的 49 场景，fb 为 17 场景，waveform 为
75 场景及 16000 温度输入，Papyrus 为 499 场景。ARM 内核与静态用户态工具交叉构建通过，
内核输出为 zImage 和板级 DTB。设备树使用
`imx50-kindle-k4.dtb`，显示时钟、电源、面板时序沿用当前板级配置。
真实设备显示效果、帧时序、处理延迟及冷启动属于实机验证范围；
本记录的验证是源码主机模型与交叉构建。
