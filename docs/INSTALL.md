# Building from Source

## Dependencies

- [FFmpeg](https://ffmpeg.org/) — video decoding and frame saving
- [libplacebo](https://code.videolan.org/videolan/libplacebo) — video rendering
- [SDL3](https://www.libsdl.org/) — window management and user interaction; SDL3_ttf for OSD text rendering
- [Vulkan](https://www.vulkan.org/) — rendering backend
- [spdlog](https://github.com/gabime/spdlog) — logging
- [Desktop Video SDK](http://www.blackmagicdesign.com/support/family/capture-and-playback) — monitoring via a Blackmagic Design DeckLink device

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

### DeckLink Support

Manually download the Desktop Video SDK and install it to `/path/to/Blackmagic DeckLink SDK 16.0`.

```sh
cmake -S . -B build -DEYEQ_ENABLE_DECKLINK=ON \
  -DDECKLINK_SDK_ROOT="/path/to/Blackmagic DeckLink SDK 16.0"
```

Using the device/mode query tool:

```sh
cmake --build build --target decklink-catalog
# List devices:
./build-sdk/bin/decklink-catalog
# Read-only query of a device's output modes. The selector must be written explicitly as index:<n> or id:<persistent-id>
./build-sdk/bin/decklink-catalog --device index:0 --connection sdi --pixel-format v210
./build-sdk/bin/decklink-catalog --device id:2697367084
```
