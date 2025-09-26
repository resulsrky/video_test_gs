#include "logger.h"
#include "qos_controller.h"
#include "utils.h"

#include <gst/gst.h>
#include <gst/rtp/rtp.h>

#include <algorithm>
#include <csignal>
#include <memory>
#include <string>
#include <vector>

using namespace ve;

namespace {

GMainLoop* g_loop = nullptr;

struct PipelineElements {
  GstElement* pipeline = nullptr;
  GstElement* source = nullptr;
  GstElement* convert = nullptr;
  GstElement* scale = nullptr;
  GstElement* rate = nullptr;
  GstElement* capsfilter = nullptr;
  GstElement* queue = nullptr;
  GstElement* encoder = nullptr;
  GstElement* parser = nullptr;
  GstElement* pay = nullptr;
  GstElement* rtpbin = nullptr;
  GstElement* tee = nullptr;
  GstElement* udpsink_rtp = nullptr;
  GstElement* udpsink_fec = nullptr;
  GstElement* udpsink_rtcp = nullptr;
  GstElement* udpsrc_rtcp = nullptr;
};

GstElement* make_checked(const char* factory, const char* name) {
  GstElement* element = gst_element_factory_make(factory, name);
  if (!element) {
    LOG_ERROR("Failed to create element '", factory, "' (", name, ")");
  }
  return element;
}

void configure_source(GstElement* source, const EngineConfig& cfg) {
  if (cfg.source == "ximagesrc") {
    g_object_set(source,
                 "use-damage", FALSE,
                 "show-pointer", FALSE,
                 NULL);
  } else if (cfg.source == "v4l2src") {
    g_object_set(source,
                 "io-mode", 4,   // userptr for low latency
                 "do-timestamp", TRUE,
                 NULL);
  } else if (cfg.source == "videotestsrc") {
    g_object_set(source,
                 "is-live", TRUE,
                 "pattern", 0,
                 NULL);
  }
}

void configure_caps(GstElement* capsfilter, const VideoProfile& profile) {
  GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                      "width", G_TYPE_INT, profile.width,
                                      "height", G_TYPE_INT, profile.height,
                                      "framerate", GST_TYPE_FRACTION, profile.fps, 1,
                                      "format", G_TYPE_STRING, "I420",
                                      NULL);
  g_object_set(capsfilter, "caps", caps, NULL);
  gst_caps_unref(caps);
}

void configure_videorate(GstElement* rate, [[maybe_unused]] const VideoProfile& profile) {
  g_object_set(rate,
               "skip-to-first", TRUE,
               "drop-only", TRUE,
               "max-duplication-time", 0,
               NULL);
}

void configure_queue(GstElement* queue, int latency_ms) {
  guint64 max_time = static_cast<guint64>(std::max(latency_ms, 10)) * GST_MSECOND;
  g_object_set(queue,
               "leaky", 2,               // downstream, drop oldest
               "max-size-buffers", 0,
               "max-size-bytes", 0,
               "max-size-time", max_time,
               "min-threshold-time", max_time / 2,
               NULL);
}

void configure_encoder(GstElement* encoder, const VideoProfile& profile) {
  g_object_set(encoder,
               "tune", 0x00000004,          // zerolatency
               "speed-preset", 1,          // ultrafast
               "key-int-max", profile.fps * 2,
               "bitrate", profile.bitrate_kbps,
               "byte-stream", TRUE,
               "bframes", 0,
               "option-string", "repeat-headers=1",
               NULL);
}

void configure_payloader(GstElement* pay) {
  g_object_set(pay,
               "pt", 96,
               "config-interval", 1,
               "mtu", 1200,
               NULL);
}

void configure_sink(GstElement* sink, const std::string& host, int port) {
  g_object_set(sink,
               "host", host.c_str(),
               "port", port,
               "ttl", 64,
               "sync", FALSE,
               "async", FALSE,
               "qos", TRUE,
               "buffer-size", 0,
               NULL);
}

struct PadLinkCtx {
  GstElement* udpsink_rtp;
  GstElement* udpsink_fec;
};

void attach_rtpbin_links(GstElement* rtpbin, GstElement* pay,
                         GstElement* udpsink_rtp, GstElement* udpsink_fec,
                         GstElement* udpsink_rtcp, GstElement* udpsrc_rtcp) {
  GstPad* pay_src = gst_element_get_static_pad(pay, "src");
  GstPad* rtp_sink = gst_element_get_request_pad(rtpbin, "send_rtp_sink_0");
  if (!pay_src || !rtp_sink || gst_pad_link(pay_src, rtp_sink) != GST_PAD_LINK_OK) {
    LOG_ERROR("Failed to link payloader to rtpbin send sink");
  }
  if (pay_src) gst_object_unref(pay_src);
  if (rtp_sink) gst_object_unref(rtp_sink);

  PadLinkCtx* ctx = g_new0(PadLinkCtx, 1);
  ctx->udpsink_rtp = udpsink_rtp;
  ctx->udpsink_fec = udpsink_fec;

  g_signal_connect_data(
      rtpbin, "pad-added",
      G_CALLBACK(+[](GstElement*, GstPad* new_pad, gpointer user_data) {
        auto* c = static_cast<PadLinkCtx*>(user_data);
        const gchar* name = GST_PAD_NAME(new_pad);
        if (g_str_has_prefix(name, "send_rtp_src_0")) {
          GstPad* sinkpad = gst_element_get_static_pad(c->udpsink_rtp, "sink");
          if (gst_pad_link(new_pad, sinkpad) == GST_PAD_LINK_OK) {
            LOG_INFO("Linked ", name, " -> udpsink_rtp");
          } else {
            LOG_ERROR("Failed to link ", name, " -> udpsink_rtp");
          }
          gst_object_unref(sinkpad);
        } else if (g_str_has_prefix(name, "send_fec_src_0")) {
          GstPad* sinkpad = gst_element_get_static_pad(c->udpsink_fec, "sink");
          if (gst_pad_link(new_pad, sinkpad) == GST_PAD_LINK_OK) {
            LOG_INFO("Linked ", name, " -> udpsink_fec");
          } else {
            LOG_ERROR("Failed to link ", name, " -> udpsink_fec");
          }
          gst_object_unref(sinkpad);
        }
      }),
      ctx,
      [](gpointer data) { g_free(data); },
      static_cast<GConnectFlags>(0));

  GstPad* rtcp_src = gst_element_get_request_pad(rtpbin, "send_rtcp_src_0");
  GstPad* rtcp_sinkpad = gst_element_get_static_pad(udpsink_rtcp, "sink");
  if (!rtcp_src || !rtcp_sinkpad || gst_pad_link(rtcp_src, rtcp_sinkpad) != GST_PAD_LINK_OK) {
    LOG_ERROR("Failed to link RTCP send pad to udpsink_rtcp");
  }
  if (rtcp_src) gst_object_unref(rtcp_src);
  if (rtcp_sinkpad) gst_object_unref(rtcp_sinkpad);

  GstPad* udpsrc_pad = gst_element_get_static_pad(udpsrc_rtcp, "src");
  GstPad* rtpbin_rtcp_sink = gst_element_get_request_pad(rtpbin, "recv_rtcp_sink_0");
  if (!udpsrc_pad || !rtpbin_rtcp_sink || gst_pad_link(udpsrc_pad, rtpbin_rtcp_sink) != GST_PAD_LINK_OK) {
    LOG_ERROR("Failed to link incoming RTCP to rtpbin");
  }
  if (udpsrc_pad) gst_object_unref(udpsrc_pad);
  if (rtpbin_rtcp_sink) gst_object_unref(rtpbin_rtcp_sink);
}

gboolean bus_call(GstBus*, GstMessage* msg, gpointer) {
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      LOG_ERROR("GStreamer error: ", (err ? err->message : "(unknown)"));
      if (dbg) { LOG_DEBUG("Debug: ", dbg); g_free(dbg); }
      if (err) g_error_free(err);
      if (g_loop) g_main_loop_quit(g_loop);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_warning(msg, &err, &dbg);
      LOG_WARN("GStreamer warning: ", (err ? err->message : "(unknown)"));
      if (dbg) { LOG_DEBUG("Debug: ", dbg); g_free(dbg); }
      if (err) g_error_free(err);
      break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
      GstState old_s, new_s, pending_s;
      gst_message_parse_state_changed(msg, &old_s, &new_s, &pending_s);
      const gchar* name = GST_OBJECT_NAME(GST_MESSAGE_SRC(msg));
      LOG_DEBUG("State changed: ", (name ? name : "(unknown)"), " ",
                gst_element_state_get_name(old_s), " -> ",
                gst_element_state_get_name(new_s));
      break;
    }
    case GST_MESSAGE_EOS:
      LOG_INFO("Pipeline reached EOS");
      if (g_loop) g_main_loop_quit(g_loop);
      break;
    default:
      break;
  }
  return TRUE;
}

void handle_sigint(int) {
  if (g_loop) g_main_loop_quit(g_loop);
}

}  // namespace

int main(int argc, char** argv) {
  auto cfgOpt = parse_args(argc, argv);
  if (!cfgOpt) return 1;
  EngineConfig cfg = *cfgOpt;

  gst_init(&argc, &argv);
  signal(SIGINT, handle_sigint);

  PipelineElements el;
  el.pipeline = gst_pipeline_new("ve-pipeline");
  if (!el.pipeline) {
    LOG_ERROR("Failed to create pipeline");
    return 1;
  }

  el.source = make_checked(cfg.source.c_str(), "source");
  el.convert = make_checked("videoconvert", "convert");
  el.scale = make_checked("videoscale", "scale");
  el.rate = make_checked("videorate", "rate");
  el.capsfilter = make_checked("capsfilter", "caps");
  el.queue = make_checked("queue", "buffer");
  el.encoder = make_checked("x264enc", "encoder");
  el.parser = make_checked("h264parse", "parser");
  el.pay = make_checked("rtph264pay", "pay");
  el.udpsink_rtp = make_checked("udpsink", "udpsink_rtp");
  el.udpsink_fec = make_checked("udpsink", "udpsink_fec");

  if (cfg.mode == "rtpbin") {
    el.rtpbin = make_checked("rtpbin", "rtpbin");
    el.udpsink_rtcp = make_checked("udpsink", "udpsink_rtcp");
    el.udpsrc_rtcp = make_checked("udpsrc", "udpsrc_rtcp");
  } else {
    el.tee = make_checked("tee", "tee");
  }

  std::vector<GstElement*> mandatory = {
      el.source, el.convert, el.scale, el.rate, el.capsfilter,
      el.queue, el.encoder, el.parser, el.pay,
      el.udpsink_rtp, el.udpsink_fec,
  };
  if (std::any_of(mandatory.begin(), mandatory.end(), [](GstElement* e){ return e == nullptr; })) {
    LOG_ERROR("Element creation failed. Ensure required GStreamer plugins are installed.");
    return 1;
  }
  if (cfg.mode == "rtpbin" && (!el.rtpbin || !el.udpsink_rtcp || !el.udpsrc_rtcp)) {
    LOG_ERROR("RTP bin mode requires rtpbin/RTCP elements");
    return 1;
  }
  if (cfg.mode == "simple" && !el.tee) {
    LOG_ERROR("Simple mode requires tee element");
    return 1;
  }

  configure_source(el.source, cfg);
  configure_caps(el.capsfilter, cfg.profile);
  configure_queue(el.queue, cfg.latency_ms);
  configure_encoder(el.encoder, cfg.profile);
  configure_payloader(el.pay);
  configure_sink(el.udpsink_rtp, cfg.dest_ip, cfg.ports.rtp_port);
  configure_sink(el.udpsink_fec, cfg.dest_ip, cfg.ports.fec_port);
  if (el.rate) configure_videorate(el.rate, cfg.profile);
  if (cfg.mode == "rtpbin") {
    configure_sink(el.udpsink_rtcp, cfg.dest_ip, cfg.ports.rtcp_send_port);
    g_object_set(el.udpsrc_rtcp, "port", cfg.ports.rtcp_recv_port, NULL);

    GstStructure* fecmap = gst_structure_new_empty("fec");
    std::string fec_desc = "rtpulpfecenc percentage=" + std::to_string(cfg.fec_percentage);
    gst_structure_set(fecmap, "0", G_TYPE_STRING, fec_desc.c_str(), NULL);
    g_object_set(el.rtpbin, "fec-encoders", fecmap, NULL);
    gst_structure_free(fecmap);
    g_object_set(el.rtpbin, "latency", cfg.latency_ms, NULL);
  }

  gst_bin_add_many(GST_BIN(el.pipeline),
                   el.source, el.convert, el.scale, el.rate, el.capsfilter,
                   el.queue, el.encoder, el.parser, el.pay,
                   el.udpsink_rtp, el.udpsink_fec,
                   NULL);
  if (cfg.mode == "rtpbin") {
    gst_bin_add_many(GST_BIN(el.pipeline), el.rtpbin, el.udpsink_rtcp, el.udpsrc_rtcp, NULL);
  } else {
    gst_bin_add(GST_BIN(el.pipeline), el.tee);
  }

  if (!gst_element_link_many(el.source, el.convert, el.scale, el.rate, el.capsfilter,
                             el.queue, el.encoder, el.parser, el.pay, NULL)) {
    LOG_ERROR("Failed to link main video chain");
    return 1;
  }

  if (cfg.mode == "rtpbin") {
    attach_rtpbin_links(el.rtpbin, el.pay, el.udpsink_rtp, el.udpsink_fec,
                        el.udpsink_rtcp, el.udpsrc_rtcp);
  } else {
    GstElement* q_rtp = make_checked("queue", "queue_rtp");
    GstElement* q_fec = make_checked("queue", "queue_fec");
    GstElement* fecenc = make_checked("rtpulpfecenc", "fecenc");
    if (!q_rtp || !q_fec || !fecenc) {
      LOG_ERROR("Failed to create FEC branch elements");
      return 1;
    }
    configure_queue(q_rtp, cfg.latency_ms);
    configure_queue(q_fec, cfg.latency_ms);
    g_object_set(fecenc, "percentage", cfg.fec_percentage, NULL);

    gst_bin_add_many(GST_BIN(el.pipeline), q_rtp, q_fec, fecenc, NULL);

    if (!gst_element_link(el.pay, el.tee)) {
      LOG_ERROR("Failed to link payloader to tee");
      return 1;
    }
    if (!gst_element_link_many(el.tee, q_rtp, el.udpsink_rtp, NULL)) {
      LOG_ERROR("Failed to link tee RTP branch");
      return 1;
    }
    if (!gst_element_link_many(el.tee, q_fec, fecenc, el.udpsink_fec, NULL)) {
      LOG_ERROR("Failed to link tee FEC branch");
      return 1;
    }
  }

  GstBus* bus = gst_element_get_bus(el.pipeline);
  g_loop = g_main_loop_new(nullptr, FALSE);
  guint bus_watch_id = 0;
  if (bus) {
    bus_watch_id = gst_bus_add_watch(bus, bus_call, nullptr);
  }

  QosController qos;
  qos.attach(el.rtpbin, el.encoder, bus);
  qos.start(1000);

  LOG_INFO("Starting pipeline to ", cfg.dest_ip,
           " ports rtp=", cfg.ports.rtp_port,
           " fec=", cfg.ports.fec_port,
           " rtcp_send=", cfg.ports.rtcp_send_port,
           " rtcp_recv=", cfg.ports.rtcp_recv_port,
            ", profile ", cfg.profile.width, "x", cfg.profile.height, "@", cfg.profile.fps,
           ", bitrate=", cfg.profile.bitrate_kbps, "kbps, fec=", cfg.fec_percentage,
           "%, latency=", cfg.latency_ms, "ms");

  gst_element_set_state(el.pipeline, GST_STATE_PLAYING);
  g_main_loop_run(g_loop);

  qos.stop();
  gst_element_set_state(el.pipeline, GST_STATE_NULL);
  if (bus_watch_id != 0) g_source_remove(bus_watch_id);
  if (bus) gst_object_unref(bus);
  if (g_loop) { g_main_loop_unref(g_loop); g_loop = nullptr; }
  gst_object_unref(el.pipeline);

  LOG_INFO("Exited cleanly");
  return 0;
}
