# libplacebo 纹理格式兼容性

> 调查日期: 2026-03-18
> libplacebo 版本: v7.360.1 (API v360)
> GPU 环境: Apple M4 Pro + MoltenVK 1.4.1

## 概述

当 `pl_map_avframe_ex()` 返回 false 时, eyeq 会抛出 `texture_format_error` 异常并触发自动 fallback (追加 `format=rgb48le` 滤镜). 失败原因分两类, 但 eyeq 的处理方式对两者一致.

## 失败的两类原因

### 类型 A: 像素格式步长不兼容 (静默失败)

**代码路径:**
```
pl_map_avframe_ex()
  → pl_plane_data_from_pixfmt()    // libav_internal.h:506-607
    → 检查 comp->step 是否一致     // libav_internal.h:568-572
    → return 0                      // 无任何日志!
  → if (!planes) goto error;        // 直接返回 false
```

**原因:** 某些 packed 格式的不同分量有不同的 pixel stride. 例如 y210le:
- Y 分量: step=4, Cb/Cr 分量: step=8
- libplacebo 不支持这种不同步长的 packed 格式
- `pl_plane_data_from_pixfmt()` 直接返回 0, **不产生任何日志**

**典型格式:** y210le, y210be, y212le, y212be, p210le, p210be, yuyv422, uyvy422, nv20le 等具有不同组件步长的 packed 格式

### 类型 B: 无兼容 GPU 纹理格式 (触发 PL_ERR)

**代码路径:**
```
pl_map_avframe_ex()
  → pl_plane_data_from_pixfmt()    // 返回 planes > 0 (通过验证)
  → pl_upload_plane()               // upload.c:225
    → pl_plane_find_fmt()           // upload.c:163-223, 遍历 GPU 格式
    → return NULL                   // 无匹配格式
    → PL_ERR("Failed picking any compatible texture format for a plane!")
```

**原因:** 像素格式通过了步长验证, 但 GPU 没有匹配的 Vulkan 纹理格式.

**会触发 PL_ERR 的 17 个格式 (Apple M4 Pro + MoltenVK):**

| 格式 | planes | 失败的 plane |
|------|--------|-------------|
| bgr8 | 1 | plane0 |
| bgr4_byte | 1 | plane0 |
| rgb8 | 1 | plane0 |
| rgb4_byte | 1 | plane0 |
| rgbf32be | 1 | plane0 |
| rgbf32le | 1 | plane0 |
| v30xle | 1 | plane0 |
| rgbf16be | 1 | plane0 |
| rgbf16le | 1 | plane0 |
| rgba128be | 1 | plane0 |
| rgba128le | 1 | plane0 |
| rgb96be | 1 | plane0 |
| rgb96le | 1 | plane0 |
| gray32be | 1 | plane0 |
| gray32le | 1 | plane0 |
| gbrap32be | 4 | 全部 4 个 plane |
| gbrap32le | 4 | 全部 4 个 plane |

注意: 此列表与 GPU/驱动相关, 不同硬件结果不同.

## eyeq 自动 fallback 机制

### 调用链

```
main.cpp frame_callback:
  window.FeedFrames(frames)                    // render.cpp:102
    → VideoFrame::UpdateAVFrame()
    → VideoFrame::GeneratePLFrame()            // video_frame.cpp:60
      → pl_map_avframe_ex() 失败
      → throw texture_format_error(...)        // video_frame.cpp:100
    ← catch (texture_format_error)             // render.cpp:117
    ← 返回 failed_ids

  对每个 failed id:
    player.GetVideoSource(id)->RequestFormatFallback()  // main.cpp:572
      → 检查 filter_graph_ 是否已含 "format="
      → 设置 need_format_fallback_ = true
      → 设置 decode_need_reset_ = true

  player.SeekTo(player.CurrentTime())          // 触发重新解码

  解码线程:
    CreateFilterGraph()                        // video_source.cpp
      → 读取 need_format_fallback_
      → effective_filter += ",format=rgb48le"
      → 重新构建 filter graph
```

### 关键设计约束
- `filter_graph_` 不从主线程修改, 通过原子变量 `need_format_fallback_` 信号通知解码线程
- 只在解码线程的 `CreateFilterGraph()` 中修改 `effective_filter` (局部变量)
- 如果用户已指定 `format=` 滤镜, 不自动 fallback (尊重用户选择)
- 只尝试一次 fallback

## 测试视频生成

```bash
# 类型 A: 静默失败 (无 PL_ERR)
ffmpeg -y -f lavfi -i "color=c=red:s=320x240:d=2:r=25" -pix_fmt y210le -c:v rawvideo -f nut /tmp/test_y210le.nut

# 类型 B: 触发 PL_ERR("Failed picking...")
ffmpeg -y -f lavfi -i "color=c=red:s=320x240:d=1:r=25" -vf "format=bgr8" -c:v rawvideo -f nut /tmp/test_bgr8.nut
```

验证:
```bash
eyeq --loglevel debug /tmp/test_bgr8.nut
# 输出中应包含:
# Failed picking any compatible texture format for a plane!    ← libplacebo PL_ERR
# [warning] Video #0: pl_map_avframe_ex() failed for pixel format bgr8
# [warning] .../test_bgr8.nut: auto-appending format=rgb48le filter...

eyeq --loglevel debug /tmp/test_y210le.nut
# 输出中应包含:
# [warning] Video #0: pl_map_avframe_ex() failed for pixel format y210le
# [warning] .../test_y210le.nut: auto-appending format=rgb48le filter...
# 注意: 无 "Failed picking" 消息
```

## 代码定位索引

| 位置 | 文件 | 行号 | 说明 |
|------|------|------|------|
| 异常定义 | src/utils.hpp | - | `texture_format_error` 类 |
| 异常抛出 | src/display/video_frame.cpp | 99-102 | `pl_map_avframe_ex()` 失败时 |
| 异常捕获 | src/display/render.cpp | 117 | `FeedFrames()` 中 per-video 捕获 |
| fallback 触发 | src/main.cpp | 569-583 | frame callback 处理 failed_ids |
| fallback 请求 | src/video_source.cpp | `RequestFormatFallback()` | 设置原子标志 |
| 滤镜追加 | src/video_source.cpp | `CreateFilterGraph()` | 解码线程中追加 format=rgb48le |
| libplacebo 步长检查 | libplacebo libav_internal.h | 568-572 | 类型 A 失败点 |
| libplacebo PL_ERR | libplacebo upload.c | 233, 346 | 类型 B 错误消息 |
