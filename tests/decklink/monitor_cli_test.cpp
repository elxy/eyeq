// CPU-only tests for the strict monitoring CLI parsing/validation. No SDK, no GPU,
// no device: every rejection path is exercised directly.

#include <cstdio>
#include <string>
#include <vector>

#include "output/monitor_cli.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string &what) {
  ++g_checks;
  if (condition) {
    std::printf("  ok   %s\n", what.c_str());
  } else {
    std::fprintf(stderr, "  FAIL %s\n", what.c_str());
    ++g_failures;
  }
}

void TestDeviceSelector() {
  std::printf("[device selector]\n");
  EYEQ::DeviceId selector;
  std::string error;
  Check(EYEQ::ParseDeviceSelector("index:3", &selector, &error) && selector.index == 3 &&
            selector.persistent_id.empty(),
        "index:3 selects by index");
  Check(EYEQ::ParseDeviceSelector("id:4294967296", &selector, &error) && selector.persistent_id == "4294967296" &&
            selector.index == -1,
        "id:... selects by persistent id");
  Check(!EYEQ::ParseDeviceSelector("3", &selector, &error), "a bare number is rejected");
  Check(!EYEQ::ParseDeviceSelector("index:", &selector, &error), "an empty value is rejected");
  Check(!EYEQ::ParseDeviceSelector("index:-1", &selector, &error), "a negative index is rejected");
  Check(!EYEQ::ParseDeviceSelector("index:1x", &selector, &error), "a trailing character is rejected");
  Check(!EYEQ::ParseDeviceSelector("port:0", &selector, &error), "an unknown selector kind is rejected");
}

void TestMonitorMode() {
  std::printf("[monitor mode]\n");
  EYEQ::MonitorMode mode;
  std::string error;
  Check(EYEQ::ParseMonitorMode("3840x2160@50", &mode, &error) && mode.width == 3840 && mode.height == 2160 &&
            mode.time_scale == 50 && mode.frame_duration == 1 && mode.is_progressive,
        "3840x2160@50 parses to exact integer data");
  Check(EYEQ::ParseMonitorMode("3840x2160@30000/1001", &mode, &error) && mode.time_scale == 30000 &&
            mode.frame_duration == 1001,
        "30000/1001 keeps the exact rational without rounding");
  Check(!EYEQ::ParseMonitorMode("3840x2160", &mode, &error), "a missing rate is rejected");
  Check(!EYEQ::ParseMonitorMode("3840x@50", &mode, &error), "a missing height is rejected");
  Check(!EYEQ::ParseMonitorMode("3840x2160@0", &mode, &error), "a zero rate is rejected");
  Check(!EYEQ::ParseMonitorMode("3840x2160@50.0", &mode, &error), "a floating rate is rejected");
}

void TestLabels() {
  std::printf("[connection/pixel format/color target]\n");
  EYEQ::MonitorConnectionChoice connection;
  EYEQ::PixelFormat format;
  EYEQ::MonitorColorTarget target;
  std::string error;
  Check(EYEQ::ParseMonitorConnectionChoice("auto", &connection, &error) && connection == EYEQ::MonitorConnectionChoice::Auto,
        "auto connection is accepted");
  Check(EYEQ::ParseMonitorConnectionChoice("sdi", &connection, &error) && connection == EYEQ::MonitorConnectionChoice::Sdi,
        "sdi is accepted");
  Check(EYEQ::ParseMonitorConnectionChoice("hdmi", &connection, &error) &&
            connection == EYEQ::MonitorConnectionChoice::Hdmi,
        "hdmi is accepted");
  Check(!EYEQ::ParseMonitorConnectionChoice("displayport", &connection, &error),
        "an unknown connection is rejected");
  Check(EYEQ::ParseMonitorPixelFormat("v210", &format, &error) && format == EYEQ::PixelFormat::V210,
        "v210 is accepted");
  Check(EYEQ::ParseMonitorPixelFormat("r210", &format, &error) && format == EYEQ::PixelFormat::R210,
        "r210 is accepted");
  Check(!EYEQ::ParseMonitorPixelFormat("yuv422", &format, &error), "an unknown pixel format is rejected");
  Check(EYEQ::ParseMonitorColorTarget("auto", &target, &error) && target == EYEQ::MonitorColorTarget::Auto,
        "auto color target is accepted");
  Check(EYEQ::ParseMonitorColorTarget("sdr709", &target, &error) && target == EYEQ::MonitorColorTarget::SdrBt709,
        "sdr709 is accepted");
  Check(EYEQ::ParseMonitorColorTarget("pq2020", &target, &error) && target == EYEQ::MonitorColorTarget::PqBt2020,
        "pq2020 is accepted");
  Check(!EYEQ::ParseMonitorColorTarget("hdr10", &target, &error), "an unknown color target is rejected");
}

void TestConnectionAuto() {
  std::printf("[connection auto]\n");
  std::string reason;
  std::string error;
  EYEQ::VideoConnection out;

  EYEQ::MonitorConnections both;
  both.known = true;
  both.sdi = true;
  both.hdmi = true;
  Check(EYEQ::ResolveMonitorConnection(EYEQ::MonitorConnectionChoice::Auto, both, &out, &reason, &error) &&
            out == EYEQ::VideoConnection::SDI,
        "auto prefers SDI when the device reports it");

  EYEQ::MonitorConnections hdmi_only;
  hdmi_only.known = true;
  hdmi_only.hdmi = true;
  Check(EYEQ::ResolveMonitorConnection(EYEQ::MonitorConnectionChoice::Auto, hdmi_only, &out, &reason, &error) &&
            out == EYEQ::VideoConnection::HDMI,
        "auto falls back to HDMI when only HDMI is reported");

  EYEQ::MonitorConnections unknown;
  Check(!EYEQ::ResolveMonitorConnection(EYEQ::MonitorConnectionChoice::Auto, unknown, &out, &reason, &error),
        "auto fails when the device reports no connections");

  EYEQ::MonitorConnections none;
  none.known = true;
  Check(!EYEQ::ResolveMonitorConnection(EYEQ::MonitorConnectionChoice::Auto, none, &out, &reason, &error),
        "auto fails when the device reports neither SDI nor HDMI");

  // An explicit choice never consults the mask and never falls back.
  Check(EYEQ::ResolveMonitorConnection(EYEQ::MonitorConnectionChoice::Hdmi, both, &out, &reason, &error) &&
            out == EYEQ::VideoConnection::HDMI,
        "an explicit connection is used verbatim");
}

void TestColorAuto() {
  std::printf("[color auto]\n");
  EYEQ::MonitorColorTarget out;
  std::string reason;
  Check(EYEQ::ResolveMonitorColorTarget(EYEQ::MonitorColorTarget::Auto, true, &out, &reason) &&
            out == EYEQ::MonitorColorTarget::PqBt2020,
        "auto picks pq2020 for an HDR main video");
  Check(EYEQ::ResolveMonitorColorTarget(EYEQ::MonitorColorTarget::Auto, false, &out, &reason) &&
            out == EYEQ::MonitorColorTarget::SdrBt709,
        "auto picks sdr709 for an SDR main video");
  Check(EYEQ::ResolveMonitorColorTarget(EYEQ::MonitorColorTarget::SdrBt709, true, &out, &reason) &&
            out == EYEQ::MonitorColorTarget::SdrBt709,
        "an explicit target is not overridden by auto");
}

void TestModeAuto() {
  std::printf("[mode auto]\n");
  std::vector<EYEQ::MonitorMode> modes;
  modes.push_back(EYEQ::MonitorMode{1920, 1080, 1, 24, true});
  modes.push_back(EYEQ::MonitorMode{1920, 1080, 1, 25, true});
  modes.push_back(EYEQ::MonitorMode{3840, 2160, 1, 25, true});
  EYEQ::MonitorMode explicit_mode{1, 1, 1, 1, true};
  bool is_auto = false;
  std::string error;
  Check(EYEQ::ParseMonitorModeChoice("auto", &is_auto, &explicit_mode, &error) && is_auto,
        "auto mode is recognized");
  Check(!EYEQ::ParseMonitorModeChoice("3840x2160", &is_auto, &explicit_mode, &error),
        "a malformed explicit mode still fails through auto parsing");
  Check(EYEQ::ParseMonitorModeChoice("3840x2160@25", &is_auto, &explicit_mode, &error) && !is_auto &&
            explicit_mode.width == 3840,
        "an explicit mode is parsed unchanged");

  EYEQ::MonitorMode out;
  std::string reason;
  // 1080p50 source: no 50 mode, lowest rate >= 50 is absent, so highest below (25) is used; the
  // covering whitelisted 1080p is preferred over 2160p (same rate, smaller area).
  Check(EYEQ::SelectMonitorModeForSource(modes, 1920, 1080, 50.0, &out, &reason, &error) && out.height == 1080 &&
            out.time_scale == 25,
        "auto picks the lowest rate not below the source, then the smallest covering mode");
  // Exact 25.0 for a 4K source keeps 2160p (covers) over 1080p.
  Check(EYEQ::SelectMonitorModeForSource(modes, 3840, 2160, 25.0, &out, &reason, &error) && out.width == 3840 &&
            out.height == 2160,
        "auto keeps the resolution that covers a 4K source at an exact rate");
  // 1080p source at 25 keeps 1080p (smallest covering).
  Check(EYEQ::SelectMonitorModeForSource(modes, 1280, 720, 25.0, &out, &reason, &error) && out.height == 1080,
        "auto upscales a smaller source to the smallest covering mode");
  Check(!EYEQ::SelectMonitorModeForSource(modes, 1920, 1080, 0.0, &out, &reason, &error),
        "auto fails on an unknown source frame rate");
  const std::vector<EYEQ::MonitorMode> empty;
  Check(!EYEQ::SelectMonitorModeForSource(empty, 1920, 1080, 25.0, &out, &reason, &error),
        "auto fails on an empty native list");
}

void TestSelectMode() {
  std::printf("[native mode selection]\n");
  std::vector<EYEQ::MonitorMode> modes;
  modes.push_back(EYEQ::MonitorMode{1920, 1080, 1001, 30000, true});
  modes.push_back(EYEQ::MonitorMode{3840, 2160, 1, 50, true});
  modes.push_back(EYEQ::MonitorMode{3840, 2160, 1, 25, false}); // interlaced
  EYEQ::MonitorMode out;
  std::string error;
  const EYEQ::MonitorMode requested{3840, 2160, 1, 50, true};
  Check(EYEQ::SelectMonitorMode(modes, requested, &out, &error) && out.time_scale == 50 && out.is_progressive,
        "an exactly listed mode is selected");
  const EYEQ::MonitorMode interlaced{3840, 2160, 1, 25, true};
  Check(!EYEQ::SelectMonitorMode(modes, interlaced, &out, &error), "an interlaced native mode is not selected");
  const EYEQ::MonitorMode missing{4096, 2160, 1, 50, true};
  Check(!EYEQ::SelectMonitorMode(modes, missing, &out, &error), "an unlisted mode is rejected");
  Check(error.find("available") != std::string::npos, "the rejection names the available modes");
  const std::vector<EYEQ::MonitorMode> empty;
  Check(!EYEQ::SelectMonitorMode(empty, requested, &out, &error), "an empty native list rejects every mode");
}

void TestBuildRequest() {
  std::printf("[output request]\n");
  EYEQ::DeviceId device;
  device.index = 0;
  const EYEQ::MonitorMode mode{3840, 2160, 1, 50, true};
  EYEQ::OutputRequest request;
  std::string error;
  Check(EYEQ::BuildMonitorOutputRequest(device, mode, EYEQ::VideoConnection::SDI, EYEQ::PixelFormat::V210,
                                        EYEQ::MonitorColorTarget::SdrBt709, 100.0, &request, &error) &&
            request.color.encoding.primaries == EYEQ::ColorPrimaries::BT709 &&
            request.color.encoding.transfer == EYEQ::ColorTransfer::SDR_BT1886 && !request.hdr.hdr_signaling,
        "an SDR target builds a coherent BT.709/BT.1886 request with HDR off");
  Check(EYEQ::BuildMonitorOutputRequest(device, mode, EYEQ::VideoConnection::HDMI, EYEQ::PixelFormat::R210,
                                        EYEQ::MonitorColorTarget::PqBt2020, 1000.0, &request, &error) &&
            request.color.encoding.primaries == EYEQ::ColorPrimaries::BT2020 &&
            request.color.encoding.transfer == EYEQ::ColorTransfer::PQ && request.hdr.hdr_signaling &&
            !request.hdr.mastering.has_primaries && !request.hdr.mastering.has_luminance && !request.hdr.cll.has_values,
        "a PQ target builds PQ/BT2020 signaling without fabricating mastering/CLL");
  Check(!EYEQ::BuildMonitorOutputRequest(device, mode, EYEQ::VideoConnection::SDI, EYEQ::PixelFormat::V210,
                                         EYEQ::MonitorColorTarget::SdrBt709, 0.0, &request, &error),
        "a non-positive target nits is rejected");
}

} // namespace

int main() {
  TestDeviceSelector();
  TestMonitorMode();
  TestLabels();
  TestConnectionAuto();
  TestColorAuto();
  TestModeAuto();
  TestSelectMode();
  TestBuildRequest();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
