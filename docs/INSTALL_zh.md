# 编译开发

EyeQ的依赖项：

- [FFmpeg](https://ffmpeg.org/)：用于读取视频、保存截图
- [libplacebo](https://code.videolan.org/videolan/libplacebo)：用于画面渲染
- [SDL3](https://www.libsdl.org/)：用于窗口管理、用户交互，SDL3_ttf用于OSD显示
- [Vulkan](https://www.vulkan.org/)：渲染后端
- [spdlog](https://github.com/gabime/spdlog)：用于日志
- [libdovi](https://github.com/quietvoid/dovi_tool/tree/main/dolby_vision)：用于解析杜比视界元数据，可选

MacOS和Linux下建议通过Homebrew编译，Windows下建议通过MSYS2编译。

## Homebrew下编译

```sh
# 安装依赖
brew tap ernestcao/homebrew-eyeq https://git.woa.com/ernestcao/homebrew-eyeq
brew install --only-dependencies eyeq # 自动安装依赖项，brew上的libplacebo二进制不带libdovi支持
brew install --only-dependencies eyeq --with-dovi # 会编译带libdovi支持的libplacebo

# 下载源码
git clone https://git.woa.com/ernestcao/eyeq.git
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
pacman -S mingw-w64-ucrt-x86_64-libplacebo # 这里的libplacebo自带libdovi支持
pacman -S mingw-w64-ucrt-x86_64-sdl3
pacman -S mingw-w64-ucrt-x86_64-sdl3-ttf
pacman -S mingw-w64-ucrt-x86_64-ffmpeg

# 下载源码
git clone https://git.woa.com/ernestcao/eyeq
cd eyeq

# 编译
cmake -S . -B build
cmake --build build
```

