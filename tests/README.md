# EyeQ Tests

[中文说明点我](./README_zh.md)

All automated tests live under `tests/`, grouped by the module they exercise, and are
wired into CMake by [tests/CMakeLists.txt](./CMakeLists.txt). Test targets are
optional: every switch below defaults to `OFF`, so the normal player build is
unaffected. Each suite states what it does and does not verify; no suite exercises a
real DeckLink device, a window present path, or desktop visual output.

## CMake switches

| Option | Default | Purpose |
|---|---|---|
| `EYEQ_BUILD_DECKLINK_TESTS` | `OFF` | Build the DeckLink CPU/fake contract tests and, together with `EYEQ_ENABLE_DECKLINK`, the deterministic SDK fault-injection and output-backend tests. No GPU, no device. |
| `EYEQ_BUILD_DECKLINK_GPU_TESTS` | `OFF` | Build the GPU packing tests: a CPU oracle, a real-GPU comparison/readback test and a lifecycle fault-injection test. Needs libplacebo and a working Vulkan device at runtime; no SDK and no DeckLink device. |
| `EYEQ_BUILD_DISPLAY_GPU_TESTS` | `OFF` | Build the offscreen display/rendering tests and the monitor composition tests: the real `RenderContext`/`FrameRenderer`/`MonitorOutput` on a headless Vulkan device, covering Fill/Slide/Grid, two-instance interleaving, HDR override, ICC isolation, OSD switches and the paced monitor path. Needs libplacebo and a working Vulkan device at runtime; no SDK, no window and no DeckLink device. |
| `EYEQ_BUILD_PLAYER_TESTS` | `OFF` | Build the CPU-only player tests: the real `SyncPlayer`/`VideoSource` over locally synthesized YUV4MPEG2 files, covering frame snapshot publication, serialized control completion, relative seeks, multi-source EOF alignment, bounded command work, stop cancellation and seek-failure propagation. No GPU, window, SDK or DeckLink device. |

`EYEQ_ENABLE_DECKLINK` and `DECKLINK_SDK_ROOT` are backend build switches, not test
switches; they are documented in the top-level [README](../README.md). Enabling them
together with `EYEQ_BUILD_DECKLINK_TESTS` additionally builds the SDK-header backend
tests below.

## DeckLink CPU/fake contract tests (no SDK, no GPU, no device)

```sh
cmake -S . -B build -DEYEQ_BUILD_DECKLINK_TESTS=ON
cmake --build build --target decklink_contract_test
ctest --test-dir build --output-on-failure
```

The suite runs the pure CPU seams: the output contract (`decklink_contract`), the
strict device-selector parsing shared with `decklink-catalog`
(`decklink_device_selector`), the strict monitor CLI parsing/validation
(`decklink_monitor_cli`), the deterministic test-signal pattern
(`decklink_test_signal_pattern`) and the independent r210/v210 known-answer reference
(`decklink_frame_packer_oracle`).

With `EYEQ_ENABLE_DECKLINK=ON` as well, the SDK-header backend tests are added under
`tests/decklink/backend` and `tests/decklink/packer`: catalog and monitor-mode
enumeration, the fault-injection catalog paths, the `decklink-catalog` CLI regression,
and the real output state machine, its concurrency/death tests and the test-signal
session/CLI tests — all against a fake SDK dispatch, never a device or a GPU.

## Player frame/control tests (no GPU, window or SDK)

```sh
cmake -S . -B build-player -DEYEQ_BUILD_PLAYER_TESTS=ON
cmake --build build-player --target player_sync_test
ctest --test-dir build-player -R player_sync --output-on-failure
```

`player_sync_test` drives the real `SyncPlayer`/`VideoSource` over locally
synthesized YUV4MPEG2 files and covers frame snapshot publication, serialized control
completion, relative seeks, multi-source EOF alignment, bounded command work, stop
cancellation and seek-failure propagation.

## GPU packing tests (no SDK, no DeckLink device)

A real Vulkan GPU is required; a missing or unsuitable GPU fails the test instead of
being skipped.

```sh
cmake -S . -B build-gpu -DEYEQ_BUILD_DECKLINK_GPU_TESTS=ON
cmake --build build-gpu
ctest --test-dir build-gpu --output-on-failure
```

- `frame_packer_gpu` compares real GPU read-back against that reference across widths
  around the format boundaries, extra strides, both precisions and encodings, finite
  clamping, grayscale/code-step anchors and chroma-phase impulses, plus a full-frame
  UHD check.
- `frame_packer_fault` injects transfer, readback, GPU-failure and allocation-failure
  conditions through test-only hooks that exist only in that test target.

## CPU known-answer and comparator tests (no GPU)

The CPU-only known-answer and comparator-mutation tests need no GPU:

```sh
cmake -S . -B build-cpu -DEYEQ_BUILD_DECKLINK_TESTS=ON
cmake --build build-cpu
ctest --test-dir build-cpu --output-on-failure
```

- `decklink_frame_packer_oracle` (CPU) checks the independent reference and the
  byte-level comparator against hand-derived known answers (primaries/complements,
  chroma filter weights, phase, padding) and against intentional corruption.

## Offscreen display/rendering tests

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

## Monitor composition-layer tests

The composition tests exercise the real layer against a headless Vulkan device and a
scripted fake sink (which can really return Still Busy/DeviceError/Closed):

```sh
cmake -S . -B build-monitor -DEYEQ_BUILD_DISPLAY_GPU_TESTS=ON
cmake --build build-monitor --target monitor_output_gpu_test monitor_output_fault_test
ctest --test-dir build-monitor -R monitor_output --output-on-failure
```

They cover SDR/PQ with r210/v210 (including non-tight `row_bytes` padding), known
black/colour/PQ-100-nit anchors, the CPU packing oracle, Fill/Slide/Grid/difference
amplification, Busy/priority/cancel/close and terminal sink failures, a fault-injected
real readback failure, and paced playback (composition order, strictly increasing
display ticks, per-frame bytes against the CPU oracle, queue overflow dropping the
oldest unscheduled frame, pause/resume and the freeze-still slot reclaim).
`monitor_output_fault_test` links a fault-injection build of the real
`frame_packer.cpp`, so the injection cannot silently miss. These tests do not verify a
window, real hardware frames or a DeckLink device.

## DeckLink test-signal generator

`decklink-test-signal` renders a deterministic CPU pattern and sends it through the
same output backend, without the `eyeq` player and without the GPU packer. It is
built by `EYEQ_ENABLE_DECKLINK=ON` (not by a test switch) and is intended for
on-hardware checks of SDI/HDMI links, mode timing, range and motion order; when run it
enables real hardware output.

```sh
cmake -S . -B build-sdk -DEYEQ_ENABLE_DECKLINK=ON \
  -DDECKLINK_SDK_ROOT="/path/to/Blackmagic DeckLink SDK 16.0"
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

## Hardware verification status

Automated validation exercises the native DeckLink write path only against a fake SDK
and the packer against a local GPU; the paced monitor path is verified with a
scripted fake sink, not with real hardware. That is the current verification status,
not a runtime restriction. SDI is the planned first hardware run; HDMI and PQ are
wired but have not been hardware-verified. `decklink-test-signal` still enables real
hardware output when run; its usage and options are documented in the top-level
[README](../README.md).

## Local UHD measurement

The local UHD four-slot short measurement is an offscreen number, not an end-to-end
4K50 result: roughly 1.0-1.1 ms/frame amortized, submit-to-Ready `p50` about 3-4 ms,
and about 33 MB (r210) or 22 MB (v210) of per-frame packed bytes. The test prints the
current numbers, reporting the mapped host buffers and the logical carrier payload
separately rather than claiming driver physical residency.
