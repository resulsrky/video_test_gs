// Utility helpers: CLI parsing and system analysis
#pragma once

#include <optional>
#include <string>

namespace ve {

struct PortsConfig {
  // Mapping:
  // p1: RTP primary (remote)
  // p2: RTP FEC (remote)
  // p3: RTCP send (remote)
  // p4: RTCP receive (local)
  int rtp_port = 5000;
  int fec_port = 5001;
  int rtcp_send_port = 5002;
  int rtcp_recv_port = 5003;
};

struct VideoProfile {
  int width = 1280;
  int height = 720;
  int fps = 30;
  int bitrate_kbps = 4000;  // encoder target
};

struct EngineConfig {
  std::string dest_ip;
  PortsConfig ports;
  VideoProfile profile;
  std::string source = "ximagesrc";  // or v4l2src/videotestsrc
  int fec_percentage = 20;            // redundancy, aims to tolerate ~5% loss
  std::string mode = "rtpbin";       // rtpbin | simple
  int latency_ms = 50;                // target sender latency hint
};

// Parse CLI of form:
//   video_engine <ip> <p1> <p2> <p3> <p4> [--source=] [--width=] [--height=]
//                                     [--fps=] [--bitrate=] [--fec=] [--mode=]
//                                     [--latency=]
// Returns std::nullopt and prints help on failure.
std::optional<EngineConfig> parse_args(int argc, char** argv);

// Lightweight system analysis to pick an initial profile.
VideoProfile auto_select_profile();

bool is_valid_ip(const std::string& ip);
bool is_valid_port(int p);

}  // namespace ve
