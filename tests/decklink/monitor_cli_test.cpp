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
  EYEQ::VideoConnection connection;
  EYEQ::PixelFormat format;
  EYEQ::MonitorColorTarget target;
  std::string error;
  Check(EYEQ::ParseMonitorConnection("sdi", &connection, &error) && connection == EYEQ::VideoConnection::SDI,
        "sdi is accepted");
  Check(EYEQ::ParseMonitorConnection("hdmi", &connection, &error) && connection == EYEQ::VideoConnection::HDMI,
        "hdmi is accepted");
  Check(!EYEQ::ParseMonitorConnection("displayport", &connection, &error), "an unknown connection is rejected");
  Check(EYEQ::ParseMonitorPixelFormat("v210", &format, &error) && format == EYEQ::PixelFormat::V210,
        "v210 is accepted");
  Check(EYEQ::ParseMonitorPixelFormat("r210", &format, &error) && format == EYEQ::PixelFormat::R210,
        "r210 is accepted");
  Check(!EYEQ::ParseMonitorPixelFormat("yuv422", &format, &error), "an unknown pixel format is rejected");
  Check(EYEQ::ParseMonitorColorTarget("sdr709", &target, &error) && target == EYEQ::MonitorColorTarget::SdrBt709,
        "sdr709 is accepted");
  Check(EYEQ::ParseMonitorColorTarget("pq2020", &target, &error) && target == EYEQ::MonitorColorTarget::PqBt2020,
        "pq2020 is accepted");
  Check(!EYEQ::ParseMonitorColorTarget("hdr10", &target, &error), "an unknown color target is rejected");
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
  TestSelectMode();
  TestBuildRequest();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
