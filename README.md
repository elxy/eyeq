# EyeQ

[中文介绍点我](./docs/README_zh.md)

A multi-video subjective quality comparison tool, focused on visual quality assessment in the lossless or near-lossless quality range.

Compared to [video-compare](https://github.com/pixop/video-compare), EyeQ adds:

1. High bit-depth, HDR10, Dolby Vision (Profile 5/8) and HDR Vivid 1.0 (`--hdrvivid-render`) support
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
eyeq --display-mode grid '<test_0>/*.jpg' '<test_1>/*.png' '<test_2>/*.jpg'
```

Image sequences are handled by FFmpeg's [image2 demuxer](https://ffmpeg.org/ffmpeg-formats.html#image2-1). Supported path patterns:
  - `'dir/*.jpg'` — glob pattern, matches all `.jpg` files in the directory (sorted by filename). **Quote it**, otherwise the shell expands it into separate arguments, each treated as a distinct video
  - `dir/%04d.jpg` — numbered sequence pattern, matches `0000.jpg`, `0001.jpg`, `0002.jpg`, …

These patterns also work in `fill` and `slide` modes, not limited to `grid` mode. Passing a bare directory path (e.g. `./images/`) is not supported.

> **Note:** EyeQ selects the `pattern_type` that the linked FFmpeg build actually supports. The legacy `dir/%*.jpg` syntax (required by `pattern_type=glob_sequence`, removed in FFmpeg 8.x) is accepted and automatically rewritten to plain glob, with a warning. If FFmpeg is built without glob support (`HAVE_GLOB`), EyeQ reports this explicitly — use the numbered sequence pattern instead.

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

Supported hardware decoders: `auto` (default), `none`, `videotoolbox` (macOS), `vaapi` (Linux), `cuda` (NVIDIA), `d3d12va` (Windows), `d3d11va` (Windows), `dxva2` (Windows).

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

- 0–9: Switch to the corresponding video (video indices start from 0; triggers on key release)
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
- Ctrl + E: Save the rendered frame (post-tone-mapping display output) as `render_<frame_number>.<video_id>.<filename>.dpx`
- Mouse wheel: Zoom in/out (centered on the cursor position)
- Middle mouse button drag: Pan the video
- I: Toggle OSD display
- Shift + S: Seek to the relative time at the cursor position
- Z: Reset zoom and position (fit to window)
- X: Force refresh (press multiple times if the window goes black after resizing)
- F: Toggle fullscreen
- F5: Reset all video offsets (re-align all videos with the main video)
- Esc: Leave fullscreen (quit when windowed; press Esc again to quit)
- Q: Quit

**Single Video Operations** (hold digit key N as modifier, then press an action key):

- N + A: Step video N backward 1 frame
- N + D: Step video N forward 1 frame
- N + →/←: Seek video N forward/backward 1 second
- N + ↓/↑: Seek video N forward/backward 5 seconds
- N + ] / Page Down / [ / Page Up: Seek video N forward/backward 1 minute
- N + F5: Reset video N offset (re-align with main video)
- Multiple digit keys can be held simultaneously to operate on multiple videos
- Frame offsets are preserved across global seek operations

### Command-Line Options

- `<videos>`: Specify videos to compare (multiple allowed). URLs are supported (FFmpeg built via Homebrew supports network protocols by default)
- `--ref`: Specify a reference video
- `--main`: Specify the main video. Other videos' rendering resolution and target color space will match the main video. If a reference video is specified, it becomes the main video by default; otherwise, the first video is used unless `--main` is set
- `--display-mode <mode>`: Display mode — `fill` (single window with key switching), `slide` (side-by-side swipe), `grid` (grid layout). If not specified, the mode is chosen automatically: `slide` for 2 videos, `fill` otherwise
- `--flicker <secs>`: During playback, alternate between the reference video and the test video at the specified interval (in seconds)
- `--amplify <ratio>`: Scale pixel differences relative to the reference video; > 1 amplifies, < 1 attenuates
- `--window-size <size>`: Window size, e.g. `--window-size 1920x1080`. In `fill` and `slide` modes, defaults to the main video's resolution; in `grid` mode, chosen automatically. Mutually exclusive with `--full-screen`
- `--full-screen`: Start in fullscreen mode. Uses the desktop resolution without switching display mode (borderless fullscreen), so the video is scaled to fit the screen. Mutually exclusive with `--window-size`
- `--grid-size <size>`: Grid layout size, e.g. `--grid-size 2x2`. Defaults to automatic selection
- `--filter <filter>`: Default video filter applied to videos without a per-video filter, using [FFmpeg Filters](https://ffmpeg.org/ffmpeg-filters.html) syntax. Per-video filters can be appended after the video path with `@`, e.g. `video.mp4@"filter"`
- `--filter-sep <sep>`: Separator for per-video filters (default `@`). Use this when filenames contain `@`
- `--no-colorspace-hint`: Disable video color space hinting; uses the default color space instead. Enable this for tone mapping when the display does not support HDR
- `--sdr-white-on-hdr <nits>`: Set the SDR reference white level (in nits) when the main video is HDR. By default, SDR videos are mapped using the standard reference white of 203 nits (ITU-R BT.2408). This option only takes effect when the main video is HDR and the target video is SDR
- `--target-display-nits <nits>`: Peak luminance of the common mapping target in nits — the single comparison baseline in every mode. With `--hdrvivid-render` it is also the target display luminance the Vivid curve is solved against; when monitoring is enabled it is the external monitor peak. Required for a `pq2020` monitor target (a physical monitor has no guessable default); otherwise it defaults to 1000. To override a source's own HDR peak, use `--source-max-luma`
- `--source-max-luma <nits>`: Opt-in override of the source HDR mastering/content peak (`pl_color_space.hdr.max_luma`) in nits. Use it when the source peak is untrusted (HLG, DPX read-back); it affects only sources that go through libplacebo's normal tone mapping, never rewrites the bitstream's dynamic metadata (the Vivid tml/rml anchors are part of the stream and stay untouched), and is decoupled from the mapping target
- `--hdrvivid-render`: Apply HDR Vivid 1.0 dynamic metadata with a GPU tone-mapping curve. Each frame that carries CUVA metadata is mapped to the common display mapping target (`--target-display-nits`) through the same single renderer every other source uses, so Vivid and Dolby Vision are compared in one display-linear space. Sources without Vivid metadata are left untouched, and the source's mastering/dynamic metadata is never rewritten into the public result
- `--high-dpi {auto,yes,no}`: High-DPI mode (default `auto`)
- `--scale-method <method>`: Upscaling method — `nearest`, `bilinear`, `bicubic`, `lanczos`, `ewa_lanczos`, `ewa_lanczossharp`, `mitchell`, `catmull_rom`, `spline36`, `spline64` (default `nearest`)
- `--plane-scale-method <method>`: Chroma interpolation method (default `lanczos`)
- `--seek-to <secs>`: Start playback at the specified time (in seconds)
- `--seek-to-frame <N>`: Start playback from the Nth frame (0-based)
- `--save-in-source`: Save frames (via `Ctrl + S` / `Ctrl + E`) to the source video's directory instead of the current working directory
- `--save-format <format>`: Frame save format (default `png`)
- `--save-render-format <format>`: Rendered-frame save format (default `dpx`)
- `--hardware-decoder {none,auto,videotoolbox,vaapi,cuda,d3d12va,d3d11va,dxva2}`: Hardware decoder (default `auto`)
- `--icc-profile <profile>`: ICC color management — `auto` uses the system profile (macOS only); a file path specifies a custom ICC profile; `N:path` specifies a profile for a specific video
- `--loglevel <level>`: Log level — `debug`, `info`, `warning`, `error`, `critical`, `off` (default `info`)
- `--debug`: Equivalent to `--loglevel debug`

### Monitoring Output (DeckLink)

A build made with `-DEYEQ_ENABLE_DECKLINK=ON` and the DeckLink SDK can send the current comparison result to an external monitor. This is **still-only**: the player stays paused and pressing Space only logs that continuous playback is not supported. The desktop preview and the DeckLink output consume the same composed result, so toggling the desktop OSD or resizing the window never changes the packed output bytes. Without `--monitor-device` the default player is unchanged and does not link the DeckLink backend.

- `--monitor-device <selector>`: Enable monitoring on a DeckLink device. The selector is explicit: `index:<n>` or `id:<persistent-id>`. Enabling monitoring disables every application-side ICC profile (global, `auto` and `N:path`, including the macOS preview); the macOS system color management is untouched
- `--monitor-mode <WxH@N|WxH@N/D>`: Native monitor mode as exact integer data, e.g. `3840x2160@50` or `3840x2160@30000/1001`. Required with `--monitor-device`; a mode the device does not natively support is rejected
- `--monitor-connection {sdi,hdmi}`: Output connection (default `sdi`)
- `--monitor-pixel-format {v210,r210}`: Output pixel format (default `v210`)
- `--monitor-color {sdr709,pq2020}`: Color target — `sdr709` (BT.709 / BT.1886) or `pq2020` (BT.2020 / PQ) (default `sdr709`)
- `--monitor-sdr-white <nits>`: SDR white level of the common mapping target when monitoring (default 203 nits)

A terminal monitor failure — a device error, a lost device, or a closed output — stops monitoring for the rest of the run: EyeQ logs the failure and keeps the desktop running, and monitoring only comes back after a player restart. A recoverable per-request failure (for example a `FramePacker` readback failure) is reported and the module keeps working; a failed request is not retried.

The monitor output quantizes the display-linear result directly to 10-bit without adding dither, so the output bytes are deterministic for tests and objective measurement; smooth gradients may show slight banding. `v210`'s 4:2:2 chroma subsampling comes with the format; `r210` (RGB 4:4:4) is the cleanest signal.

When monitoring is enabled, `--target-display-nits` is the external monitor peak (the comparison baseline); for a `pq2020` target it should match the monitor's calibrated peak. `--monitor-sdr-white` sets the SDR white of the common mapping target.

Migration from older builds: a source-peak override previously written as `--target-display-nits N` is now `--source-max-luma N`. `sdr709` monitoring no longer defaults the target to 100 nits; pass `--target-display-nits 100` to keep the old monitor peak.

### Environment Variables

- `EYEQ_FRAME_CACHE`: Override the automatic frame cache size. By default, EyeQ adjusts the frame cache based on total video resolution: 32 frames for ≤ 8K, 16 frames for > 8K, and 8 frames for > 16K total pixels. Set this variable to a positive integer to use a fixed cache size instead

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
   - If the main video is HDR10, the target color space is HDR10, and SDR videos will be mapped using a reference white of 203 nits. Use `--sdr-white-on-hdr` to override this value

4. **Incompatible pixel format (e.g. `pl_map_avframe_ex() failed`)**

   Some pixel formats are not supported by libplacebo's Vulkan backend — either the format has incompatible component strides (e.g. `y210le`) or no matching GPU texture format exists (e.g. `bgr8`). EyeQ will automatically append a `format=rgb48le` filter to convert the pixel format via FFmpeg. If you have already specified a `format=` filter manually, EyeQ will not override it — in that case, try `--filter format=rgb48le`.

5. **How do I play back a DPX frame saved via `Ctrl + E`?**

   The DPX header records transfer/primaries with SMPTE ST 268:2014 values (PQ=14, HLG=15, BT.2020=15) that FFmpeg's DPX decoder does not recognize (it maps transfer/colorimetric only up to 12/10), and its reference high quantity (HDR peak) is not read. EyeQ therefore sees an unknown color space and no `max_luma` — add a `setparams` filter to restore the color space, and `--source-max-luma` to supply the source peak used for tone mapping:

   ```
   eyeq --no-colorspace-hint --source-max-luma 1000 \
     'render_0001.0.foo.mkv.dpx@setparams=color_primaries=bt2020:color_trc=smpte2084'
   ```

   Use `color_trc=arib-std-b67` for HLG. On an HDR display the PQ output passes through without tone mapping, so `--no-colorspace-hint` and `--source-max-luma` are unnecessary; `--no-colorspace-hint` is required only on non-HDR displays to enable tone mapping.

## DeckLink Output

The DeckLink SDI/HDMI output backend has an independent implementation: device/mode enumeration, a default SDK frame pool, CPU frame copy, scheduled playback with completion events, still frames and safe shutdown. A GPU frame packer (`src/output/frame_packer.hpp` / `.cpp`) converts nonlinear full-range float RGB into Video-range r210/v210 through an asynchronous readback ring. The `eyeq` player is wired to the packer for **still-only** external monitoring (see the Monitoring Output section above): the composition layer keeps a single renderer and hands the same composed result to both the desktop preview and the monitor output, but continuous hardware-clock playback is not implemented yet. Audio and native HDR passthrough are not implemented, and `decklink-catalog` remains read-only. A standalone `decklink-test-signal` tool renders a deterministic CPU pattern through the same backend and enables real hardware output when run. Automated validation exercises the native write path only against a fake SDK and the packer against a local GPU; that is the current verification status, not a runtime restriction.

Optional CMake switches (all default to `OFF`; the normal player build is unaffected):

| Option | Default | Purpose |
|---|---|---|
| `EYEQ_BUILD_DECKLINK_TESTS` | `OFF` | Build the CPU/fake contract tests and, with `EYEQ_ENABLE_DECKLINK`, the deterministic SDK fault-injection and output-backend tests. No GPU, no device. |
| `EYEQ_ENABLE_DECKLINK` | `OFF` | Build the SDK backend (read-only catalog plus the output backend) and the `decklink-catalog` and `decklink-test-signal` tools. Requires macOS on Apple Silicon and `DECKLINK_SDK_ROOT`. It also wires the still-only external monitoring options into `eyeq`. |
| `EYEQ_BUILD_DECKLINK_GPU_TESTS` | `OFF` | Build the GPU packing tests: a CPU oracle, a real-GPU comparison/readback test and a lifecycle fault-injection test. Needs libplacebo and a working Vulkan device at runtime; no SDK and no DeckLink device. |
| `EYEQ_BUILD_DISPLAY_GPU_TESTS` | `OFF` | Build the offscreen display/rendering tests: the real `RenderContext`/`FrameRenderer` on a headless Vulkan device, covering Fill/Slide/Grid, two-instance interleaving, HDR override, ICC isolation and OSD switches. Needs libplacebo and a working Vulkan device at runtime; no SDK, no window and no DeckLink device. |
| `EYEQ_BUILD_PLAYER_TESTS` | `OFF` | Build the CPU-only player tests: the real `SyncPlayer`/`VideoSource` over locally synthesized YUV4MPEG2 files, covering frame snapshot publication, serialized control completion, relative seeks, multi-source EOF alignment, bounded command work, stop cancellation and seek-failure propagation. No GPU, window, SDK or DeckLink device. |
| `DECKLINK_SDK_ROOT` | *(empty)* | Path to the Blackmagic DeckLink SDK root (read-only); required by `EYEQ_ENABLE_DECKLINK`. Nothing is downloaded or modified. |

Running the CPU/fake contract tests (no SDK required):

```sh
cmake -S . -B build -DEYEQ_BUILD_DECKLINK_TESTS=ON
cmake --build build --target decklink_contract_test
ctest --test-dir build --output-on-failure
```

Running the player frame/control tests (no GPU, window or SDK required):

```sh
cmake -S . -B build-player -DEYEQ_BUILD_PLAYER_TESTS=ON
cmake --build build-player --target player_sync_test
ctest --test-dir build-player -R player_sync --output-on-failure
```

Building the read-only catalog backend and tool:

```sh
cmake -S . -B build-sdk -DEYEQ_ENABLE_DECKLINK=ON \
  -DDECKLINK_SDK_ROOT="/path/to/Blackmagic DeckLink SDK 16.0"
cmake --build build-sdk --target decklink-catalog
# List devices:
./build-sdk/bin/decklink-catalog
# List output modes for a device (read-only). The selector is explicit: index:<n> or
# id:<persistent-id>, and a bare number is rejected instead of being guessed.
./build-sdk/bin/decklink-catalog --device index:0 --connection sdi --pixel-format v210
./build-sdk/bin/decklink-catalog --device id:2697367084
```

The tool only queries the device; it never enables an output, schedules frames or changes device configuration.

### Test-signal generator

`decklink-test-signal` renders a deterministic CPU pattern and sends it through the
same output backend, without the `eyeq` player and without the GPU packer. It is
intended for on-hardware checks of SDI/HDMI links, mode timing, range and motion
order, and it enables real hardware output when run.

Current automated validation runs the tool and the backend only against the fake
SDK. That is a verification status, not a runtime behavior: the executable does
enable hardware output. SDI is the planned first hardware run; HDMI and PQ are wired
but have not been hardware-verified.

```sh
cmake --build build-sdk --target decklink-test-signal
# 10 seconds at the mode's rational rate, SDI, v210, SDR:
./build-sdk/bin/decklink-test-signal --device index:0 --connection sdi \
  --pixel-format v210 --mode auto --seconds 10
```

Options: `--device index:<n>|id:<id>` (required, same explicit selector as
`decklink-catalog`), `--connection sdi|hdmi`, `--pixel-format r210|v210`, `--mode
WxH@fps|auto` (auto prefers 3840x2160 50p; an explicit mode must be in the device's
native list), `--hdr` (BT2020/PQ), `--frames N` or `--seconds S` (mutually exclusive,
default 5 seconds), `--preroll N` (default 4, capped to the frame count; if the device
pool fills first, playback starts early instead of waiting for pre-Start completions),
`--drain-timeout-ms N` (default 5000, a no-progress timeout rather than a whole-run
deadline), `--still` (display one still frame; no start and no scheduling) and
`-h|--help`. Exit codes: 0 success, 1 usage/request/run error, 2 DeckLink driver
unavailable.

Before enabling the output the tool reads `bmdDeckLinkStatusBusy` on the selected
device and refuses if the device reports busy, if `IDeckLinkStatus` is unavailable or
if the query fails; it never probes with Enable/Disable and never terminates or
preempts another application such as DaVinci Resolve. That check is a one-shot
snapshot, not a device lock, so it cannot guarantee against a concurrent grab.
Ctrl-C/SIGTERM only sets a flag that the consumer thread observes: it stops
submitting new frames, then closes the output and consumes the retained events
before exiting.

### GPU frame packer

`eyeq_frame_packer` implements the `FramePacker` seam. It takes a sampleable,
nonlinear full-range float RGB texture (`RGBA16F`, `RGBA32F` or any float format with
at least 16-bit components) on the same `pl_gpu`, applies the frozen first-version
policy (BT709/SDR-BT1886 or BT2020/PQ, Video range, Left chroma, no transfer/tonemap/
ICC processing) and produces r210 (big-endian) or v210 (little-endian) bytes in a
fixed four-slot ring backed by persistent mapped host buffers and asynchronous
`pl_tex_download` + `pl_buf_poll`. In monitoring mode the `eyeq` player sends the
packer's output to an external monitor (see the Monitoring Output section above).

Running the GPU tests (no SDK, no DeckLink device; a real Vulkan GPU is required and a
missing GPU fails the test instead of being skipped):

```sh
cmake -S . -B build-gpu -DEYEQ_BUILD_DECKLINK_GPU_TESTS=ON
cmake --build build-gpu
ctest --test-dir build-gpu --output-on-failure
```

### Offscreen display/rendering tests

`RenderContext` owns the shared GPU lifetime (log, Vulkan instance/device, FFmpeg
Vulkan hw_device and the queue-lock callbacks) while `FrameRenderer` owns the
per-target rendering resources, so several targets can be rendered from one context.
The offscreen tests exercise the real classes without a window and without the
DeckLink SDK:

```sh
cmake -S . -B build-display -DEYEQ_BUILD_DISPLAY_GPU_TESTS=ON
cmake --build build-display --target display_renderer_gpu_test
ctest --test-dir build-display -R display_renderer_gpu --output-on-failure
```

They render Fill/Slide/Grid (including reference/difference amplification) into
distinct full-range RGB float targets, assert pixel and geometry results, run two
renderers with different sizes and color targets on the same GPU interleaved, check
that one renderer's output is unchanged by the other and survives its destruction,
and cover a failed frame update (valid-invalid-valid), the target-local HDR nits
override, ICC isolation and the OSD enable/disable switch. They do not verify the
window present path, desktop visual output or any hardware mapping; those remain
manual/environment-limited.

### Still monitor composition layer

`src/output/monitor_output.hpp` consumes the common display-linear result produced by
the coordination layer (`src/display/display_coordinator.*`), encodes it into a fixed
offscreen full-range RGB target in the selected output pixel format, packs it with
`FramePacker` and hands the bytes to `FrameSink::DisplayStill`. It never owns a
`FrameRenderer`. It owns the `FrameSink`; at most one request is in flight and main
advances it on the same thread with `Tick()` (or the low-level `Submit()`/`Pump()`). A
new request while one is in flight is reported as Busy and is not read or mapped.
`Close()` drains the packer and closes the owned sink within one total deadline.

With `EYEQ_ENABLE_DECKLINK=ON` the `eyeq` player is wired to this layer for still-only
external monitoring (see "Monitoring Output (DeckLink)"); continuous hardware-clock
playback, audio and native HDR passthrough are still not implemented.

The composition tests exercise the real layer against a headless Vulkan device and a
scripted fake sink (which can really return Still Busy/DeviceError/Closed):

```sh
cmake -S . -B build-monitor -DEYEQ_BUILD_DISPLAY_GPU_TESTS=ON
cmake --build build-monitor --target monitor_output_gpu_test monitor_output_fault_test
ctest --test-dir build-monitor -R monitor_output --output-on-failure
```

They cover SDR/PQ with r210/v210 (including non-tight `row_bytes` padding), known
black/colour/PQ-100-nit anchors, the CPU packing oracle, Fill/Slide/Grid/difference
amplification, Busy/priority/cancel/close and terminal sink failures, and a
fault-injected real readback failure. `monitor_output_fault_test` links a
fault-injection build of the real `frame_packer.cpp`, so the injection cannot silently
miss. These tests do not verify a window, real hardware frames or a DeckLink device.

The CPU-only known-answer and comparator-mutation tests belong to the CPU test suite
and need no GPU:

```sh
cmake -S . -B build-cpu -DEYEQ_BUILD_DECKLINK_TESTS=ON
cmake --build build-cpu
ctest --test-dir build-cpu --output-on-failure
```

- `decklink_frame_packer_oracle` (CPU) checks the independent reference and the
  byte-level comparator against hand-derived known answers (primaries/complements,
  chroma filter weights, phase, padding) and against intentional corruption.
- `frame_packer_gpu` compares real GPU read-back against that reference across widths
  around the format boundaries, extra strides, both precisions and encodings, finite
  clamping, grayscale/code-step anchors and chroma-phase impulses, plus a full-frame
  UHD check.
- `frame_packer_fault` injects transfer, readback, GPU-failure and allocation-failure
  conditions through test-only hooks that exist only in that test target.

Local UHD four-slot short measurement (offscreen, not an end-to-end 4K50 result):
roughly 1.0-1.1 ms/frame amortized, submit-to-Ready `p50` about 3-4 ms, and about
33 MB (r210) or 22 MB (v210) of per-frame packed bytes; the test prints the current
numbers, reporting the mapped host buffers and the logical carrier payload separately
rather than claiming driver physical residency. HDMI and PQ output remain wired but
have not been hardware-verified.

## License

This software is licensed under the GNU Lesser General Public License (LGPL) version 2.1 or later. See [LICENSE.md](./LICENSE.md) for details.
