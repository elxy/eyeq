# EyeQ 测试

[English](./README.md)

所有自动化测试都位于 `tests/`，按被测模块分组，并通过
[tests/CMakeLists.txt](./CMakeLists.txt) 接入 CMake。测试目标是可选的：下列开关默认
全部为 `OFF`，不影响普通播放器构建。每个测试集会说明自己**验证什么、不验证什么**；
没有任何测试集验证真实 DeckLink 设备、窗口 present 路径或桌面视觉输出。

## CMake 开关

| 开关 | 默认 | 用途 |
|---|---|---|
| `EYEQ_BUILD_DECKLINK_TESTS` | `OFF` | 构建 DeckLink CPU/fake 契约测试；配合 `EYEQ_ENABLE_DECKLINK` 还会构建确定性的 SDK 故障注入与输出后端测试。无需 GPU、设备 |
| `EYEQ_BUILD_DECKLINK_GPU_TESTS` | `OFF` | 构建 GPU 打包测试：CPU oracle、真实 GPU 对比/读回测试、生命周期故障注入测试。运行时需要 libplacebo 与可用的 Vulkan 设备；不需要 SDK，也不需要 DeckLink 设备 |
| `EYEQ_BUILD_DISPLAY_GPU_TESTS` | `OFF` | 构建离屏显示/渲染测试与监看组合层测试：在无窗口的 headless Vulkan 设备上运行真实 `RenderContext`/`FrameRenderer`/`MonitorOutput`，覆盖 Fill/Slide/Grid、双实例交错、HDR override、ICC 隔离、OSD 开关与 paced 监看路径。运行时需要 libplacebo 与可用的 Vulkan 设备；不需要 SDK、窗口或 DeckLink 设备 |
| `EYEQ_BUILD_PLAYER_TESTS` | `OFF` | 构建纯 CPU 播放器测试：用本地合成的 YUV4MPEG2 文件驱动真实 `SyncPlayer`/`VideoSource`，覆盖帧快照发布、控制完成顺序、相对 seek 累积、多源 EOF 对齐、有界命令预算、Stop 取消与 seek 失败传播。无需 GPU、窗口、SDK 或 DeckLink 设备 |

`EYEQ_ENABLE_DECKLINK` 与 `DECKLINK_SDK_ROOT` 是后端构建开关而非测试开关，详见顶层
[README](../docs/README_zh.md)。二者与 `EYEQ_BUILD_DECKLINK_TESTS` 同时开启时，还会构建
下面提到的基于 SDK 头文件的后端测试。

## DeckLink CPU/fake 契约测试（无需 SDK、GPU、设备）

```sh
cmake -S . -B build -DEYEQ_BUILD_DECKLINK_TESTS=ON
cmake --build build --target decklink_contract_test
ctest --test-dir build --output-on-failure
```

该测试集覆盖纯 CPU 接缝：输出契约（`decklink_contract`）、与 `decklink-catalog`
共用的严格设备选择器解析（`decklink_device_selector`）、严格的监看 CLI
解析/校验（`decklink_monitor_cli`）、确定性的测试信号画面
（`decklink_test_signal_pattern`）以及独立的 r210/v210 已知答案参考
（`decklink_frame_packer_oracle`）。

若同时开启 `EYEQ_ENABLE_DECKLINK=ON`，还会加入 `tests/decklink/backend` 与
`tests/decklink/packer` 下的基于 SDK 头文件的后端测试：catalog 与 monitor mode
枚举、故障注入的 catalog 路径、`decklink-catalog` CLI 回归，以及真实输出状态机及其
并发/终止测试、测试信号 session/CLI 测试——全部跑在 fake SDK dispatch 上，绝不接触
设备或 GPU。

## 播放器帧/控制测试（无需 GPU、窗口、SDK）

```sh
cmake -S . -B build-player -DEYEQ_BUILD_PLAYER_TESTS=ON
cmake --build build-player --target player_sync_test
ctest --test-dir build-player -R player_sync --output-on-failure
```

`player_sync_test` 用本地合成的 YUV4MPEG2 文件驱动真实
`SyncPlayer`/`VideoSource`，覆盖帧快照发布、控制完成顺序、相对 seek 累积、多源 EOF
对齐、有界命令预算、Stop 取消与 seek 失败传播。

## GPU 打包测试（无需 SDK、DeckLink 设备）

需要真实 Vulkan GPU；无可用/不合适的 GPU 时测试直接失败而不是跳过。

```sh
cmake -S . -B build-gpu -DEYEQ_BUILD_DECKLINK_GPU_TESTS=ON
cmake --build build-gpu
ctest --test-dir build-gpu --output-on-failure
```

- `frame_packer_gpu`：在格式边界附近的多组宽度、额外 stride、两种精度与编码、有限值
  clamp、灰阶/码值锚点、色度相位脉冲上把真实 GPU 读回与参考逐字节对比，并包含 UHD
  全帧校验。
- `frame_packer_fault`：通过只存在于该测试目标中的测试钩子注入传输、读回、GPU 失败与
  分配失败。

## CPU 已知答案与比较器测试（无需 GPU）

纯 CPU 的已知答案与比较器突变测试不需要 GPU：

```sh
cmake -S . -B build-cpu -DEYEQ_BUILD_DECKLINK_TESTS=ON
cmake --build build-cpu
ctest --test-dir build-cpu --output-on-failure
```

- `decklink_frame_packer_oracle`（纯 CPU）：用手工推导的已知答案（原色/互补色、色度
  滤波权重与相位、补齐字节）校验独立参考实现，并用故意破坏的字节校验比较器会拒绝错误。

## 离屏显示/渲染测试

`RenderContext` 拥有共享的 GPU 生命周期（log、Vulkan instance/device、FFmpeg Vulkan
hw_device 与 queue-lock 回调），`FrameRenderer` 拥有每个目标独立的渲染资源，因此可以在
同一个 context 上渲染多个目标。离屏测试不需要窗口，也不需要 DeckLink SDK：

```sh
cmake -S . -B build-display -DEYEQ_BUILD_DISPLAY_GPU_TESTS=ON
cmake --build build-display --target display_renderer_gpu_test
ctest --test-dir build-display -R display_renderer_gpu --output-on-failure
```

测试把 Fill/Slide/Grid（含参考/差异放大）渲染到不同的 full-range RGB 浮点目标，并对像素与
几何做可失败断言；在同一 GPU 上把两个不同尺寸、不同色彩目标的 renderer 交错调用，检查一个
renderer 的输出不受另一个影响、且另一个被销毁后仍可继续；还覆盖失败帧更新（有效→无效→有效）、
目标局部 HDR nits override、ICC 隔离与 OSD 开关。它不验证窗口 present 路径、桌面视觉输出或任何
硬件映射，这些仍属于人工/环境受限项。

## 监看组合层测试

组合测试在无窗口的 headless Vulkan 设备上运行真实组合层，配合可真正返回 Still
Busy/DeviceError/Closed 的 scripted fake sink：

```sh
cmake -S . -B build-monitor -DEYEQ_BUILD_DISPLAY_GPU_TESTS=ON
cmake --build build-monitor --target monitor_output_gpu_test monitor_output_fault_test
ctest --test-dir build-monitor -R monitor_output --output-on-failure
```

覆盖 SDR/PQ 与 r210/v210（含非紧凑 `row_bytes` 补齐）、已知黑场/色块/PQ 100nit 锚点、CPU
打包 oracle、Fill/Slide/Grid/差异放大、Busy/取消/关闭与终态 sink 故障，
以及 paced 播放（合成顺序、显示节拍严格递增、逐帧字节对 CPU oracle、队列满顶旧、暂停/恢复、
恢复时回收冻结静帧的 packer 槽位），再加上一次注入真实读回失败的故障测试。
`monitor_output_fault_test` 链接的是带故障注入 hook 的真实 `frame_packer.cpp`，确保注入确实
命中。这些测试不验证窗口、真实硬件帧或 DeckLink 设备。

## DeckLink 测试信号发生器

`decklink-test-signal` 生成确定性的 CPU 测试画面，并通过同一输出后端发送，不经 `eyeq` 播放器、不依赖 GPU 打包器。它由 `EYEQ_ENABLE_DECKLINK=ON` 构建（不是测试开关），用于实机验证 SDI/HDMI 链路、模式时基、range 与运动顺序，运行时**会启用真实硬件输出**。

```sh
cmake -S . -B build-sdk -DEYEQ_ENABLE_DECKLINK=ON \
  -DDECKLINK_SDK_ROOT="/path/to/Blackmagic DeckLink SDK 16.0"
cmake --build build-sdk --target decklink-test-signal
# 按模式的准确有理帧率输出 10 秒，SDI、v210、SDR：
./build-sdk/bin/decklink-test-signal --device index:0 --connection sdi \
  --pixel-format v210 --mode auto --seconds 10
```

选项：`--device index:<n>|id:<id>`（必填，选择器与 `decklink-catalog` 相同）、`--connection sdi|hdmi`、`--pixel-format r210|v210`、`--mode WxH@fps|auto`（auto 优先 3840x2160 50p；显式模式必须存在于设备原生列表）、`--hdr`（BT2020/PQ）、`--frames N` 与 `--seconds S`（互斥，默认 5 秒）、`--preroll N`（默认 4，上限为帧数；若设备帧池先满则提前开始播放，不等 Start 前不可能出现的完成事件）、`--drain-timeout-ms N`（默认 5000，是无进展超时而非整次运行死线）、`--still`（显示一次静帧，不 Start、不调度）与 `-h|--help`。退出码：0 成功，1 用法/请求/运行错误，2 DeckLink 驱动不可用。

启用输出前，工具会在选定设备上只读查询 `bmdDeckLinkStatusBusy`；若设备报告忙、`IDeckLinkStatus` 缺失或查询失败则拒绝，且不会用 Enable/Disable 试探，也不会终止/抢占 DaVinci Resolve 等其他应用。该检查是瞬时快照而非设备锁，不能保证绝对排除并发占用。Ctrl-C/SIGTERM 只设置标志，由消费线程观察：停止提交新帧，随后关闭输出并消费已保留事件再退出。

## 硬件验证状态

自动化验证目前只在 fake SDK 上跑通原生 DeckLink 写路径，打包器仅在本机 GPU 上验证，
paced 监看路径则用 scripted fake sink 验证；这些是验证状态，不是运行行为限制。首轮计划
先跑 SDI；HDMI 与 PQ 已接线但未经实机验收。`decklink-test-signal` 运行时仍会启用真实硬件
输出，其用法与选项见顶层 [README](../docs/README_zh.md)。

## 本机 UHD 测量

本机 UHD 四槽短测是离屏数值，不代表端到端 4K50：摊销约 1.0-1.1 ms/帧，submit-to-Ready
`p50` 约 3-4 ms，帧字节约 33 MB（r210）或 22 MB（v210）。测试会打印当前数值，并分别
报告映射主机缓冲与逻辑载体 payload，不声称测量驱动物理驻留。
