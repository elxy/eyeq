# HDR Display and Luminance Mapping

This page explains which mappings the luminance goes through when EyeQ plays HDR content, which options decide them, and what happens on the macOS desktop and on a DeckLink monitor.

## 1. Overall Pipeline

```
Source (PQ / HLG / SDR, with static or dynamic metadata)
  │  ① EyeQ mapping: each source is mapped to the same mapping target; no mapping without a target
  ▼
Common result (all videos are compared, switched and swiped in the same luminance space)
  │  encoded as the output signal (PQ or SDR)
  ├─► macOS desktop window ──② system mapping (EDR)──► display
  └─► DeckLink output ──────────────────────────────► monitor (③ the monitor's own processing)
```

- ① is done by EyeQ and decided by `--target-display-nits` and `--tone-mapping` (section 3). The common result has a fixed size (the main video resolution for Fill/Slide, the window pixel size for Grid); after encoding it is scaled to the window or the DeckLink mode, keeping the aspect ratio with black borders, in the output encoding (e.g. PQ codes) with `--scale-method`. The desktop window and DeckLink show the same picture.
- ② exists only when EyeQ outputs PQ to the macOS desktop, and is done by the system. EyeQ cannot control it; it can only influence it through the HDR metadata handed to the system (section 4.3). A SDR output is a plain layer, and the system does not map its luminance.
- ③ depends on the monitor: a professional reference monitor usually shows PQ at absolute luminance and clips what exceeds the panel peak; a consumer TV may apply its own tone mapping.

Terms:

| Term | Meaning |
|---|---|
| PQ | The transfer function used by HDR10 (SMPTE ST 2084). Code values correspond to absolute luminance from 0 to 10000 nits |
| Mastering peak | The maximum luminance in the source's mastering display metadata, e.g. 1000 or 4000 nits |
| MaxCLL | The luminance of the brightest pixel in the content. It can be slightly above the mastering peak; EyeQ's mapping does not use it and only passes it to macOS unchanged |
| Mapping target | The display condition EyeQ maps every source to: a HDR display with peak N, or a SDR display |
| EDR headroom | How many times the SDR white macOS allows HDR to reach. It changes with the brightness setting, the preset, and whether HDR content is shown |
| Reference mode | A reference preset of an Apple XDR display, such as "HDR Video (P3-ST 2084)", with fixed SDR white and peak |

## 2. Which Setting to Use

| Purpose | Recommended setting |
|---|---|
| Everyday viewing, looking like QuickTime | No mapping target |
| Subjective quality assessment, absolute luminance judgment | Reference mode + `--target-display-nits 1000` |
| Reproducible rendered frames, or consistency across machines | A mapping target |
| Let EyeQ decide how HDR is mapped on a SDR display | `--target-display-nits sdr` |
| Simulate a reference monitor that clips above its peak | `--target-display-nits N --tone-mapping clip` or `clip-rgb` |

Without a mapping target, EyeQ does not compress the source, and the result depends entirely on the system mapping, which changes with the brightness setting and the display. With a mapping target, EyeQ's mapping does not depend on the machine or the display settings, and the same command gives the same Ctrl+E rendered frame.

## 3. Options and Rules

You only need to answer two questions: **where to map to** (`--target-display-nits`) and **how to map** (`--tone-mapping`).

### 3.1 Where to map to: `--target-display-nits <nits|sdr>`

| Value | Meaning | Output |
|---|---|---|
| Not given | No mapping target; EyeQ does not map | Follows the main video: PQ when the main video is HDR, SDR when it is SDR |
| `N` | A HDR display with a peak of N nits | PQ |
| `sdr` | A SDR display; the mapping target peak is the SDR white (`--reference-white`, default 203 nits) | SDR |

There is no default. Whether the output is PQ or SDR is decided by the mapping target (or by the main video without one), **not by the display**: when a PQ main video plays on a SDR display, EyeQ still outputs PQ, macOS maps it to SDR, and EyeQ warns about it.

The output color space (the same for the desktop window and DeckLink):

| Mapping target | Output color space |
|---|---|
| Not given | The main video's, e.g. BT.709 / BT.1886, BT.2020 / PQ, BT.2020 / HLG; BT.709 / BT.1886 without color tags |
| `N` | BT.2020 / PQ |
| `sdr` | BT.709 / BT.1886; BT.709 / sRGB when the main video is sRGB |

- The gamut of `sdr` is always BT.709: when the main video is wide-gamut SDR (e.g. P3), colors outside BT.709 are clipped. To keep the wide gamut, give no mapping target.
- Without a mapping target, when EyeQ cannot output the main video's transfer function (e.g. gamma 2.2), EyeQ fails and asks for a mapping target.
- When the system offers no window format for it (e.g. macOS has no P3 / BT.1886 window), EyeQ uses the window's actual gamut and says so in the log.

### 3.2 How to map: `--tone-mapping {system,clip,clip-rgb,spline}`

Only what exceeds the mapping target is processed; when the source peak does not exceed the target, all four give the same result.

| Value | Meaning |
|---|---|
| `system` | EyeQ does not map and leaves it to the system, the same as QuickTime. The default without a mapping target |
| `spline` | Roll off the highlights, keeping highlight detail. The default with a mapping target |
| `clip` | Clip and keep the hue: a pixel above the target is scaled down by its largest channel to the target peak, so it gets darker as a whole |
| `clip-rgb` | Clip R, G and B separately to the target peak; colors above it may turn whiter or shift in hue |

For example, with a mapping target of 300 nits, the source pixel (626, 513, 347) becomes (300, 247, 167) with `clip` and (300, 300, 300) with `clip-rgb`. Neither `clip` nor `clip-rgb` changes pixels within the range.

With `sdr` as the mapping target, `clip` scales down in BT.2020; when converting to the output gamut, saturated colors outside that gamut are still clipped per channel.

A source mapped by the curve of its own dynamic metadata is not affected by `--tone-mapping`. Currently this is only HDR Vivid with `--hdrvivid-render` (section 8).

### 3.3 Combining the two options

| `--target-display-nits` | `--tone-mapping` | Result |
|---|---|---|
| Not given | Not given or `system` | EyeQ does not map; the output follows the main video |
| Not given | `clip` / `clip-rgb` / `spline` | Error: no mapping target |
| `N` or `sdr` | Not given | Map to the target with `spline` |
| `N` or `sdr` | `clip` / `clip-rgb` / `spline` | Map to the target with the given method |
| `N` or `sdr` | `system` | Error: conflict |

### 3.4 When a mapping target is required

EyeQ does not choose a mapping target for you. In these cases EyeQ fails without one:

| Case | Required |
|---|---|
| The main video is SDR and there is a HDR video | `sdr` (SDR output) or `N` (PQ output, SDR videos placed at the reference white) |
| `--monitor-device` | `N` (the peak of a PQ monitor) or `sdr` (a SDR monitor) |
| `--hdrvivid-render` | `N` or `sdr`, as the target display luminance of the Vivid curve |

Other conflicts:

- An explicit `--monitor-color` must match the mapping target: `pq2020` with `N`, `sdr709` with `sdr`.
- When PQ output is requested but the platform provides no HDR window format, EyeQ fails and suggests `--target-display-nits sdr` instead of clipping silently.

## 4. macOS Desktop

This section is only about PQ output. EDR headroom, presets and system mapping may differ between displays.

### 4.1 Reference mode: the system shows absolute luminance

With a reference preset such as "HDR Video (P3-ST 2084)", the system sends PQ to the display at absolute luminance and no longer compresses it (while the content does not exceed the display peak). Only EyeQ's mapping ① applies then: when the mapping target equals the display peak (e.g. `--target-display-nits 1000`), the luminance on screen is the luminance EyeQ computes.

**For subjective quality assessment, HDR judgment and tone-mapping evaluation, use reference mode with a mapping target.**

### 4.2 Non-reference mode: the system maps again

With a default preset (such as "Pro Display XDR (P3-1600 nits)"):

- PQ is not shown at absolute luminance, but scaled up or down relative to the current SDR brightness;
- highlights above the current EDR headroom are compressed again by the system;
- the system's mapping curve is not published, changes with the brightness slider, auto-brightness and ambient light, and takes into account the mastering peak EyeQ hands to it (section 4.3).

So absolute luminance judgments are not reliable, and shadow and highlight visibility differ from the actual viewing conditions. But all videos go through the same system mapping at the same time, so **relative A/B comparison is still fair**. When the output is PQ and the display is not in reference mode, EyeQ warns at startup.

### 4.3 HDR metadata handed to macOS

With PQ output, EyeQ hands one set of static HDR metadata to the system along with the window. It is only metadata and does not change any pixel:

| Value | Content |
|---|---|
| Mastering peak (declared peak) | With mapping target N: min(N, the largest mastering peak of all HDR sources). Without a mapping target: the largest mastering peak of all HDR sources |
| MaxCLL, MaxFALL | The main video's values, passed unchanged |

- The declared peak is computed once in the first few frames and then fixed; switching the main video does not change it.
- In non-reference mode, **the higher the declared peak, the darker the whole picture**, including shadows and midtones, not only highlights. MaxCLL has no such effect.
- Reference mode, DeckLink monitors and Ctrl+E saved frames are not affected by the declared peak.
- As a result: when several videos play without a mapping target, a video with a lower mastering peak looks darker than when played alone; with a mapping target, the declared peak does not exceed the target, so the system does not compress again according to a higher source peak.

### 4.4 Comparing with QuickTime

QuickTime does not compress HDR10 itself, but hands the source metadata to the system for mapping. EyeQ does the same without a mapping target, and the pixels the two send to the screen differ by less than 1%; the remaining difference presumably comes from the decoding implementation (e.g. chroma upsampling). With a mapping target, a video mastered above the target looks different from QuickTime, because EyeQ compresses the highlights first.

When comparing the two players by eye: QuickTime dims HDR highlights when it loses focus, while EyeQ does not. The one being watched must be in the foreground.

## 5. Frames Saved with Ctrl+E

Ctrl+E saves the final picture EyeQ presents to the desktop window, i.e. the result after mapping ① and before it is handed to macOS, without the system mapping ②:

- The resolution is the window's pixel size, not the video resolution; the frame includes the layout of the current display mode (Slide divider, Grid cells, black borders) and the OSD text.
- It is saved in the window's current output encoding: PQ/BT.2020 or SDR.
- Without a mapping target, the saved frame is not compressed, and its luminance can reach the source peak.
- In non-reference mode, the saved frame differs in luminance from what is seen on screen; in reference mode the two match.

To view a saved DPX again, FFmpeg cannot read the PQ/HLG and the peak from the file header (section 6), so use `setparams` together with `--default-mastering-nits`. Use the output peak at the time of saving: the mapping target if one was given, otherwise the source's mastering peak.

## 6. Source Mastering Peak

Both EyeQ's mapping ① and the declared peak depend on the source's mastering peak. A PQ source without a mastering peak is treated as reaching 10000 nits: with a mapping target it is over-compressed, without one the declared peak is too high, and in both cases the picture looks darker. EyeQ warns about such sources.

`--default-mastering-nits N` supplies a peak to HDR sources that have no mastering peak:

- Only sources without one are filled in; sources with a mastering peak are left unchanged. A Dolby Vision source without mastering metadata gets its peak from the RPU and does not count as missing.
- MaxCLL and dynamic metadata are left unchanged. A MaxCLL above N is only reported.

EyeQ reads metadata through FFmpeg. Peaks in video bitstreams such as HEVC and AV1, and in MP4/MOV/MKV containers, are read; but for DPX and JPEG XL, FFmpeg does not read the peak even if the file records one, and it cannot read the PQ/HLG tags of DPX either. The `setparams` filter can only change color tags and cannot supply a peak, so such files need both `setparams` and `--default-mastering-nits`.

## 7. DeckLink Monitor

With `--monitor-device`:

- A mapping target is required. `--target-display-nits N` outputs a PQ signal, and N should be set to the monitor's actual peak; `--target-display-nits sdr` outputs a BT.709 / BT.1886 signal with `--reference-white` as the SDR white.
- The PQ signal only declares PQ and BT.2020, without mastering or MaxCLL metadata.
- There is no macOS system mapping. How the picture is shown is up to the monitor: a reference monitor shows absolute luminance and clips above its peak; `--tone-mapping clip` or `clip-rgb` makes EyeQ handle it the same way.
- The desktop window and the monitor use the same mapping result and show the same picture (zoom, pan, slide and switching stay in sync). When the common result differs in size from the monitor mode, it is scaled with `--scale-method`, keeping the aspect ratio with black borders. The desktop window still goes through the system mapping of section 4, so the two may look different; trust the monitor.

## 8. Sources with Dynamic Metadata

Dynamic metadata describes luminance per scene or per frame, to adapt the content to displays of different luminance. Whether it changes the picture depends on the standard and the implementation: some schemes only compress when the target is darker than the master; others (such as the HDR Vivid curve and Dolby's display management) adjust the whole curve to the target, and may change the picture even when the target is brighter.

Without a mapping target, EyeQ does not map, and dynamic metadata has no effect.

- **Dolby Vision**: EyeQ performs the reshaping and uses the L1 scene luminance instead of the mastering peak; the mapping method is still decided by `--tone-mapping`. EyeQ does not implement Dolby's full official display management, while QuickTime uses macOS's Dolby display management. **So Dolby Vision may look different in EyeQ and QuickTime by nature**, and leaving out the mapping target does not remove this difference.
- **HDR10+**: EyeQ uses the scene peak instead of the mastering peak; the mapping method is still decided by `--tone-mapping`, and HDR10+'s own mapping curve is not used.
- **HDR Vivid**:
  - Without `--hdrvivid-render`, it is handled as plain HDR10.
  - With `--hdrvivid-render`, a mapping target is required, and EyeQ maps to it with the Vivid curve. This may change the picture even when the source peak does not exceed the target, and is not affected by `--tone-mapping`. It simulates the "target display": in reference mode or on a DeckLink monitor it is suitable for evaluating Vivid; in non-reference mode the system maps again, so it is not.

## 9. Multiple Videos

- All videos are mapped to **the same** mapping target and compared in the same luminance space.
- Each video is mapped with **its own** metadata. If the same content carries different metadata (e.g. one with a 4000 nits master and one with a 1000 nits master), the pictures differ with a mapping target; this is the real result of the metadata difference.
- There is only one system mapping and one declared peak, taken from all HDR sources (section 4.3).
- Comparing HDR and SDR videos together: when the main video is HDR, the SDR white is placed into the HDR output at `--reference-white`, and in non-reference mode it may look brighter than the SDR white of the desktop; when the main video is SDR, a mapping target is required.
- Dynamic metadata only affects the video that carries it; when compared with other videos, a difference may come from the mapping method rather than the coding quality alone. For near-lossless comparison, keep the source peak at or below the mapping target and do not enable `--hdrvivid-render`, so that no video is compressed.

## 10. Known Limitations

- In reference mode, whether the system clips or compresses content above the display peak has not been verified.
- On a SDR display, which looks better — leaving it to macOS or letting EyeQ map to SDR — has not been compared.
- Dolby Vision and HDR10+ have not been compared with QuickTime; `--hdrvivid-render` with `sdr` as the target has not been verified.
- On Linux and Windows, without a mapping target the system compositor handles HDR, which has not been verified; third-party HDR displays on macOS have not been tested.
