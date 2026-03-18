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
brew tap ernestcao/homebrew-eyeq https://git.woa.com/ernestcao/homebrew-eyeq
brew install eyeq
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
eyeq --display-mode grid <test_0>/%*.jpg <test_1>/%*.png <test_2>/%*.jpg
```

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

支持的硬件解码器：`none`(默认)、`auto`、`videotoolbox`(macOS)、`vaapi`(Linux)、`cuda`(NVIDIA)、`d3d12va` (Windows)、`d3d11va` (Windows)、`dxva2` (Windows)。

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

- 0-9：切换对应视频，视频序号从0开始
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
- 鼠标滚轮：缩放视频（以鼠标位置为中心）
- 按住鼠标中键拖动：平移视频
- I：显示或隐藏OSD
- Shift + S：seek到鼠标所处位置的相对时间
- Z：恢复默认缩放和位置（贴紧窗口）
- X：强制刷新，遇到窗口缩放黑屏时多按几次可恢复
- Q / Esc：退出

### 启动参数

- `<videos>`：指定对比视频，可以有多个。支持URL（Homebrew的ffmpeg库默认支持）
- `--ref`：指定参考视频
- `--main`：指定主视频。其他视频的分辨率、渲染的目标色彩空间都会对齐主视频。如果指定了参考视频，则主视频为参考视频；否则默认为第一个视频，除非指定`--main`
- `--display-mode <mode>`：指定显示模式，`fill`即单个窗口显示，按键切换，`slide`即左右滑动显示，`grid`即网格显示。若未指定，则根据视频数量自动选择，2个视频时采用左右滑动显示，其他情况采用单窗口显示
- `--flicker <secs>`：播放时，按照指定时间间隔交替显示参考视频和对比视频，单位秒
- `--amplify <ratio>`：缩放与参考视频的像素差别，> 1时放大，< 1时缩小
- `--window-size <size>`：指定窗口大小，如`--window-size 1920x1080`，`fill`和`slide`模式下默认使用主视频的分辨率，`grid`模式下自适应选择
- `--grid-size <size>`：指定`grid`显示时的网格大小，如`--grid-size 2x2`，默认自适应选择
- `--filter <filter>`：指定视频滤镜，作为未单独指定滤镜的视频的默认滤镜，即[FFmpeg Filters](https://ffmpeg.org/ffmpeg-filters.html)。也可以在视频路径后使用`@`分隔符为单个视频指定滤镜，如`video.mp4@"filter"`
- `--filter-sep <sep>`：指定per-video filter的分隔符，默认为`@`。当文件名包含`@`时，可使用该选项指定其他分隔符
- `--no-colorspace-hint`：不使用视频色彩空间渲染，而是使用默认的色彩空间，当显示设备不支持HDR时启用该选项以进行色调映射
- `--high-dpi {auto,yes,no}`：High-DPI模式，默认`auto`自动检测
- `--scale-method <method>`：视频放大方法，包括`nearest`、`bilinear`、`bicubic`、`lanczos`、`ewa_lanczos`、`ewa_lanczossharp`、`mitchell`、`catmull_rom`、`spline36`、`spline64`，默认为`nearest`
- `--plane-scale-method <method>`：色度插值方法，默认为`lanczos`
- `--seek-to <secs>`：播放起始时间，单位为秒
- `--seek-frames <frames>`：播放起始帧数，当帧数较大时速度会比较慢
- `--frame-cache <frames>`：帧缓存数量，默认为16。如果帧缓存大小小于I帧间隔，则按`A`回退时可能会跳跃到上一个I帧
- `--save-in-source`：设置后，`Ctrl + S`将保存视频当前帧到视频文件所在目录中
- `--save-format <format>`：帧保存格式，默认为`png`
- `--hardware-decoder {none,auto,videotoolbox,vaapi,cuda,d3d12va,d3d11va,dxva2}`：硬件解码器，默认`none`
- `--icc-profile <profile>`：ICC色彩管理。`auto`使用系统配置(仅macOS)，也可指定ICC文件路径，或`N:path`为特定视频指定
- `--loglevel <level>`：日志级别，包括`debug`、`info`、`warning`、`error`、`critical`、`off`，默认为`info`
- `--debug`：等同于`--loglevel debug`

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
   - 如果主视频是HDR10，则渲染的目标色彩空间是HDR10的，SDR视频按照参考白(203 nits)进行映射

4. 错误提示：`Failed picking any compatible texture format for a plane!`

   说明libplacebo不支持当前pixel format。EyeQ会自动追加`format=rgb48le`滤镜，通过FFmpeg进行格式转换。如果已手动指定了`format=`滤镜，EyeQ不会覆盖，此时请尝试`--filter format=rgb48le`。
