# Building from Source

## Dependencies

- [FFmpeg](https://ffmpeg.org/) — video decoding and frame saving
- [libplacebo](https://code.videolan.org/videolan/libplacebo) — video rendering
- [SDL3](https://www.libsdl.org/) — window management and user interaction; SDL3_ttf for OSD text rendering
- [Vulkan](https://www.vulkan.org/) — rendering backend
- [spdlog](https://github.com/gabime/spdlog) — logging

Building via Homebrew is recommended on macOS and Linux. On Windows, building via MSYS2 is recommended.

## Building with Homebrew (macOS / Linux)

```sh
# Install dependencies
brew tap elxy/eyeq
brew install --only-dependencies eyeq

# Clone the source
git clone https://github.com/elxy/eyeq.git
cd eyeq

# Build
export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig"
export CMAKE_PREFIX_PATH="$(brew --prefix)"
cmake -B build -S .
cmake --build build
```

## Building with MSYS2 (Windows)

First install [MSYS2](https://www.msys2.org/#installation), then use the `ucrt64` environment:

```sh
# Install dependencies
pacman -S mingw-w64-ucrt-x86_64-toolchain base-devel
pacman -S mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
pacman -S mingw-w64-ucrt-x86_64-spdlog
pacman -S mingw-w64-ucrt-x86_64-vulkan-headers
pacman -S mingw-w64-ucrt-x86_64-libplacebo
pacman -S mingw-w64-ucrt-x86_64-sdl3
pacman -S mingw-w64-ucrt-x86_64-sdl3-ttf
pacman -S mingw-w64-ucrt-x86_64-ffmpeg

# Clone the source
git clone https://github.com/elxy/eyeq.git
cd eyeq

# Build
cmake -S . -B build
cmake --build build
```
