#include "logger.h"
#include "qos_controller.h"
#include "utils.h"

#include <gst/gst.h>
#include <gst/rtp/rtp.h>

#include <csignal>
#include <memory>

using namespace ve;

static GMainLoop* g_loop = nullptr;

static gboolean bus_call(GstBus* bus, GstMessage* msg, gpointer /*data*/) {
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr; gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      LOG_ERROR("GStreamer ERROR: ", (err ? err->message : "(unknown)"));
      if (dbg) { LOG_DEBUG("Debug: ", dbg); g_free(dbg); }
      if (err) g_error_free(err);
      if (g_loop) g_main_loop_quit(g_loop);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError* err = nullptr; gchar* dbg = nullptr;
      gst_message_parse_warning(msg, &err, &dbg);
      LOG_WARN("GStreamer WARN: ", (err ? err->message : "(unknown)"));
      if (dbg) { LOG_DEBUG("Debug: ", dbg); g_free(dbg); }
      if (err) g_error_free(err);
      break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
      GstState old_s, new_s, pending_s;
      gst_message_parse_state_changed(msg, &old_s, &new_s, &pending_s);
      const gchar* name = GST_OBJECT_NAME(GST_MESSAGE_SRC(msg));
      LOG_DEBUG("State changed: ", (name ? name : "(unknown)"), " ",
                gst_element_state_get_name(old_s), " -> ", gst_element_state_get_name(new_s));
      break;
    }
    case GST_MESSAGE_EOS:
      LOG_INFO("Pipeline EOS");
      if (g_loop) g_main_loop_quit(g_loop);
      break;
    default:
      break;
  }
  return TRUE;
}

static void handle_sigint(int) {
  if (g_loop) g_main_loop_quit(g_loop);
}

int main(int argc, char** argv) {
  auto cfgOpt = parse_args(argc, argv);
  if (!cfgOpt) return 1;
  EngineConfig cfg = *cfgOpt;

  gst_init(&argc, &argv);

  signal(SIGINT, handle_sigint);

  // Elements
  GstElement *pipeline = gst_pipeline_new("ve-pipeline");
  if (!pipeline) { LOG_ERROR("Failed to create pipeline"); return 1; }

  GstElement *source = gst_element_factory_make(cfg.source.c_str(), "source");
  GstElement *capsfilter = gst_element_factory_make("capsfilter", "caps");
  GstElement *queue = gst_element_factory_make("queue", "queue");
  GstElement *encoder = gst_element_factory_make("x264enc", "x264enc");
  GstElement *parser = gst_element_factory_make("h264parse", "h264parse");
  GstElement *pay = gst_element_factory_make("rtph264pay", "rtph264pay");
  GstElement *rtpbin = nullptr;
  GstElement *tee = nullptr;
  if (cfg.mode == "rtpbin") {
    rtpbin = gst_element_factory_make("rtpbin", "rtpbin");
  } else {
    tee = gst_element_factory_make("tee", "tee");
  }

  GstElement *udpsink_rtp = gst_element_factory_make("udpsink", "udpsink-rtp");
  GstElement *udpsink_fec = gst_element_factory_make("udpsink", "udpsink-fec");
  GstElement *udpsink_rtcp = cfg.mode == "rtpbin" ? gst_element_factory_make("udpsink", "udpsink-rtcp") : nullptr;
  GstElement *udpsrc_rtcp = cfg.mode == "rtpbin" ? gst_element_factory_make("udpsrc", "udpsrc-rtcp") : nullptr;

  if (!source || !capsfilter || !queue || !encoder || !parser || !pay ||
      !udpsink_rtp || !udpsink_fec || (cfg.mode == "rtpbin" && (!rtpbin || !udpsink_rtcp || !udpsrc_rtcp)) ||
      (cfg.mode == "simple" && !tee)) {
    LOG_ERROR("Element creation failed. Ensure GStreamer plugins are installed (ximagesrc/v4l2src, x264enc, rtpulpfecenc, etc.)");
    return 1;
  }

  // Configure source
  if (cfg.source == "ximagesrc") {
    g_object_set(source, "use-damage", FALSE, "show-pointer", FALSE, NULL);
  } else if (cfg.source == "v4l2src") {
    // Optionally set device via env/arg in the future
  } else if (cfg.source == "videotestsrc") {
    g_object_set(source, "is-live", TRUE, NULL);
  }

  // Caps for resolution/fps
  GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                      "width", G_TYPE_INT, cfg.profile.width,
                                      "height", G_TYPE_INT, cfg.profile.height,
                                      "framerate", GST_TYPE_FRACTION, cfg.profile.fps, 1,
                                      NULL);
  g_object_set(capsfilter, "caps", caps, NULL);
  gst_caps_unref(caps);

  // Queue buffering: 50ms cap, leaky to keep latency low
  g_object_set(queue,
               "leaky", 2 /*downstream*/,
               "max-size-buffers", 0,
               "max-size-bytes", 0,
               "max-size-time", 50 * GST_MSECOND,
               NULL);

  // Encoder low latency settings
  g_object_set(encoder,
               "tune", 0x00000004 /*zerolatency*/,
               "speed-preset", 1 /*ultrafast*/,
               "key-int-max", cfg.profile.fps * 2,
               "bitrate", cfg.profile.bitrate_kbps,
               NULL);

  // Parser for bytestream
  g_object_set(parser, "config-interval", -1, "disable-passthrough", TRUE, NULL);

  // Payloader: dynamic pt=96, set ssrc optionally
  g_object_set(pay, "pt", 96, "config-interval", 1, NULL);

  // Configure rtpbin to use ULPFEC internally (rtpbin mode)
  if (cfg.mode == "rtpbin") {
    GstStructure* fecmap = gst_structure_new_empty("fec");
    std::string enc = std::string("rtpulpfecenc percentage=") + std::to_string(cfg.fec_percentage);
    gst_structure_set(fecmap, "0", G_TYPE_STRING, enc.c_str(), NULL);
    g_object_set(rtpbin, "fec-encoders", fecmap, NULL);
    gst_structure_free(fecmap);
  }

  // RTP bin configuration: keep defaults for stability

  // UDP sinks/src configuration
  g_object_set(udpsink_rtp, "host", cfg.dest_ip.c_str(), "port", cfg.ports.rtp_port,
               "ttl", 64, "sync", FALSE, "async", FALSE, NULL);
  g_object_set(udpsink_fec, "host", cfg.dest_ip.c_str(), "port", cfg.ports.fec_port,
               "ttl", 64, "sync", FALSE, "async", FALSE, NULL);
  if (cfg.mode == "rtpbin") {
    g_object_set(udpsink_rtcp, "host", cfg.dest_ip.c_str(), "port", cfg.ports.rtcp_send_port,
                 "ttl", 64, "sync", FALSE, "async", FALSE, NULL);
    g_object_set(udpsrc_rtcp, "port", cfg.ports.rtcp_recv_port, NULL);
  }

  if (cfg.mode == "rtpbin") {
    gst_bin_add_many(GST_BIN(pipeline),
                     source, capsfilter, queue, encoder, parser, pay, rtpbin,
                     udpsink_rtp, udpsink_fec, udpsink_rtcp, udpsrc_rtcp,
                     NULL);
  } else {
    gst_bin_add_many(GST_BIN(pipeline),
                     source, capsfilter, queue, encoder, parser, pay, tee,
                     udpsink_rtp, udpsink_fec,
                     NULL);
  }

  if (!gst_element_link(source, capsfilter)) { LOG_ERROR("Link failed: source->capsfilter"); return 1; }
  if (!gst_element_link(capsfilter, queue)) { LOG_ERROR("Link failed: capsfilter->queue"); return 1; }
  if (!gst_element_link(queue, encoder)) { LOG_ERROR("Link failed: queue->encoder"); return 1; }
  if (!gst_element_link(encoder, parser)) { LOG_ERROR("Link failed: encoder->parser"); return 1; }
  if (!gst_element_link(parser, pay)) { LOG_ERROR("Link failed: parser->pay"); return 1; }

  if (cfg.mode == "rtpbin") {
    // Link payloader src pad to rtpbin send_rtp_sink_0
    GstPad* fec_src = gst_element_get_static_pad(pay, "src");
    GstPad* rtp_sink = gst_element_get_request_pad(rtpbin, "send_rtp_sink_0");
    if (!fec_src || !rtp_sink || gst_pad_link(fec_src, rtp_sink) != GST_PAD_LINK_OK) {
      LOG_ERROR("Failed to link pay->rtpbin");
      if (fec_src) gst_object_unref(fec_src);
      if (rtp_sink) gst_object_unref(rtp_sink);
      return 1;
    }
    gst_object_unref(fec_src);
    gst_object_unref(rtp_sink);

    // Dynamically link rtpbin src pads when they appear
    struct PadLinkCtx { GstElement* udpsink_rtp; GstElement* udpsink_fec; };
    auto* ctx = g_new0(PadLinkCtx, 1);
    ctx->udpsink_rtp = udpsink_rtp;
    ctx->udpsink_fec = udpsink_fec;

    g_signal_connect(rtpbin, "pad-added", G_CALLBACK(+[](GstElement* rtpbin_el, GstPad* new_pad, gpointer user_data){
      PadLinkCtx* c = static_cast<PadLinkCtx*>(user_data);
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
    }), ctx);
  } else {
    // simple mode: pay -> tee; tee branch A -> udpsink_rtp, branch B -> rtpulpfecenc -> udpsink_fec
    GstElement* q1 = gst_element_factory_make("queue", "q-rtp");
    GstElement* q2 = gst_element_factory_make("queue", "q-fec");
    GstElement* fecenc = gst_element_factory_make("rtpulpfecenc", "fecenc");
    if (!q1 || !q2 || !fecenc) { LOG_ERROR("Failed to create tee branch elements"); return 1; }
    g_object_set(fecenc, "percentage", cfg.fec_percentage, NULL);
    gst_bin_add_many(GST_BIN(pipeline), q1, q2, fecenc, NULL);
    if (!gst_element_link(pay, tee)) { LOG_ERROR("Link failed: pay->tee"); return 1; }
    if (!gst_element_link_many(tee, q1, udpsink_rtp, NULL)) { LOG_ERROR("Link failed: tee->q1->udpsink_rtp"); return 1; }
    if (!gst_element_link_many(tee, q2, fecenc, udpsink_fec, NULL)) { LOG_ERROR("Link failed: tee->q2->fecenc->udpsink_fec"); return 1; }
  }

  // Dynamically link rtpbin src pads when they appear
  struct PadLinkCtx { GstElement* udpsink_rtp; GstElement* udpsink_fec; };
  auto* ctx = g_new0(PadLinkCtx, 1);
  ctx->udpsink_rtp = udpsink_rtp;
  ctx->udpsink_fec = udpsink_fec;

  g_signal_connect(rtpbin, "pad-added", G_CALLBACK(+[](GstElement* rtpbin_el, GstPad* new_pad, gpointer user_data){
    PadLinkCtx* c = static_cast<PadLinkCtx*>(user_data);
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
  }), ctx);

  if (cfg.mode == "rtpbin") {
    // Link rtpbin send_rtcp_src_0 -> udpsink_rtcp (request pad)
    GstPad* rtcp_src = gst_element_get_request_pad(rtpbin, "send_rtcp_src_0");
    GstPad* rtcp_sinkpad = gst_element_get_static_pad(udpsink_rtcp, "sink");
    if (!rtcp_src || !rtcp_sinkpad || gst_pad_link(rtcp_src, rtcp_sinkpad) != GST_PAD_LINK_OK) {
      LOG_ERROR("Failed to link rtpbin RTCP src -> udpsink_rtcp");
      if (rtcp_src) gst_object_unref(rtcp_src);
      if (rtcp_sinkpad) gst_object_unref(rtcp_sinkpad);
      return 1;
    }
    gst_object_unref(rtcp_src);
    gst_object_unref(rtcp_sinkpad);
  }

  if (cfg.mode == "rtpbin") {
    // Link udpsrc_rtcp -> rtpbin recv_rtcp_sink_0
    GstPad* rtcp_srcpad = gst_element_get_static_pad(udpsrc_rtcp, "src");
    GstPad* rtcp_sinkpad2 = gst_element_get_request_pad(rtpbin, "recv_rtcp_sink_0");
    if (!rtcp_srcpad || !rtcp_sinkpad2 || gst_pad_link(rtcp_srcpad, rtcp_sinkpad2) != GST_PAD_LINK_OK) {
      LOG_ERROR("Failed to link udpsrc_rtcp -> rtpbin");
      if (rtcp_srcpad) gst_object_unref(rtcp_srcpad);
      if (rtcp_sinkpad2) gst_object_unref(rtcp_sinkpad2);
      return 1;
    }
    gst_object_unref(rtcp_srcpad);
    gst_object_unref(rtcp_sinkpad2);
  }

  // Bus and mainloop
  GstBus* bus = gst_element_get_bus(pipeline);
  g_loop = g_main_loop_new(nullptr, FALSE);
  gst_bus_add_watch(bus, bus_call, nullptr);

  // QoS controller (stub, attached for future use)
  QosController qos;
  qos.attach(rtpbin, encoder, bus);
  qos.start(1000);

  LOG_INFO("Starting pipeline to ", cfg.dest_ip,
           " ports rtp=", cfg.ports.rtp_port,
           " fec=", cfg.ports.fec_port,
           " rtcp_send=", cfg.ports.rtcp_send_port,
           " rtcp_recv=", cfg.ports.rtcp_recv_port,
           ", profile ", cfg.profile.width, "x", cfg.profile.height, "@", cfg.profile.fps,
           ", bitrate=", cfg.profile.bitrate_kbps, "kbps, fec=", cfg.fec_percentage, "%");

  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  g_main_loop_run(g_loop);

  // Tear down
  qos.stop();
  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (bus) gst_object_unref(bus);
  if (g_loop) { g_main_loop_unref(g_loop); g_loop = nullptr; }
  gst_object_unref(pipeline);

  LOG_INFO("Exited cleanly");
  return 0;
}
