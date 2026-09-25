#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/log.h>
#include <libavutil/frame.h>
}
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "CLI11.hpp"
#include "version.h"
#include "log.hpp"
#include "utils.hpp"
#include "sync_player.hpp"
#include "display/window.hpp"
#include "display/render.hpp"
#include "display/platform_activate.h"

#include <libplacebo/colorspace.h>

using namespace EYEQ;

struct EyeQArgs {
  std::vector<std::string> videos;  // All video paths, indexed by ID
  std::vector<std::string> filters; // Filters corresponding to each video
  int main_id;
  std::optional<int> ref_id;
  std::string default_filter; // Global default filter
  std::string filter_sep;     // Per-video filter separator

  DisplayMode display_mode;
  float flicker_interval;
  std::optional<float> amplify;
  int win_w, win_h;
  int grid_w, grid_h;
  bool full_screen;

  bool colorspace_hint;
  HighDpiMode high_dpi_mode;
  ScaleMethod scale_method;
  ScaleMethod plane_scale_method;

  float seek_to;
  size_t seek_to_frame;

  bool save_in_source;
  std::string save_format;
  std::string save_render_format;

  LoggingLevel log_level;
  HardwareDecoder hardware_decoder;

  std::vector<std::string> icc_profiles; // ICC profile specifications
  float sdr_white_on_hdr;                // SDR white level (nits) when rendering under HDR
  double target_display_nits;            // Target display peak luminance (nits); overrides HDR max_luma when passed
  bool target_display_set;               // Whether --target-display-nits was explicitly passed
};

void print_version([[maybe_unused]] int count) {
  std::cout << "eyeq version: " << VERSION_TAG << std::endl;
  exit(0);
}

void parse_size(int &w, int &h, const std::string str) {
  std::stringstream ss(str);
  char x;
  ss >> w >> x >> h;
  if ('x' != x) {
    throw std::invalid_argument("Invalid format: expected 'x' as separator");
  }
}

static std::string generate_title(const struct EyeQArgs &args, const std::vector<int> &ids) {
  if (DisplayMode::Slide != args.display_mode) {
    return args.videos[ids[0]];
  }

  return args.videos[ids[0]] + " | " + args.videos[ids[1]];
}

static void parse_args(struct EyeQArgs &args, int argc, char **argv) {
  CLI::App app{"EyeQ, a video compare tool for subjective quality assessment focused on lossless or near-lossless "
               "quality range. Supports HDR10 and 10-bit format content display."};
  argv = app.ensure_utf8(argv);
  app.usage("Comparing videos:\n"
            "  eyeq [videos]...\n"
            "Comparing with reference:\n"
            "  eyeq [videos]... --ref [reference]");

  app.footer("CONTROL KEYS:\n"
             "  - 0-9: Switch to corresponding video\n"
             "  - R: Switch to the reference video (resumes when key is released)\n"
             "  - T: Swap the left and right videos (resumes when key is released)\n"
             "  - Space: Pause/Play\n"
             "  - →: Forward 1 second\n"
             "  - ←: Backward 1 second\n"
             "  - ↓: Forward 5 seconds\n"
             "  - ↑: Backward 5 seconds\n"
             "  - ] / Page Down: Forward 60 seconds\n"
             "  - [ / Page Up: Backward 60 seconds\n"
             "  - Shift + ] / End: Forward 10000 seconds\n"
             "  - Shift + [ / Home: Backward 10000 seconds\n"
             "  - A: Previous frame\n"
             "  - D: Next frame\n"
             "  - Ctrl + S: Save current frame of video\n"
             "  - Ctrl + E: Save rendered frame (DPX)\n"
             "  - Shift + S: Seek to mouse X position\n"
             "  - I: Toggle OSD (On-Screen Display)\n"
             "  - Mouse wheel: Zoom video (centered on cursor)\n"
             "  - Hold middle mouse button and drag: Pan video\n"
             "  - Z: Reset zoom and position, fit to window\n"
             "  - X: Force refresh\n"
             "  - F: Toggle fullscreen\n"
             "  - Esc: Leave fullscreen (quit when windowed)\n"
             "  - Q: Quit");

  app.add_flag_function("--version", print_version, "Print the version of this program");
  std::vector<std::string> videos;
  auto option_videos =
      app.add_option<std::vector<std::string>>("videos", videos, "Video files to display")->expected(1, 10)->required();
  auto option_ref =
      app.add_flag("--ref", "Specific reference video")->multi_option_policy(CLI::MultiOptionPolicy::Throw);
  auto option_main = app.add_flag("--main", "Specific main video")->multi_option_policy(CLI::MultiOptionPolicy::Throw);

  app.add_option("--filter", args.default_filter, "Default filter applied to videos without per-video filter");
  app.add_option("--filter-sep", args.filter_sep, "Separator for per-video filter in video path (default: @)");

  std::optional<DisplayMode> display_mode;
  std::map<std::string, DisplayMode> dm_map{
      {"fill", DisplayMode::Fill}, {"slide", DisplayMode::Slide}, {"grid", DisplayMode::Grid}};
  app.add_option("--display-mode", display_mode, "Display mode: slide mode for 2 videos, fill mode otherwise")
      ->transform(CLI::CheckedTransformer(dm_map, CLI::ignore_case))
      ->option_text("{fill,slide,grid}");
  app.add_option("--flicker", args.flicker_interval,
                 "Specific reference flicker interval in seconds, <= 0 means no flicker");
  app.add_option("--amplify", args.amplify, "Amplifies the artefacts of videos relative to reference")
      ->needs(option_ref);

  auto *option_window_size = app.add_option_function<std::string>(
      "--window-size", [&args](const std::string &s) { parse_size(args.win_w, args.win_h, s); },
      "Specific the resolution of window size, e.g. 1920x1080. Default is auto selected");
  app.add_option_function<std::string>(
      "--grid-size", [&args](const std::string &s) { parse_size(args.grid_w, args.grid_h, s); },
      "Specific the numbers of columns and rows in grid mode, e.g. 3x1. Default is auto selected");
  auto *option_full_screen =
      app.add_flag("--full-screen", args.full_screen, "Start in fullscreen (desktop resolution, no mode switch)");
  option_full_screen->excludes(option_window_size);

  app.add_flag("!--no-colorspace-hint", args.colorspace_hint, "Disable colorspace hint to allow tone mapping");
  app.add_option("--sdr-white-on-hdr", args.sdr_white_on_hdr,
                 "SDR reference white level in nits when the main video is HDR (default: 203)");
  app.add_option("--target-display-nits", args.target_display_nits,
                 "Target display peak luminance (nits); when passed, overrides the peak (max_luma) of all HDR "
                 "content");
  std::map<std::string, HighDpiMode> hdpi_map{
      {"auto", HighDpiMode::Auto}, {"yes", HighDpiMode::Yes}, {"no", HighDpiMode::No}};
  app.add_option("--high-dpi", args.high_dpi_mode, "High DPI mode")
      ->transform(CLI::CheckedTransformer(hdpi_map, CLI::ignore_case))
      ->option_text("{auto,yes,no}")
      ->default_str("auto");
  std::map<std::string, ScaleMethod> sm_map{
      {"nearest", ScaleMethod::Nearest},        {"bilinear", ScaleMethod::Bilinear},
      {"bicubic", ScaleMethod::Bicubic},        {"lanczos", ScaleMethod::Lanczos},
      {"ewa_lanczos", ScaleMethod::EWALanczos}, {"ewa_lanczossharp", ScaleMethod::EWALanczosSharp},
      {"mitchell", ScaleMethod::Mitchell},      {"catmull_rom", ScaleMethod::CatmullRom},
      {"spline36", ScaleMethod::Spline36},      {"spline64", ScaleMethod::Spline64},
  };
  std::string sm_option_text =
      "{nearest,bilinear,bicubic,lanczos,ewa_lanczos,ewa_lanczossharp,mitchell,catmull_rom,spline36,spline64}";
  app.add_option("--scale-method", args.scale_method, "Scale method for video upscaling and chroma plane interpolation")
      ->transform(CLI::CheckedTransformer(sm_map, CLI::ignore_case))
      ->option_text(sm_option_text);
  app.add_option("--plane-scale-method", args.plane_scale_method, "Scale method for chroma planes")
      ->transform(CLI::CheckedTransformer(sm_map, CLI::ignore_case))
      ->option_text(sm_option_text);

  app.add_option("--seek-to", args.seek_to, "Seek to seconds before playing");
  app.add_option("--seek-to-frame", args.seek_to_frame, "Start playing from the Nth frame (0-based)")
      ->excludes("--seek-to");

  app.add_flag("--save-in-source", args.save_in_source, "Use source's directory to save frames");
  app.add_option("--save-format", args.save_format, "Format of saved frames, default is png");
  app.add_option("--save-render-format", args.save_render_format,
                 "Format of saved rendered frames, default is dpx");

  std::map<std::string, LoggingLevel> ll_map{
      {"off", LoggingLevel::NONE},     {"critical", LoggingLevel::FATAL}, {"error", LoggingLevel::ERR},
      {"warning", LoggingLevel::WARN}, {"info", LoggingLevel::INFO},      {"debug", LoggingLevel::DEBUG},
      {"trace", LoggingLevel::TRACE},
  };
  auto *option_loglevel = app.add_option("--loglevel", args.log_level, "Log level")
                              ->transform(CLI::CheckedTransformer(ll_map, CLI::ignore_case))
                              ->option_text("{off,critical,error,warning,info,debug,trace}");
  app.add_flag("--debug", "Shorthand for --loglevel debug")->excludes(option_loglevel);

  std::map<std::string, HardwareDecoder> hw_map{
      {"none", HardwareDecoder::None},
      {"auto", HardwareDecoder::Auto},
      {"videotoolbox", HardwareDecoder::VideoToolbox},
      {"vaapi", HardwareDecoder::VAAPI},
      {"cuda", HardwareDecoder::CUDA},
      {"d3d12va", HardwareDecoder::D3D12VA},
      {"d3d11va", HardwareDecoder::D3D11VA},
      {"dxva2", HardwareDecoder::DXVA2},
  };
  app.add_option("--hardware-decoder", args.hardware_decoder, "Hardware decoder")
      ->transform(CLI::CheckedTransformer(hw_map, CLI::ignore_case))
      ->option_text("{none,auto,videotoolbox,vaapi,cuda,d3d12va,d3d11va,dxva2}");

  app.add_option("--icc-profile", args.icc_profiles,
                 "ICC profile for display color management. "
                 "Use 'auto' for system profile, a file path for custom profile, "
                 "or 'N:path' to assign to specific video N (e.g. '0:display.icc')")
      ->expected(1, 10);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    exit(app.exit(e));
  }

  if (app.count("--debug")) {
    args.log_level = LoggingLevel::DEBUG;
  }

  // Overriding HDR max_luma is opt-in: only when the user explicitly passes --target-display-nits.
  args.target_display_set = app.count("--target-display-nits") > 0;

  int video_idx = 0;
  std::optional<int> ref_idx;
  std::optional<int> main_idx;
  for (auto *option : app.parse_order()) {
    if (option == option_videos)
      video_idx++;
    if (option == option_main)
      main_idx = video_idx;
    if (option == option_ref)
      ref_idx = video_idx;
  }

  video_idx = 0;
  std::optional<int> main_id;
  for (const auto &video : videos) {
    const bool is_ref = ref_idx.value_or(-1) == video_idx;
    const bool is_main = main_idx.value_or(-1) == video_idx;

    if (is_main)
      main_id = video_idx;
    if (is_ref)
      args.ref_id = video_idx;

    // Split video path and per-video filter
    std::string path = video;
    std::string filter;
    size_t sep_pos = video.rfind(args.filter_sep);
    if (sep_pos != std::string::npos) {
      path = video.substr(0, sep_pos);
      filter = video.substr(sep_pos + args.filter_sep.size());
    } else {
      filter = args.default_filter;
    }
    args.videos.push_back(path);
    args.filters.push_back(filter);
    video_idx++;
  }

  args.main_id = main_idx.has_value() ? main_id.value() : (args.ref_id.has_value() ? args.ref_id.value() : 0);

  // Number of non-reference videos
  size_t nb_display_videos = args.videos.size() - (args.ref_id.has_value() ? 1 : 0);

  // Set and validate display mode
  if (!display_mode.has_value()) {
    if (nb_display_videos == 2) {
      display_mode = DisplayMode::Slide;
    } else {
      display_mode = DisplayMode::Fill;
    }
  }
  args.display_mode = display_mode.value();

  if (DisplayMode::Slide == args.display_mode) {
    if (nb_display_videos < 2) {
      Logger->critical("At least 2 videos are needed for slide mode");
      exit(1);
    } else if (nb_display_videos > 2) {
      Logger->warn("{} videos are given, only the first 2 will be used", nb_display_videos);
    }
  } else if (DisplayMode::Grid == args.display_mode) {
    if (nb_display_videos < 2) {
      Logger->critical("At least 2 videos are needed for grid mode");
      exit(1);
    }
  }

  if (args.grid_w > 0 && args.grid_h > 0) {
    if (args.grid_w * args.grid_h < static_cast<int>(nb_display_videos)) {
      Logger->critical("The numbers of columns and rows in grid mode must be larger than the number of videos");
      exit(1);
    }
  }
}

static void update_window_and_grid_size(int &win_w, int &win_h, int &grid_w, int &grid_h, const int video_w,
                                        const int video_h, const int nb_videos, SDL_DisplayID display_id,
                                        bool high_dpi) {
  int display_w, display_h;
  Window::GetDisplayResolution(display_w, display_h, display_id, high_dpi);
  float pixel_density = Window::GetDisplayPixelDensity(display_id, high_dpi);

  bool win_auto = win_w <= 0 || win_h <= 0;
  bool grid_auto = grid_w <= 0 || grid_h <= 0;
  if (grid_auto) {
    int max_w = win_auto ? display_w : win_w;
    int max_h = win_auto ? display_h : win_h;
    calculate_best_grid_size(grid_w, grid_h, max_w, max_h, pixel_density, video_w, video_h, nb_videos);
    Logger->debug("Best grid size is {}x{}", grid_w, grid_h);
  }

  if (win_auto) {
    calculate_best_window_size(grid_w, grid_h, win_w, win_h, display_w, display_h, pixel_density, video_w, video_h);
    Logger->debug("Best window size is {}x{}", win_w, win_h);
  }
}

static void fit_to_window(DisplayState &state, SyncPlayer &player, const Window &window, DisplayMode display_mode) {
  if (DisplayMode::Grid != display_mode) {
    int resized_w, resized_h;
    int src_w = player.Width();
    int dst_w = window.PixelWidth();
    int dst_h = window.PixelHeight();
    Window::FitResolutionWithAspectRatio(resized_w, resized_h, (float)player.Width() / player.Height(), dst_w, dst_h);
    state.scale = static_cast<float>(resized_w) / src_w;
    state.offset_x = (((float)dst_w - resized_w) / 2.);
    state.offset_y = (((float)dst_h - resized_h) / 2.);
  } else {
    // In Grid mode, state.scale is a relative zoom factor; initial 1.0 means fit to cell
    // Centering offset is computed per-video by the renderer; reset offsets to zero here
    state.scale = 1.;
    state.offset_x = 0.;
    state.offset_y = 0.;
  }
}

static void sigterm_handler([[maybe_unused]] int sig) { exit(123); }

// Saves the frames the user is currently seeing. The serial in the file name
// comes from the consumed snapshot, not from the mutating decode cursor, so the
// saved file matches the displayed frame even if the player has moved on.
static void save_snapshot_frames(const EyeQArgs &args, const FrameSnapshot &snapshot) {
  for (const auto &[id, frame] : snapshot.frames) {
    int serial = 0;
    if (auto it = snapshot.metadata.find(id); it != snapshot.metadata.end()) {
      serial = it->second.frame_serial;
    }
    std::filesystem::path filename(args.videos[id]);
    std::filesystem::path path;
    if (args.save_in_source) {
      path = filename.parent_path();
    }
    path /= fmt::format("{:04d}.{}.{}.{}", serial, id, filename.filename().string(), args.save_format);
    if (save_frame(frame.get(), path.string(), args.save_format) < 0) {
      Logger->error("Failed to save frame to {}", path.string());
    } else {
      Logger->info("{} saved", path.string());
    }
  }
}

int main(int argc, char **argv) {
#ifdef _WIN32
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
#endif

  struct EyeQArgs args{
      .filter_sep = "@",
      .flicker_interval = 0,
      .win_w = 0,
      .win_h = 0,
      .grid_w = 0,
      .grid_h = 0,
      .full_screen = false,
      .colorspace_hint = true,
      .high_dpi_mode = HighDpiMode::Auto,
      .scale_method = ScaleMethod::Nearest,
      .plane_scale_method = ScaleMethod::Lanczos,
      .seek_to = 0.,
      .seek_to_frame = 0,
      .save_in_source = false,
      .save_format = "png",
      .save_render_format = "dpx",
      .log_level = LoggingLevel::INFO,
      .hardware_decoder = HardwareDecoder::Auto,
      .sdr_white_on_hdr = 0,
      .target_display_nits = 1000.0,
      .target_display_set = false,
  };

  parse_args(args, argc, argv);
  set_log_level(args.log_level);

  if (args.target_display_set && (args.target_display_nits <= 0.0 || args.target_display_nits > 10000.0)) {
    Logger->critical("--target-display-nits must be in the range (0, 10000] nits");
    return 1;
  }

  signal(SIGINT, sigterm_handler);  // Interrupt (ANSI)
  signal(SIGTERM, sigterm_handler); // Termination (ANSI)

  uint32_t flags = SDL_INIT_VIDEO;
  if (!SDL_Init(flags)) {
    Logger->critical("Could not initialize SDL: {}", SDL_GetError());
    exit(1);
  }

  // Allow mouse clicks to pass through to inactive windows on macOS, so that
  // click events reach the SDL event loop even when the app is not activated.
  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
  SDL_DisplayID display_id = Window::CurrentDisplay();

  SyncPlayer player;
  int num_videos = static_cast<int>(args.videos.size());
  int decode_threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / num_videos);
  std::vector<int> ids;
  for (int i = 0; i < num_videos; i++) {
    try {
      player.AddVideoSource(
          std::make_unique<VideoSource>(args.videos[i], args.filters[i], decode_threads, args.hardware_decoder), i);
    } catch (const std::exception &e) {
      Logger->critical(e.what());
      return 1;
    }
    if (!args.ref_id.has_value() || i != args.ref_id.value())
      ids.push_back(i);
  }

  // Determine frame cache size: env override > auto based on total resolution
  {
    size_t frame_cache = 32;
    const char *env_cache = std::getenv("EYEQ_FRAME_CACHE");
    if (env_cache) {
      frame_cache = std::stoul(env_cache);
      Logger->info("Frame cache set to {} via EYEQ_FRAME_CACHE", frame_cache);
    } else {
      // Total resolution = sum of width * height across all videos
      int64_t total_pixels = 0;
      for (auto &[id, src] : player) {
        total_pixels += static_cast<int64_t>(src->CodecWidth()) * src->CodecHeight();
      }
      // 8K ~ 7680x4320 = 33,177,600 pixels;  16K ~ 2x 8K = 66,355,200 pixels
      constexpr int64_t k8KPixels = 33'177'600;
      constexpr int64_t k16KPixels = k8KPixels * 4;
      if (total_pixels > k16KPixels) {
        frame_cache = 8;
      } else if (total_pixels > k8KPixels) {
        frame_cache = 16;
      }
      Logger->debug("Total resolution {:.1f} Mpx, frame cache auto-set to {}", total_pixels / 1e6, frame_cache);
    }
    for (auto &[id, src] : player) {
      src->SetMaxCachedFrames(frame_cache);
    }
  }
  player.SetMainId(args.main_id);
  Logger->info("Main video is #{}: {}", args.main_id, args.videos[args.main_id]);
  try {
    player.StartSources(args.seek_to, args.seek_to_frame);
  } catch (const std::exception &e) {
    Logger->critical("Failed to start sources: {}", e.what());
    player.Stop();
    SDL_Quit();
    return 1;
  }

  // Check if hardware decoding took effect for all videos
  if (args.hardware_decoder != HardwareDecoder::None) {
    auto unused_ids = player.CheckUnusedHardwareDecoders();
    if (!unused_ids.empty()) {
      std::string codec_list;
      for (int id : unused_ids) {
        if (!codec_list.empty())
          codec_list += ", ";
        codec_list += fmt::format("#{} ({})", id, args.videos[id]);
      }
      static const std::map<HardwareDecoder, const char *> hw_names = {
          {HardwareDecoder::Auto, "auto"},       {HardwareDecoder::VideoToolbox, "videotoolbox"},
          {HardwareDecoder::VAAPI, "vaapi"},     {HardwareDecoder::CUDA, "cuda"},
          {HardwareDecoder::D3D12VA, "d3d12va"}, {HardwareDecoder::D3D11VA, "d3d11va"},
          {HardwareDecoder::DXVA2, "dxva2"},
      };
      auto it = hw_names.find(args.hardware_decoder);
      const char *hardware_decoder = (it != hw_names.end()) ? it->second : "unknown";
      Logger->warn("Hardware decoder {} is requested but NOT USED for: {}. "
                   "These video streams may use unsupported codecs.",
                   hardware_decoder, codec_list);
    }
  }

  // Resolve high-dpi mode: auto → bool
  bool high_dpi;
  if (args.high_dpi_mode == HighDpiMode::Yes) {
    high_dpi = true;
  } else if (args.high_dpi_mode == HighDpiMode::No) {
    high_dpi = false;
  } else {
    // Auto: enable high-dpi if display has high pixel density and video exceeds logical display resolution
    float pixel_density = Window::GetDisplayPixelDensity(display_id, true);
    if (pixel_density > 1.0f) {
      int display_w, display_h;
      Window::GetDisplayResolution(display_w, display_h, display_id, false);
      high_dpi = player.Width() > display_w || player.Height() > display_h;
      if (high_dpi) {
        Logger->info("Auto-enabling high-dpi mode: video {}x{} exceeds display {}x{} (pixel density: {:.1f})",
                     player.Width(), player.Height(), display_w, display_h, pixel_density);
      }
    } else {
      high_dpi = false;
    }
  }

  std::string title = generate_title(args, ids);
  if (DisplayMode::Grid == args.display_mode) {
    update_window_and_grid_size(args.win_w, args.win_h, args.grid_w, args.grid_h, player.Width(), player.Height(),
                                ids.size(), display_id, high_dpi);
  } else {
    if (args.win_w <= 0 || args.win_h <= 0) {
      args.win_w = player.Width();
      args.win_h = player.Height();
    }
  }
  Window window(title, args.win_w, args.win_h, args.log_level, args.colorspace_hint, high_dpi, display_id,
                args.full_screen);
  window.InitRender(args.display_mode, args.scale_method, args.plane_scale_method);
  window.SetTargetDisplayNits(args.target_display_nits, args.target_display_set);

  // Load ICC profiles
  for (const auto &spec : args.icc_profiles) {
    // Parse "N:path" or "path" format
    size_t colon_pos = spec.find(':');
    if (colon_pos != std::string::npos && colon_pos > 0 && colon_pos < spec.size() - 1) {
      // Check if all characters before the colon are digits
      std::string prefix = spec.substr(0, colon_pos);
      bool all_digits = std::all_of(prefix.begin(), prefix.end(), ::isdigit);
      if (all_digits) {
        int video_id = std::stoi(prefix);
        std::string path = spec.substr(colon_pos + 1);
        Logger->info("Loading ICC profile '{}' for video {}", path, video_id);
        window.LoadICCProfile(path, video_id);
        continue;
      }
    }
    // Global ICC profile (including "auto" and plain paths)
    Logger->info("Loading ICC profile '{}' globally", spec);
    window.LoadICCProfile(spec, std::nullopt);
  }

  if (args.sdr_white_on_hdr > 0) {
    window.SetSdrWhiteOnHdr(args.sdr_white_on_hdr);
  }
  window.SetMainSource(args.main_id);

  struct DisplayState state = {
      .show_ref = false,
      .ids = ids,
      .amplify = args.amplify,
      .window_attached = true,
      .scale = 1.,
      .offset_x = 0.,
      .offset_y = 0.,
      .fill =
          {
              .id = ids[0],
          },
      .slide =
          {
              .split_pos = 0.5,
          },
      .grid =
          {
              .cols = args.grid_w,
              .rows = args.grid_h,
          },
  };
  if (args.ref_id.has_value()) {
    state.ref_id = args.ref_id.value();
  }
  if (DisplayMode::Slide == args.display_mode) {
    state.slide.left_id = ids[0];
    state.slide.right_id = ids[1];
  }

  // In fullscreen the window no longer matches the video resolution, so fit the video into it explicitly
  if (args.full_screen) {
    fit_to_window(state, player, window, args.display_mode);
  }

  std::atomic<bool> need_refresh = false;
  float seek_offset_s = 0.0;

  // Digit key modifier state for single-video operations
  struct DigitKeyState {
    bool held = false;
    bool used_as_modifier = false;
  } digit_keys[10];

  // Returns all held digit keys that have valid video sources
  auto get_held_digits = [&digit_keys, &player]() -> std::vector<int> {
    std::vector<int> held_ids;
    for (int i = 0; i < 10; i++) {
      if (digit_keys[i].held && player.HasVideoSource(i)) {
        held_ids.push_back(i);
      }
    }
    return held_ids;
  };
  std::string title_;
  // The snapshot currently fed to the renderers; Ctrl+S / Ctrl+E use its serials
  // so they match what the user sees. Owned by this (UI) thread.
  std::shared_ptr<const FrameSnapshot> shown_snapshot;
  // Set when the player reports an unrecoverable failure; the main loop then
  // leaves through the normal cleanup path with a non-zero exit code.
  bool fatal_exit = false;
  int exit_code = 0;
  std::string fatal_message;
  // When a rendered-frame save is pending, the next render must consume the
  // save request against the frames the user is looking at. Set on Ctrl+E and
  // cleared after that render, so no new snapshot is fed in between.
  bool hold_snapshot_for_save = false;

  // Player-thread notifications consumed by the UI thread. No callback mutates
  // DisplayState/title from the decode side.
  auto consume_player_events = [&]() {
    for (auto &outcome : player.TakeControlOutcomes()) {
      const char *status = "unknown";
      switch (outcome.status) {
      case ControlStatus::Ok:
        status = "ok";
        break;
      case ControlStatus::Boundary:
        status = "boundary";
        break;
      case ControlStatus::Failed:
        status = "failed";
        break;
      case ControlStatus::Cancelled:
        status = "cancelled";
        break;
      case ControlStatus::Rejected:
        status = "rejected";
        break;
      }
      if (outcome.status == ControlStatus::Failed || outcome.status == ControlStatus::Rejected) {
        Logger->error("Control request {} {}: {}", outcome.request_id, status, outcome.message);
      } else {
        Logger->debug("Control request {} {}: {}", outcome.request_id, status, outcome.message);
      }
      // A failed format fallback leaves the renderer without a usable mapping;
      // stop instead of continuing to feed frames the renderer rejected.
      if (outcome.kind == ControlKind::RequestFormatFallback && outcome.status == ControlStatus::Failed) {
        Logger->critical("Texture format fallback failed: {}", outcome.message);
        fatal_exit = true;
        exit_code = 1;
      }
    }
    if (player.ConsumeFatalError(&fatal_message)) {
      Logger->critical("Player stopped after an unrecoverable error: {}", fatal_message);
      fatal_exit = true;
      exit_code = 1;
    }
    if (uint32_t flickers = player.ConsumeFlickerEvents(); flickers > 0) {
      if (flickers % 2 == 1) {
        state.show_ref = !state.show_ref;
      }
      need_refresh = true;
    }
    if (player.ConsumeResolutionChanged()) {
      SDL_Event resize_event;
      resize_event.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
      SDL_PushEvent(&resize_event);
    }
  };

  player.SetFlickerInterval(args.flicker_interval);

  player.Start(true);

  // After all initialization, process accumulated system events and re-activate the window
  // Fix macOS issue where the window loses focus after lengthy initialization
  SDL_PumpEvents();
  window.Raise();
  EYEQ::ActivateApp();

  SDL_Event event;
  SDL_Keymod keymod;
  while (true) {
    SDL_PumpEvents();
    while (!SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kUpdateIntervalMS));

      consume_player_events();
      if (fatal_exit) {
        goto exit;
      }

      // Feed the newest publication (coalesced single slot) and derive title/OSD
      // from the same snapshot, so no cursor is queried after the frames arrived.
      // While a save is pending, skip feeding so Render() saves exactly the
      // frames named by the user's Ctrl+E.
      if (!hold_snapshot_for_save) {
        if (auto snapshot = player.TakeSnapshot()) {
          auto failed_ids = window.FeedFrames(snapshot->frames);
          if (!failed_ids.empty()) {
            // The fallback runs through the player's control path (rebuild filter
            // graph + re-seek); a failure is reported as a control outcome.
            for (int id : failed_ids) {
              Logger->warn("Video #{} ({}): texture format incompatible, requesting fallback", id, args.videos[id]);
            }
            player.RequestFormatFallback(failed_ids);
          } else {
            shown_snapshot = snapshot;
            std::string suffix = " ";
            if (DisplayMode::Fill == args.display_mode) {
              if (auto it = snapshot->metadata.find(state.fill.id); it != snapshot->metadata.end()) {
                suffix += it->second.pict_type;
              }
            }
            if (auto main_it = snapshot->metadata.find(args.main_id); main_it != snapshot->metadata.end()) {
              title_ = title + " @ No." + std::to_string(main_it->second.frame_serial) + " frame" + suffix;
            }
            state.osd_infos.clear();
            for (const auto &[id, info] : snapshot->metadata) {
              OsdInfo osd;
              osd.filename = std::filesystem::path(args.videos[id]).filename().string();
              osd.video_id = id;
              osd.pict_type = info.pict_type;
              osd.current_time_s = snapshot->current_time_s;
              osd.frame_serial = info.frame_serial;
              osd.total_frames = info.total_frames;
              state.osd_infos[id] = osd;
            }
            float duration = snapshot->end_time_s - snapshot->start_time_s;
            state.total_duration_s = duration;
            if (duration > 0) {
              state.playback_progress = (snapshot->current_time_s - snapshot->start_time_s) / duration;
              state.playback_progress = std::clamp(state.playback_progress, 0.0f, 1.0f);
            }
            state.playback_fps = snapshot->playback_fps;
            state.main_fps = snapshot->main_fps;
            need_refresh = true;
          }
        }
      }

      if (need_refresh) {
        // Update time corresponding to mouse position (for OSD bottom-right display)
        if (state.show_osd && state.total_duration_s > 0) {
          float mx, my;
          SDL_GetMouseState(&mx, &my);
          float progress = mx / static_cast<float>(window.Width());
          progress = std::clamp(progress, 0.0f, 1.0f);
          state.mouse_time_s = progress * state.total_duration_s;
        } else {
          state.mouse_time_s = -1.0f;
        }
        window.Render(state);
        hold_snapshot_for_save = false; // the pending rendered-frame save was consumed
        window.SetTitle(title_);
        need_refresh = false;
      }
      SDL_PumpEvents();
    }

    keymod = SDL_GetModState();
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
      if (SDLK_0 <= event.key.key && event.key.key <= SDLK_9) {
        int id = event.key.key - SDLK_0;
        if (!event.key.repeat) {
          Logger->debug("{} key pressed, marking as held", id);
          digit_keys[id].held = true;
          digit_keys[id].used_as_modifier = false;
        }
        break; // Do NOT switch video on KEY_DOWN; handled on KEY_UP
      }
      switch (event.key.key) {
      case SDLK_R:
        if (!state.ref_id.has_value()) {
          continue;
        }
        if (state.show_ref) {
          continue;
        }
        Logger->debug("r key pressed, display ref video");
        state.show_ref = true;
        need_refresh = true;
        break;
      case SDLK_S:
        if (keymod & SDL_KMOD_CTRL) {
          Logger->debug("ctrl+s key pressed, save frames");
          if (shown_snapshot) {
            save_snapshot_frames(args, *shown_snapshot);
          } else {
            Logger->warn("ctrl+s ignored: no displayed frame yet");
          }
        }
        break;
      case SDLK_E:
        if (keymod & SDL_KMOD_CTRL) {
          Logger->debug("ctrl+e key pressed, save rendered frame");
          const int id = args.main_id;
          std::filesystem::path path;
          std::filesystem::path src(args.videos[id]);
          if (args.save_in_source) {
            path = src.parent_path();
          }
          int serial = 0;
          if (shown_snapshot) {
            if (auto it = shown_snapshot->metadata.find(id); it != shown_snapshot->metadata.end()) {
              serial = it->second.frame_serial;
            }
          }
          path /= fmt::format("render_{:04d}.{}.{}.{}", serial, id, src.filename().string(), args.save_render_format);
          window.RequestSaveRenderedFrame(path);
          // Freeze snapshot feeding until the next Render() consumes the save
          // request, so the saved bytes are the frames named above.
          hold_snapshot_for_save = true;
          need_refresh = true; // force one render pass to consume the save request
        }
        break;
      case SDLK_T:
        if (DisplayMode::Slide == args.display_mode) {
          Logger->debug("t key pressed, swap the left and right videos");
          state.slide.left_id = ids[1];
          state.slide.right_id = ids[0];
          need_refresh = true;
        }
        break;
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      // Activate window on click if it doesn't have keyboard focus
      if (!window.HasInputFocus()) {
        EYEQ::ActivateApp();
        window.Raise();
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (event.motion.state & SDL_BUTTON_MMASK) {
        state.offset_x += event.motion.xrel * window.GetDisplayPixelDensity();
        state.offset_y += event.motion.yrel * window.GetDisplayPixelDensity();
        state.offset_x = align_to_pixel_edge(state.offset_x, state.scale);
        state.offset_y = align_to_pixel_edge(state.offset_y, state.scale);
        state.window_attached = false;
        Logger->trace("offset x: {}, y: {}", state.offset_x, state.offset_y);
        need_refresh = true;
      } else if (DisplayMode::Slide == args.display_mode) {
        float split_pos = align_to_pixel_edge(event.motion.x, state.scale) / static_cast<float>(window.Width());
        state.slide.split_pos = split_pos;
        Logger->trace("split_pos changed to {:.2f}", state.slide.split_pos);
        need_refresh = true;
      } else if (state.show_osd) {
        // Refresh OSD bottom-right time display on mouse move
        need_refresh = true;
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL: {
      float new_scale, ratio_rel;
      if (SDL_MOUSEWHEEL_NORMAL == event.wheel.direction) {
        ratio_rel = event.wheel.y / 20.0;
      } else {
        ratio_rel = -event.wheel.y / 20.0;
      }
      new_scale = state.scale * (1 + ratio_rel);
      new_scale = FFMIN(FFMAX(new_scale, 0.01), 20.0 * window.GetDisplayPixelDensity());
      if (new_scale == state.scale) {
        break;
      }

      if (DisplayMode::Grid == args.display_mode) {
        float mouse_x, mouse_y;
        SDL_GetMouseState(&mouse_x, &mouse_y);
        mouse_x *= window.GetDisplayPixelDensity();
        mouse_y *= window.GetDisplayPixelDensity();

        // Calculate grid cell layout
        float line_width = kLineWidth * window.GetDisplayPixelDensity();
        float cell_width = (window.PixelWidth() - (state.grid.cols - 1) * line_width) / state.grid.cols;
        float cell_height = (window.PixelHeight() - (state.grid.rows - 1) * line_width) / state.grid.rows;

        // Determine which grid cell the mouse is in
        int col = 0, row = 0;
        float accum = 0;
        for (int c = 0; c < state.grid.cols; c++) {
          float next = accum + cell_width;
          if (mouse_x < next || c == state.grid.cols - 1) {
            col = c;
            break;
          }
          accum = next + line_width;
        }
        accum = 0;
        for (int r = 0; r < state.grid.rows; r++) {
          float next = accum + cell_height;
          if (mouse_y < next || r == state.grid.rows - 1) {
            row = r;
            break;
          }
          accum = next + line_width;
        }

        // Mouse position relative to the cell
        float mouse_in_cell_x = mouse_x - col * (cell_width + line_width);
        float mouse_in_cell_y = mouse_y - row * (cell_height + line_width);

        // Zoom centered on the mouse position within the cell
        state.offset_x = mouse_in_cell_x - (mouse_in_cell_x - state.offset_x) * new_scale / state.scale;
        state.offset_y = mouse_in_cell_y - (mouse_in_cell_y - state.offset_y) * new_scale / state.scale;
        state.scale = new_scale;
        state.offset_x = align_to_pixel_edge(state.offset_x, state.scale);
        state.offset_y = align_to_pixel_edge(state.offset_y, state.scale);
        state.window_attached = false;
        Logger->debug("grid scale changed to {:.2f}, cell ({}, {}), offset ({:.2f}, {:.2f})", state.scale, col, row,
                      state.offset_x, state.offset_y);
        need_refresh = true;
        break;
      }

      // Zoom centered on the mouse position
      float mouse_x, mouse_y;
      SDL_GetMouseState(&mouse_x, &mouse_y);
      mouse_x *= window.GetDisplayPixelDensity();
      mouse_y *= window.GetDisplayPixelDensity();
      state.offset_x = mouse_x - (mouse_x - state.offset_x) * new_scale / state.scale;
      state.offset_y = mouse_y - (mouse_y - state.offset_y) * new_scale / state.scale;
      state.scale = new_scale;
      state.offset_x = align_to_pixel_edge(state.offset_x, state.scale);
      state.offset_y = align_to_pixel_edge(state.offset_y, state.scale);
      state.slide.split_pos = align_to_pixel_edge(mouse_x, state.scale) / static_cast<float>(window.Width());
      state.window_attached = false;
      Logger->debug("scale changed to {:.2f}, mouse changed to ({:.2f}, {:.2f}), offset changed to ({:.2f}, {:.2f})",
                    state.scale, mouse_x, mouse_y, state.offset_x, state.offset_y);
      need_refresh = true;
    } break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      Logger->debug("Window pixel size changed {}x{}", event.window.data1, event.window.data2);
      need_refresh = window.OnResized();
      if (state.window_attached) {
        fit_to_window(state, player, window, args.display_mode);
        need_refresh = true;
      }
      break;
    case SDL_EVENT_QUIT:
      goto exit;
    case SDL_EVENT_KEY_UP:
      if (event.key.key == SDLK_ESCAPE) {
        // mpv-style: Esc leaves fullscreen first, a second press quits
        if (window.IsFullscreen()) {
          Logger->debug("esc key released, leave fullscreen");
          window.SetFullscreen(false);
          need_refresh = true;
          break;
        }
        goto exit;
      }
      if (event.key.key == SDLK_Q) {
        goto exit;
      }
      // Handle digit key release: switch video only if not used as modifier
      if (SDLK_0 <= event.key.key && event.key.key <= SDLK_9) {
        int id = event.key.key - SDLK_0;
        if (digit_keys[id].held && !digit_keys[id].used_as_modifier) {
          Logger->debug("{} key released (not modifier), switch to video {}", id, id);
          if (player.HasVideoSource(id)) {
            state.fill.id = id;
            need_refresh = true;
          } else {
            Logger->warn("{} is not a valid video ID", id);
          }
        }
        digit_keys[id].held = false;
        digit_keys[id].used_as_modifier = false;
        break;
      }
      switch (event.key.key) {
      case SDLK_A: {
        auto held = get_held_digits();
        if (!held.empty()) {
          for (int id : held) {
            Logger->debug("{} + A: step video {} to previous frame", id, id);
            digit_keys[id].used_as_modifier = true;
            player.StepPrevFrameSingle(id);
          }
        } else {
          Logger->debug("a key released, step to previous frame");
          player.StepPrevFrame();
        }
        break;
      }
      case SDLK_D: {
        auto held = get_held_digits();
        if (!held.empty()) {
          for (int id : held) {
            Logger->debug("{} + D: step video {} to next frame", id, id);
            digit_keys[id].used_as_modifier = true;
            player.StepNextFrameSingle(id);
          }
        } else {
          Logger->debug("d key released, step to next frame");
          player.StepNextFrame();
        }
        break;
      }
      case SDLK_R:
        if (!state.ref_id.has_value()) {
          break;
        }
        Logger->debug("r key released, hide ref video");
        state.show_ref = false;
        need_refresh = true;
        break;
      case SDLK_T:
        if (DisplayMode::Slide == args.display_mode) {
          Logger->debug("t key released, resumes the left and right videos back");
          state.slide.left_id = ids[0];
          state.slide.right_id = ids[1];
          need_refresh = true;
        }
        break;
      case SDLK_X:
        Logger->debug("x key released, force refresh");
        need_refresh = true;
        break;
      case SDLK_F:
        Logger->debug("f key released, toggle fullscreen");
        window.SetFullscreen(!window.IsFullscreen());
        need_refresh = true;
        break;
      case SDLK_Z:
        Logger->debug("z key released, fit to window");
        state.window_attached = true;
        fit_to_window(state, player, window, args.display_mode);
        need_refresh = true;
        break;
      case SDLK_SPACE:
        Logger->debug("space key pressed, switch pause state");
        player.InvertPause();
        break;
      case SDLK_F5: {
        auto held = get_held_digits();
        if (!held.empty()) {
          for (int id : held) {
            Logger->debug("{} + F5: reset offset for video {}", id, id);
            digit_keys[id].used_as_modifier = true;
            player.ResetVideoOffset(id);
          }
        } else {
          Logger->debug("F5: reset all video offsets");
          player.ResetAllVideoOffsets();
        }
        need_refresh = true;
        break;
      }
      case SDLK_I:
        Logger->debug("i key released, toggle OSD");
        state.show_osd = !state.show_osd;
        need_refresh = true;
        break;
      case SDLK_S:
        if (keymod & SDL_KMOD_SHIFT) {
          // Shift+S: seek to relative time at mouse X position
          if (!shown_snapshot) {
            break;
          }
          float mouse_x, mouse_y;
          SDL_GetMouseState(&mouse_x, &mouse_y);
          float progress = mouse_x / static_cast<float>(window.Width());
          float start = shown_snapshot->start_time_s;
          float end = shown_snapshot->end_time_s;
          float target = start + progress * (end - start);
          Logger->debug("Shift+S: seek to {:.2f}s (progress {:.2f})", target, progress);
          player.SeekTo(target);
        }
        break;
      case SDLK_HOME:
        seek_offset_s = -10000;
        goto do_seek;
      case SDLK_END:
        seek_offset_s = 10000;
        goto do_seek;
      case SDLK_PAGEUP:
        seek_offset_s = -60;
        goto do_seek;
      case SDLK_PAGEDOWN:
        seek_offset_s = 60;
        goto do_seek;
      case SDLK_LEFT:
        seek_offset_s = -1;
        goto do_seek;
      case SDLK_RIGHT:
        seek_offset_s = 1;
        goto do_seek;
      case SDLK_UP:
        seek_offset_s = -5;
        goto do_seek;
      case SDLK_DOWN:
        seek_offset_s = 5;
        goto do_seek;
      case SDLK_LEFTBRACKET:
        if (keymod & SDL_KMOD_SHIFT) {
          seek_offset_s = -10000;
        } else {
          seek_offset_s = -60;
        }
        goto do_seek;
      case SDLK_RIGHTBRACKET:
        if (keymod & SDL_KMOD_SHIFT) {
          seek_offset_s = 10000;
        } else {
          seek_offset_s = 60;
        }
      do_seek:
        {
          auto held = get_held_digits();
          if (!held.empty()) {
            for (int id : held) {
              Logger->debug("{} + seek: video {} seek offset {:.0f}s", id, id, seek_offset_s);
              digit_keys[id].used_as_modifier = true;
              player.SeekOffsetSingle(id, seek_offset_s);
            }
          } else {
            player.SeekOffset(seek_offset_s);
          }
          break;
        }
      }
      break;
    }
  }

exit:
  // Drop the UI-held snapshot before stopping/releasing the sources: a snapshot
  // keeps AVFrame references whose opaque points at the VideoSource streams.
  shown_snapshot.reset();
  player.Stop();

  SDL_Quit();
  return exit_code;
}
