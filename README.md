# video_engine

A low-latency RTP video sender built with GStreamer and C++.

## Features

- RTP/H.264 streaming pipeline with optional `rtpbin` topology for RTCP and ULPFEC.
- Automatic system profiling to select an initial resolution, framerate, and bitrate.
- Configurable latency target that caps internal buffering (~50 ms by default).
- Forward error correction (FEC) with tunable redundancy to tolerate packet loss.
- QoS controller that monitors RTCP statistics and adapts encoder bitrate based on loss.
- Command-line configuration for destination IP, port bundle, source, and encoder settings.

## Building

The project uses CMake and requires GStreamer 1.20+ development packages:

```bash
cmake -S . -B build
cmake --build build
```

If `cmake` fails because GStreamer headers or pkg-config files are missing, install the
appropriate packages for your distribution (for example on Debian/Ubuntu:
`sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev`).

## Usage

```
./video_engine <dest_ip> <rtp_port> <fec_port> <rtcp_send_port> <rtcp_recv_port> [options]
```

Key options:

- `--source=ximagesrc|v4l2src|videotestsrc`
- `--width=<int>` `--height=<int>` `--fps=<int>` `--bitrate=<kbps>`
- `--fec=<percentage>` controls ULPFEC redundancy (default 20)
- `--mode=rtpbin|simple` selects between the RTCP-enabled sender or a tee+FEC topology
- `--latency=<ms>` adjusts the sender side buffering budget (clamped to 10-200 ms)

Example:

```
./video_engine 192.168.1.50 5000 5001 5002 5003 --source=v4l2src --bitrate=6000 --fec=25
```

## Runtime notes

- The main pipeline is `source -> videoconvert -> videoscale -> videorate -> capsfilter -> queue -> x264enc -> h264parse -> rtph264pay`.
- Queues are configured to leak downstream with a time window derived from the latency target to keep end-to-end delay low.
- In `rtpbin` mode the payloader connects into `rtpbin`, which handles RTCP, RTP retransmission caps, and FEC fan-out.
- In `simple` mode a `tee` drives dedicated queues for RTP and FEC branches using `rtpulpfecenc`.
- The QoS controller periodically inspects `rtpbin` stats and nudges the encoder bitrate up/down when loss crosses thresholds.
