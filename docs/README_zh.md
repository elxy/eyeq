# EyeQ

多视频主观质量对比工具，专注于无损或接近无损质量范围的主观质量评估。

相比于[video-compare](https://github.com/pixop/video-compare)，主要增加了：

1. 支持高位深、HDR10和Dolby Vision (Profile 5)
2. 支持ICC色彩管理，macOS下自动检测系统显示器ICC配置
3. 支持1-10个视频同时对比，随时切换参考视频，便于评估保真度
4. 三种显示模式：Fill(切换)、Slide(左右滑动)、Grid(网格)

## 安装

macOS和Linux下推荐通过Homebrew安装，Windows下推荐通过MSYS2使用源码安装，详见[INSTALL_zh.md](./INSTALL_zh.md)。

### Homebrew

```sh
brew tap elxy/eyeq
brew install eyeq
# macOS下需要对二进制签名：
codesign --force --deep --sign - /opt/homebrew/bin/eyeq
# 或者从源码编译：
brew install --build-from-source eyeq
```

### 预编译二进制

macOS、Linux和Windows的预编译二进制可以从[Releases](https://github.com/elxy/eyeq/releases)页面下载，解压后即可使用。

macOS下需要先移除隔离属性：

```sh
xattr -cr <解压目录>
```

## 使用说明

### 基本用法

- **切换对比**(Fill模式)

```sh
eyeq --display-mode fill <test_0> <test_1>
```

按0和1切换视频0和视频1。最多支持10个视频同时对比，对应数字键0-9。

- **左右滑动对比**(Slide模式)

```sh
eyeq --display-mode slide <test_a> <test_b>
```

左右移动鼠标滑动对比视频A和B。

- **网格模式同时对比多个视频**(Grid模式)

```sh
eyeq --display-mode grid <test_0> <test_1> <test_2> <test_3>
```

- **对比保真度**

```sh
eyeq --display-mode slide --ref <ref> <test_a> <test_b>
```

按`R`键切换显示参考视频（松手恢复）。

- **指定主视频**

其他视频（包括参考视频）的渲染分辨率、色彩空间等，都将对齐主视频：

```sh
eyeq --display-mode slide --ref <ref> <test_a> --main <test_b>
```

- **使用网格模式对比多个文件夹的图片**

```sh
eyeq --display-mode grid '<test_0>/*.jpg' '<test_1>/*.png' '<test_2>/*.jpg'
```

图片序列由 FFmpeg 的 [image2 demuxer](https://ffmpeg.org/ffmpeg-formats.html#image2-1) 处理，支持以下路径模式：
  - `'dir/*.jpg'` — glob 模式，匹配目录下所有 `.jpg` 文件（按文件名排序）。**必须加引号**，否则会被 shell 展开成多个参数，每个被当成独立视频源
  - `dir/%04d.jpg` — 序号模式，匹配 `0000.jpg`、`0001.jpg`、`0002.jpg`……

这些模式在 `fill` 和 `slide` 模式下同样适用，不限于 `grid` 模式。不支持直接传入裸目录路径（如 `./images/`）。

> **注意：** EyeQ 会探测所链接的 FFmpeg 实际支持哪种 `pattern_type` 并自动选择。旧语法 `dir/%*.jpg`（属于 FFmpeg 8.x 已移除的 `pattern_type=glob_sequence`）仍可接受，会被自动改写为普通 glob 并给出警告。若 FFmpeg 编译时未启用 glob 支持（`HAVE_GLOB`），EyeQ 会明确报告该原因，此时请改用序号模式。

### 滤镜

- **指定色彩空间**

```sh
eyeq <test_0> <test_1> --filter setparams=color_trc=bt709:color_primaries=bt709
```

通过`--filter`选项指定所有视频的滤镜，可处理色彩空间信息丢失的情况。

- **为不同视频指定不同滤镜**

```sh
eyeq <test_0>@"setparams=color_trc=bt709" <test_1>@"format=rgb48le"
```

在视频路径后使用`@`分隔符附加该视频的滤镜。未附加滤镜的视频使用`--filter`指定的默认滤镜。如果文件名包含`@`，可通过`--filter-sep`指定其他分隔符：

```sh
eyeq --filter-sep '#' user@host.mp4#"filter1" test.mp4#"filter2"
```

### 硬件解码

通过`--hardware-decoder`选项开启硬件解码加速：

```sh
# 自动选择可用的硬件解码器
eyeq --hardware-decoder auto <test_0> <test_1>

# 指定macOS VideoToolbox
eyeq --hardware-decoder videotoolbox <test_0> <test_1>

# 指定Linux VAAPI
eyeq --hardware-decoder vaapi <test_0> <test_1>
```

支持的硬件解码器：`auto`(默认)、`none`、`videotoolbox`(macOS)、`vaapi`(Linux)、`cuda`(NVIDIA)、`d3d12va` (Windows)、`d3d11va` (Windows)、`dxva2` (Windows)。

### ICC色彩管理

通过`--icc-profile`选项启用ICC色彩管理：

```sh
# macOS自动检测系统显示器ICC配置
eyeq --icc-profile auto <test_0> <test_1>

# 使用自定义ICC文件
eyeq --icc-profile /path/to/display.icc <test_0> <test_1>

# 为特定视频指定ICC
eyeq --icc-profile 0:display.icc <test_0> <test_1>
```

### 操作方法

启动后，按键操作如下：

- 0-9：切换对应视频，视频序号从0开始（松手时触发）
- R：切换参考视频，松手恢复
- T：交换左右视频，松手恢复
- Space：暂停/播放
- →：前进一秒
- ←：回退一秒
- ↓：前进五秒
- ↑：回退五秒
- ] / Page Down：前进一分钟
- [ / Page Up：回退一分钟
- Shift + ] / End：前进10000秒
- Shift + [ / Home：回退10000秒
- A：回退一帧
- D：前进一帧
- Ctrl + S：保存视频当前帧，保存在目录`<帧序号>.<视频ID>.<文件名>.png`
- Ctrl + E：保存渲染后的帧（色调映射后的最终显示画面），保存在目录`render_<帧序号>.<视频ID>.<文件名>.dpx`
- 鼠标滚轮：缩放视频（以鼠标位置为中心）
- 按住鼠标中键拖动：平移视频
- I：显示或隐藏OSD
- Shift + S：seek到鼠标所处位置的相对时间
- Z：恢复默认缩放和位置（贴紧窗口）
- X：强制刷新，遇到窗口缩放黑屏时多按几次可恢复
- F：切换全屏
- F5：重置所有视频偏移（恢复与主视频帧对齐）
- Esc：退出全屏（窗口模式下再按一次才退出程序）
- Q：退出

**单视频操作**（按住数字键N作为修饰键，再按操作键）：

- N + A：视频N回退一帧
- N + D：视频N前进一帧
- N + →/←：视频N前进/回退一秒
- N + ↓/↑：视频N前进/回退五秒
- N + ] / Page Down / [ / Page Up：视频N前进/回退一分钟
- N + F5：重置视频N偏移（恢复与主视频帧对齐）
- 可同时按住多个数字键，对多个视频同时操作
- 帧偏移在全局seek后保持不变

### 启动参数

- `<videos>`：指定对比视频，可以有多个。支持URL（Homebrew的ffmpeg库默认支持）
- `--ref`：指定参考视频
- `--main`：指定主视频。其他视频的分辨率、渲染的目标色彩空间都会对齐主视频。如果指定了参考视频，则主视频为参考视频；否则默认为第一个视频，除非指定`--main`
- `--display-mode <mode>`：指定显示模式，`fill`即单个窗口显示，按键切换，`slide`即左右滑动显示，`grid`即网格显示。若未指定，则根据视频数量自动选择，2个视频时采用左右滑动显示，其他情况采用单窗口显示
- `--flicker <secs>`：播放时，按照指定时间间隔交替显示参考视频和对比视频，单位秒
- `--amplify <ratio>`：缩放与参考视频的像素差别，> 1时放大，< 1时缩小
- `--window-size <size>`：指定窗口大小，如`--window-size 1920x1080`，`fill`和`slide`模式下默认使用主视频的分辨率，`grid`模式下自适应选择。与`--full-screen`互斥
- `--full-screen`：启动后直接进入全屏模式。使用桌面分辨率、不切换显示模式（无边框全屏），视频按比例缩放铺满屏幕。与`--window-size`互斥
- `--grid-size <size>`：指定`grid`显示时的网格大小，如`--grid-size 2x2`，默认自适应选择
- `--filter <filter>`：指定视频滤镜，作为未单独指定滤镜的视频的默认滤镜，即[FFmpeg Filters](https://ffmpeg.org/ffmpeg-filters.html)。也可以在视频路径后使用`@`分隔符为单个视频指定滤镜，如`video.mp4@"filter"`
- `--filter-sep <sep>`：指定per-video filter的分隔符，默认为`@`。当文件名包含`@`时，可使用该选项指定其他分隔符
- `--no-colorspace-hint`：不使用视频色彩空间渲染，而是使用默认的色彩空间，当显示设备不支持HDR时启用该选项以进行色调映射
- `--sdr-white-on-hdr <nits>`：当主视频为HDR时，设置SDR视频的参考白电平（单位nits）。默认按照标准参考白203 nits（ITU-R BT.2408）进行映射。该选项仅在主视频为HDR、对比视频为SDR时生效
- `--target-display-nits <nits>`：目标显示峰值亮度，单位 nits。传入后，把所有 HDR 内容（HLG、DPX 读回）的峰值（`max_luma`）覆盖为该值
- `--high-dpi {auto,yes,no}`：High-DPI模式，默认`auto`自动检测
- `--scale-method <method>`：视频放大方法，包括`nearest`、`bilinear`、`bicubic`、`lanczos`、`ewa_lanczos`、`ewa_lanczossharp`、`mitchell`、`catmull_rom`、`spline36`、`spline64`，默认为`nearest`
- `--plane-scale-method <method>`：色度插值方法，默认为`lanczos`
- `--seek-to <secs>`：播放起始时间，单位为秒
- `--seek-to-frame <N>`：从第 N 帧开始播放（从 0 开始计数）
- `--save-in-source`：设置后，`Ctrl + S` / `Ctrl + E` 保存的帧将输出到视频文件所在目录中
- `--save-format <format>`：帧保存格式，默认为`png`
- `--save-render-format <format>`：渲染帧保存格式，默认为`dpx`
- `--hardware-decoder {none,auto,videotoolbox,vaapi,cuda,d3d12va,d3d11va,dxva2}`：硬件解码器，默认`auto`
- `--icc-profile <profile>`：ICC色彩管理。`auto`使用系统配置(仅macOS)，也可指定ICC文件路径，或`N:path`为特定视频指定
- `--loglevel <level>`：日志级别，包括`debug`、`info`、`warning`、`error`、`critical`、`off`，默认为`info`
- `--debug`：等同于`--loglevel debug`

### 环境变量

- `EYEQ_FRAME_CACHE`：覆盖自动帧缓存大小。默认情况下，EyeQ根据所有视频的总分辨率自动调整帧缓存：总像素≤8K时为32帧，>8K时为16帧，>16K时为8帧。设置该环境变量为正整数可使用固定的缓存大小

## FAQ

1. 为什么视频在QuickTime和EyeQ中显示效果不同？

   主要原因是，QuickTime做了颜色管理，EyeQ没有（除非使用`--icc-profile`）。次要原因是，对于BT.709，macOS的EOTF与EyeQ不同。
   - 对于HDR10，需要切换显示器设置：系统设置 -> 显示器 -> 预置 -> "HDR Video (P3 ST 2084)"。
   - 对于BT.709，QuickTime会按照EOTF ≈ 1.961 gamma将R'G'B'解码为RGB，而EyeQ会按照BT.1886 (≈ 2.4 gamma)解码。所以QuickTime看起来会更亮一点。
      - QuickTime也可以支持BT.1886，需要修改mov文件的nclc标签，设置为1-2-1，详见[Quicktime Color Management: why so many ISSUES?!](https://www.youtube.com/watch?v=1QlnhlO6Gu8)

   次要原因包括：缩放算法（包括UV平面的缩放）、去块效应、去振铃、位深抖动，部分播放器甚至会做色彩增强。EyeQ相较于[libplacebo的默认渲染设置](https://code.videolan.org/videolan/libplacebo/-/blob/v7.351/src/renderer.c?ref_type=heads#L201)，去掉了位深抖动，同时允许调整缩放算法。

2. 为什么保存的png视频帧，在Preview和EyeQ中颜色不一致？

   EyeQ调用ffmpeg库保存视频帧为png。对于不含ICC profile且非sRGB的帧，会将color_primaries和color_trc写入cICP tag。

   对于HDR10，cICP tag中的color_trc为BT.2100 (PQ)，macOS会复用HDR的渲染流程，Preview和QuickTime的显示一致。

   对于BT.709，ffmpeg除了写入cICP tag，还会按照gamma ≈ 1.961写入gAMA tag。而cICP的优先级高于gAMA。会发现：
   - 对比Preview和Safari，Preview偏暗，而Safari偏亮。cICP tag是2023年12月才正式加入png-3标准，可能macOS对SDR的支持尚不完善？
   - 移除png的cICP和gAMA tag，Preview按照sRGB渲染，颜色与EyeQ只在暗部有一点点差别。原因是sRGB的非线性部分和BT.1886都是2.4 gamma，只在暗部能看出细微差别。
   - 移除png的cICP tag，Preview按照gamma = 1.961进行渲染，与QuickTime播放BT.709视频的颜色看不出差别。

3. 能否同时对比HDR和SDR视频？

   可以，但需要注意通过`--main`指定主视频。
   - 如果主视频是SDR，则渲染的目标色彩空间是SDR的，HDR视频会进行色调映射
   - 如果主视频是HDR10，则渲染的目标色彩空间是HDR10的，SDR视频按照参考白(203 nits)进行映射。可通过`--sdr-white-on-hdr`调整该亮度

4. 不兼容的像素格式（如提示`pl_map_avframe_ex() failed`）

   部分像素格式不被libplacebo的Vulkan后端支持——可能是格式的分量步长不兼容（如`y210le`），也可能是GPU没有对应的纹理格式（如`bgr8`）。EyeQ会自动追加`format=rgb48le`滤镜，通过FFmpeg进行格式转换。如果已手动指定了`format=`滤镜，EyeQ不会覆盖，此时请尝试`--filter format=rgb48le`。

5. 如何回看`Ctrl + E`保存的DPX渲染帧？

   DPX 头部用 SMPTE ST 268:2014 的枚举值记录 transfer/primaries（PQ=14、HLG=15、BT.2020=15），而 FFmpeg 的 DPX 解码器不认识这些值（transfer/colorimetric 只映射到 12/10），reference high quantity（HDR 峰值）也不会被读取。因此 EyeQ 拿到的是未知色彩空间、且没有 `max_luma`——需用 `setparams` 滤镜恢复色彩空间，并用 `--target-display-nits` 提供色调映射所需的峰值：

   ```
   eyeq --no-colorspace-hint --target-display-nits 1000 \
     'render_0001.0.foo.mkv.dpx@setparams=color_primaries=bt2020:color_trc=smpte2084'
   ```

   HLG 用 `color_trc=arib-std-b67`。在 HDR 显示器上 PQ 直通、无需色调映射，`--no-colorspace-hint` 与 `--target-display-nits` 可不加；`--no-colorspace-hint` 仅在非 HDR 显示器上需要，用于开启色调映射。

## 许可证

本软件基于 GNU 宽通用公共许可证 (LGPL) 2.1 或更高版本发布。详见 [LICENSE.md](../LICENSE.md)。
