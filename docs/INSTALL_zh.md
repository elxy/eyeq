# 编译开发

EyeQ的依赖项：

- [FFmpeg](https://ffmpeg.org/)：用于读取视频、保存截图
- [libplacebo](https://code.videolan.org/videolan/libplacebo)：用于画面渲染
- [SDL3](https://www.libsdl.org/)：用于窗口管理、用户交互，SDL3_ttf用于OSD显示
- [Vulkan](https://www.vulkan.org/)：渲染后端
- [spdlog](https://github.com/gabime/spdlog)：用于日志
- [Desktop Video SDK](http://www.blackmagicdesign.com/support/family/capture-and-playback)：通过Blackmagic Design的DeckLink设备接入监视器。

MacOS和Linux下建议通过Homebrew编译，Windows下建议通过MSYS2编译。

## Homebrew下编译

```sh
# 安装依赖
brew tap elxy/eyeq
brew install --only-dependencies eyeq # 自动安装依赖项

# 下载源码
git clone https://github.com/elxy/eyeq.git
cd eyeq

# 编译
export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig"
export CMAKE_PREFIX_PATH="$(brew --prefix)"
cmake -B build -S .
cmake --build build
```

## MSYS2下编译

需要先安装[msys2](https://www.msys2.org/#installation)。这里以`ucrt64`环境：

```sh
# 安装依赖
pacman -S mingw-w64-ucrt-x86_64-toolchain base-devel
pacman -S mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
pacman -S mingw-w64-ucrt-x86_64-spdlog
pacman -S mingw-w64-ucrt-x86_64-vulkan-headers
pacman -S mingw-w64-ucrt-x86_64-libplacebo
pacman -S mingw-w64-ucrt-x86_64-sdl3
pacman -S mingw-w64-ucrt-x86_64-sdl3-ttf
pacman -S mingw-w64-ucrt-x86_64-ffmpeg

# 下载源码
git clone https://github.com/elxy/eyeq.git
cd eyeq

# 编译
cmake -S . -B build
cmake --build build
```

### DeckLink支持

手动下载Desktop Video SDK，安装到`/path/to/Blackmagic DeckLink SDK 16.0`。

```sh
cmake -S . -B build -DEYEQ_ENABLE_DECKLINK=ON \
  -DDECKLINK_SDK_ROOT="/path/to/Blackmagic DeckLink SDK 16.0"
```

使用设备/模式查询工具：

```sh
cmake --build build --target decklink-catalog
# 列出设备：
./build-sdk/bin/decklink-catalog
# 只读查询某设备的输出模式。选择器必须显式写成index:<n>或id:<persistent-id>
./build-sdk/bin/decklink-catalog --device index:0 --connection sdi --pixel-format v210
./build-sdk/bin/decklink-catalog --device id:2697367084
```
