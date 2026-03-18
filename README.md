# EyeQ

[中文介绍点我](./docs/README_zh.md)

A multi-video subjective quality comparison tool, focused on visual quality assessment in the lossless or near-lossless quality range.

Compared to [video-compare](https://github.com/pixop/video-compare), EyeQ adds:

1. High bit-depth, HDR10, and Dolby Vision (Profile 5) support
2. ICC color management with automatic system display ICC profile detection on macOS
3. Simultaneous comparison of 1–10 videos with on-the-fly reference switching for fidelity evaluation
4. Three display modes: Fill (switch), Slide (side-by-side swipe), and Grid

## Installation

On macOS and Linux, installing via Homebrew is recommended. On Windows, building from source under MSYS2 is recommended. See [INSTALL.md](./docs/INSTALL.md) for details.

### Homebrew

```sh
brew tap elxy/eyeq
brew install eyeq
# If you are on macOS, you need to sign the binary:
codesign --force --deep --sign - /opt/homebrew/bin/eyeq
# Or build from source:
brew install --build-from-source eyeq
```

### Pre-built Binaries

Pre-built binaries for macOS, Linux, and Windows are available on the [Releases](https://github.com/elxy/eyeq/releases) page. Download the archive for your platform and extract it.

On macOS, you need to remove the quarantine attribute before running:

```sh
xattr -cr <extracted_directory>
```

## Usage

### Basic Usage

- **Toggle comparison** (Fill mode)

```sh
eyeq --display-mode fill <test_0> <test_1>
```

Press 0 and 1 to switch between video 0 and video 1. Up to 10 videos can be compared simultaneously, mapped to number keys 0–9.

- **Side-by-side swipe comparison** (Slide mode)

```sh
eyeq --display-mode slide <test_a> <test_b>
```

Move the mouse left and right to swipe between video A and B.

- **Multi-video grid comparison** (Grid mode)

```sh
eyeq --display-mode grid <test_0> <test_1> <test_2> <test_3>
```

- **Fidelity comparison**

```sh
eyeq --display-mode slide --ref <ref> <test_a> <test_b>
```

Press and hold `R` to show the reference video (releases back to test video).

- **Specifying the main video**

Other videos (including the reference) will match the main video's rendering resolution, color space, etc.:

```sh
eyeq --display-mode slide --ref <ref> <test_a> --main <test_b>
```

- **Grid mode for comparing images across multiple directories**

```sh
eyeq --display-mode grid <test_0>/%*.jpg <test_1>/%*.png <test_2>/%*.jpg
```

### Filters

- **Specifying color space**

```sh
eyeq <test_0> <test_1> --filter setparams=color_trc=bt709:color_primaries=bt709
```

Use `--filter` to apply a filter to all videos, useful for handling missing color space metadata.

- **Per-video filters**

```sh
eyeq <test_0>@"setparams=color_trc=bt709" <test_1>@"format=rgb48le"
```

Append a filter after the video path using the `@` separator. Videos without an appended filter use the default filter specified by `--filter`. If the filename contains `@`, use `--filter-sep` to specify a different separator:

```sh
eyeq --filter-sep '#' user@host.mp4#"filter1" test.mp4#"filter2"
```

### Hardware Decoding

Enable hardware-accelerated decoding with the `--hardware-decoder` option:

```sh
# Automatically select an available hardware decoder
eyeq --hardware-decoder auto <test_0> <test_1>

# Use macOS VideoToolbox
eyeq --hardware-decoder videotoolbox <test_0> <test_1>

# Use Linux VAAPI
eyeq --hardware-decoder vaapi <test_0> <test_1>
```

Supported hardware decoders: `none` (default), `auto`, `videotoolbox` (macOS), `vaapi` (Linux), `cuda` (NVIDIA), `d3d12va` (Windows), `d3d11va` (Windows), `dxva2` (Windows).

### ICC Color Management

Enable ICC color management with the `--icc-profile` option:

```sh
# macOS: automatically detect system display ICC profile
eyeq --icc-profile auto <test_0> <test_1>

# Use a custom ICC file
eyeq --icc-profile /path/to/display.icc <test_0> <test_1>

# Specify ICC for a specific video
eyeq --icc-profile 0:display.icc <test_0> <test_1>
```

### Controls

After launching, the following keyboard and mouse controls are available:

- 0–9: Switch to the corresponding video (video indices start from 0)
- R: Show the reference video (hold; releases back on key up)
- T: Swap left/right videos (hold; releases back on key up)
- Space: Pause / Play
- →: Seek forward 1 second
- ←: Seek backward 1 second
- ↓: Seek forward 5 seconds
- ↑: Seek backward 5 seconds
- ] / Page Down: Seek forward 1 minute
- [ / Page Up: Seek backward 1 minute
- Shift + ] / End: Seek forward 10000 seconds
- Shift + [ / Home: Seek backward 10000 seconds
- A: Step backward 1 frame
- D: Step forward 1 frame
- Ctrl + S: Save the current frame as `<frame_number>.<video_id>.<filename>.png`
- Mouse wheel: Zoom in/out (centered on the cursor position)
- Middle mouse button drag: Pan the video
- I: Toggle OSD display
- Shift + S: Seek to the relative time at the cursor position
- Z: Reset zoom and position (fit to window)
- X: Force refresh (press multiple times if the window goes black after resizing)
- Q / Esc: Quit

### Command-Line Options

- `<videos>`: Specify videos to compare (multiple allowed). URLs are supported (FFmpeg built via Homebrew supports network protocols by default)
- `--ref`: Specify a reference video
- `--main`: Specify the main video. Other videos' rendering resolution and target color space will match the main video. If a reference video is specified, it becomes the main video by default; otherwise, the first video is used unless `--main` is set
- `--display-mode <mode>`: Display mode — `fill` (single window with key switching), `slide` (side-by-side swipe), `grid` (grid layout). If not specified, the mode is chosen automatically: `slide` for 2 videos, `fill` otherwise
- `--flicker <secs>`: During playback, alternate between the reference video and the test video at the specified interval (in seconds)
- `--amplify <ratio>`: Scale pixel differences relative to the reference video; > 1 amplifies, < 1 attenuates
- `--window-size <size>`: Window size, e.g. `--window-size 1920x1080`. In `fill` and `slide` modes, defaults to the main video's resolution; in `grid` mode, chosen automatically
- `--grid-size <size>`: Grid layout size, e.g. `--grid-size 2x2`. Defaults to automatic selection
- `--filter <filter>`: Default video filter applied to videos without a per-video filter, using [FFmpeg Filters](https://ffmpeg.org/ffmpeg-filters.html) syntax. Per-video filters can be appended after the video path with `@`, e.g. `video.mp4@"filter"`
- `--filter-sep <sep>`: Separator for per-video filters (default `@`). Use this when filenames contain `@`
- `--no-colorspace-hint`: Disable video color space hinting; uses the default color space instead. Enable this for tone mapping when the display does not support HDR
- `--high-dpi {auto,yes,no}`: High-DPI mode (default `auto`)
- `--scale-method <method>`: Upscaling method — `nearest`, `bilinear`, `bicubic`, `lanczos`, `ewa_lanczos`, `ewa_lanczossharp`, `mitchell`, `catmull_rom`, `spline36`, `spline64` (default `nearest`)
- `--plane-scale-method <method>`: Chroma interpolation method (default `lanczos`)
- `--seek-to <secs>`: Start playback at the specified time (in seconds)
- `--seek-frames <frames>`: Start playback at the specified frame number (may be slow for large values)
- `--frame-cache <frames>`: Frame cache size (default 16). If the cache size is smaller than the I-frame interval, stepping backward with `A` may jump to the previous I-frame
- `--save-in-source`: Save frames (via `Ctrl + S`) to the source video's directory instead of the current working directory
- `--save-format <format>`: Frame save format (default `png`)
- `--hardware-decoder {none,auto,videotoolbox,vaapi,cuda,d3d12va,d3d11va,dxva2}`: Hardware decoder (default `none`)
- `--icc-profile <profile>`: ICC color management — `auto` uses the system profile (macOS only); a file path specifies a custom ICC profile; `N:path` specifies a profile for a specific video
- `--loglevel <level>`: Log level — `debug`, `info`, `warning`, `error`, `critical`, `off` (default `info`)
- `--debug`: Equivalent to `--loglevel debug`

## FAQ

1. **Why does a video look different in QuickTime and EyeQ?**

   The primary reason is that QuickTime applies color management, while EyeQ does not (unless `--icc-profile` is used). A secondary reason is that macOS uses a different EOTF for BT.709 than EyeQ.
   - For HDR10, you need to change the display settings: System Settings → Displays → Presets → "HDR Video (P3 ST 2084)".
   - For BT.709, QuickTime decodes R'G'B' to RGB using EOTF ≈ 1.961 gamma, while EyeQ uses BT.1886 (≈ 2.4 gamma). As a result, QuickTime appears slightly brighter.
     - QuickTime can also use BT.1886 if the MOV file's nclc tag is set to 1-2-1. See [Quicktime Color Management: why so many ISSUES?!](https://www.youtube.com/watch?v=1QlnhlO6Gu8)

   Other contributing factors include: scaling algorithms (including chroma plane scaling), deblocking, deringing, bit-depth dithering, and some players even apply color enhancement. Compared to [libplacebo's default rendering settings](https://code.videolan.org/videolan/libplacebo/-/blob/v7.351/src/renderer.c?ref_type=heads#L201), EyeQ disables bit-depth dithering and allows customizing the scaling algorithm.

2. **Why does a saved PNG frame look different in Preview and EyeQ?**

   EyeQ uses the FFmpeg library to save video frames as PNG. For frames without an ICC profile and not in sRGB, the color_primaries and color_trc are written into the cICP tag.

   For HDR10, the cICP tag's color_trc is BT.2100 (PQ), and macOS reuses the HDR rendering pipeline, so Preview and QuickTime display consistently.

   For BT.709, FFmpeg writes both a cICP tag and a gAMA tag (gamma ≈ 1.961). Since cICP takes priority over gAMA:
   - Comparing Preview and Safari, Preview appears darker while Safari appears brighter. The cICP tag was only officially added to the PNG-3 specification in December 2023, so macOS's SDR support may not be fully mature.
   - Removing both the cICP and gAMA tags causes Preview to render as sRGB, and the colors differ from EyeQ only slightly in dark areas. This is because sRGB's nonlinear segment and BT.1886 both use 2.4 gamma, with only subtle differences visible in the shadows.
   - Removing only the cICP tag causes Preview to render with gamma = 1.961, matching QuickTime's BT.709 video rendering.

3. **Can I compare HDR and SDR videos simultaneously?**

   Yes, but you need to use `--main` to specify the main video.
   - If the main video is SDR, the target color space is SDR, and HDR videos will be tone-mapped
   - If the main video is HDR10, the target color space is HDR10, and SDR videos will be mapped using a reference white of 203 nits

4. **Error: `Failed picking any compatible texture format for a plane!`**

   This means libplacebo does not support the current pixel format. Use `--filter format=rgb48le` to convert the format via FFmpeg.

## License

This software is licensed under the GNU Lesser General Public License (LGPL) version 2.1 or later. See [LICENSE.md](./LICENSE.md) for details.
