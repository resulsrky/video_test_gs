#include "utils.h"
#include "logger.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <thread>
#include <sstream>
#include <string>
#include <vector>

namespace ve {

static std::optional<int> to_int(const char* s) {
  int v = 0;
  auto [ptr, ec] = std::from_chars(s, s + std::strlen(s), v);
  if (ec != std::errc() || ptr != s + std::strlen(s)) return std::nullopt;
  return v;
}

bool is_valid_port(int p) { return p > 0 && p < 65536; }

bool is_valid_ip(const std::string& ip) {
  int parts = 0;
  std::stringstream ss(ip);
  std::string item;
  while (std::getline(ss, item, '.')) {
    if (item.empty() || item.size() > 3) return false;
    if (!std::all_of(item.begin(), item.end(), ::isdigit)) return false;
    int v = std::stoi(item);
    if (v < 0 || v > 255) return false;
    parts++;
  }
  return parts == 4;
}

VideoProfile auto_select_profile() {
  // Basic heuristic using CPU cores and (optional) /proc/meminfo
  int cores = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  long mem_mb = 0;
  std::ifstream f("/proc/meminfo");
  if (f) {
    std::string k; long v; std::string unit;
    while (f >> k >> v >> unit) {
      if (k == "MemTotal:") { mem_mb = v / 1024; break; }
    }
  }

  VideoProfile p;
  if (cores >= 8 && mem_mb >= 8000) {
    p.width = 1920; p.height = 1080; p.fps = 60; p.bitrate_kbps = 8000;
  } else if (cores >= 4) {
    p.width = 1280; p.height = 720; p.fps = 30; p.bitrate_kbps = 4000;
  } else {
    p.width = 854; p.height = 480; p.fps = 30; p.bitrate_kbps = 1500;
  }
  LOG_INFO("Auto profile cores=", cores, " memMB=", mem_mb,
           " -> ", p.width, "x", p.height, "@", p.fps, " bitrate=", p.bitrate_kbps, "kbps");
  return p;
}

static void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " <dest_ip> <rtp_port> <fec_port> <rtcp_send_port> <rtcp_recv_port> [options]\n"
            << "  Ports: rtp primary, rtp FEC, rtcp send (remote), rtcp recv (local)\n"
            << "Options:\n"
            << "  --source=ximagesrc|v4l2src\n"
            << "  --width=<int>  --height=<int>  --fps=<int>\n"
            << "  --bitrate=<kbps>  --fec=<percentage 0-100>\n";
}

std::optional<EngineConfig> parse_args(int argc, char** argv) {
  if (argc < 6) {
    print_usage(argv[0]);
    return std::nullopt;
  }

  EngineConfig cfg;
  cfg.dest_ip = argv[1];
  if (!is_valid_ip(cfg.dest_ip)) {
    LOG_ERROR("Invalid IP: ", cfg.dest_ip);
    return std::nullopt;
  }

  auto p1 = to_int(argv[2]);
  auto p2 = to_int(argv[3]);
  auto p3 = to_int(argv[4]);
  auto p4 = to_int(argv[5]);
  if (!p1 || !p2 || !p3 || !p4 ||
      !is_valid_port(*p1) || !is_valid_port(*p2) || !is_valid_port(*p3) || !is_valid_port(*p4)) {
    LOG_ERROR("Invalid port(s)");
    return std::nullopt;
  }
  cfg.ports = { *p1, *p2, *p3, *p4 };

  cfg.profile = auto_select_profile();

  for (int i = 6; i < argc; ++i) {
    std::string a = argv[i];
    auto eat = [&](const char* key) -> std::optional<std::string> {
      std::string k = std::string(key) + "=";
      if (a.rfind(k, 0) == 0) return a.substr(k.size());
      return std::nullopt;
    };
    if (auto v = eat("--source")) cfg.source = *v;
    else if (auto v = eat("--width")) cfg.profile.width = std::stoi(*v);
    else if (auto v = eat("--height")) cfg.profile.height = std::stoi(*v);
    else if (auto v = eat("--fps")) cfg.profile.fps = std::stoi(*v);
    else if (auto v = eat("--bitrate")) cfg.profile.bitrate_kbps = std::stoi(*v);
    else if (auto v = eat("--fec")) cfg.fec_percentage = std::clamp(std::stoi(*v), 0, 100);
    else if (auto v = eat("--mode")) cfg.mode = *v;
    else {
      LOG_WARN("Unknown arg: ", a);
    }
  }

  if (cfg.source != "ximagesrc" && cfg.source != "v4l2src" && cfg.source != "videotestsrc") {
    LOG_WARN("Unsupported source '", cfg.source, "', defaulting to ximagesrc");
    cfg.source = "ximagesrc";
  }

  if (cfg.mode != "rtpbin" && cfg.mode != "simple") {
    LOG_WARN("Unsupported mode '", cfg.mode, "', defaulting to rtpbin");
    cfg.mode = "rtpbin";
  }

  return cfg;
}

}  // namespace ve
