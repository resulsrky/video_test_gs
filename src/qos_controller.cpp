#include "qos_controller.h"
#include "logger.h"

#include <gst/gst.h>
#include <chrono>
#include <thread>
#include <algorithm>

namespace {

using namespace ve;

bool extract_field_from_value(const GValue* value, const char* field, double& out) {
  if (!value) return false;
  if (GST_VALUE_HOLDS_STRUCTURE(value)) {
    const GstStructure* st = gst_value_get_structure(value);
    if (gst_structure_has_field(st, field)) {
      double tmp = 0.0;
      if (gst_structure_get_double(st, field, &tmp)) {
        out = tmp;
        return true;
      }
    }
    for (gint i = 0; i < gst_structure_n_fields(st); ++i) {
      const char* name = gst_structure_nth_field_name(st, i);
      if (extract_field_from_value(gst_structure_get_value(st, name), field, out)) {
        return true;
      }
    }
  } else if (GST_VALUE_HOLDS_ARRAY(value)) {
    for (guint i = 0; i < gst_value_array_get_size(value); ++i) {
      if (extract_field_from_value(gst_value_array_get_value(value, i), field, out)) {
        return true;
      }
    }
  } else if (GST_VALUE_HOLDS_LIST(value)) {
    for (guint i = 0; i < gst_value_list_get_size(value); ++i) {
      if (extract_field_from_value(gst_value_list_get_value(value, i), field, out)) {
        return true;
      }
    }
  }
  return false;
}

bool extract_field_from_structure(const GstStructure* st, const char* field, double& out) {
  if (!st) return false;
  if (gst_structure_has_field(st, field)) {
    double tmp = 0.0;
    if (gst_structure_get_double(st, field, &tmp)) {
      out = tmp;
      return true;
    }
  }
  for (gint i = 0; i < gst_structure_n_fields(st); ++i) {
    const char* name = gst_structure_nth_field_name(st, i);
    if (extract_field_from_value(gst_structure_get_value(st, name), field, out)) {
      return true;
    }
  }
  return false;
}

}  // namespace

namespace ve {

QosController::QosController() = default;
QosController::~QosController() { stop(); }

void QosController::attach(GstElement* rtpbin, GstElement* encoder, GstBus* bus) {
  rtpbin_ = rtpbin;
  encoder_ = encoder;
  bus_ = bus;
  if (encoder_) {
    unsigned int bitrate = 0;
    g_object_get(encoder_, "bitrate", &bitrate, NULL);
    base_bitrate_ = bitrate > 0 ? bitrate : 4000;
    min_bitrate_ = std::max(500u, static_cast<unsigned int>(base_bitrate_ * 6 / 10));
    max_bitrate_ = std::max(base_bitrate_, static_cast<unsigned int>(base_bitrate_ * 15 / 10));
  }
}

void QosController::start(int interval_ms) {
  if (running_) return;
  interval_ms_ = interval_ms;
  running_ = true;
  worker_ = std::thread([this]{ run_loop(); });
}

void QosController::stop() {
  if (!running_) return;
  running_ = false;
  if (worker_.joinable()) worker_.join();
}

void QosController::run_loop() {
  // NOTE: In a full implementation, query rtpbin stats (RR reports) and adjust encoder bitrate.
  // Here we provide a stub that could be extended. We keep bitrate steady unless we detect errors.
  int stable_count = 0;
  while (running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));

    if (!encoder_) continue;

    double fraction_lost = 0.0;
    bool have_stats = false;

    if (rtpbin_) {
      GObject* session = nullptr;
      g_signal_emit_by_name(rtpbin_, "get-internal-session", 0, &session);
      if (session) {
        GstStructure* stats = nullptr;
        g_object_get(session, "stats", &stats, NULL);
        if (stats) {
          if (extract_field_from_structure(stats, "fraction-lost", fraction_lost)) {
            have_stats = true;
          }
          gst_structure_free(stats);
        }
        g_object_unref(session);
      }
    }

    if (!have_stats) {
      if ((++stable_count % 10) == 0) {
        LOG_DEBUG("QoS: no stats available yet");
      }
      continue;
    }

    fraction_lost = std::clamp(fraction_lost, 0.0, 1.0);

    unsigned int bitrate = 0;
    g_object_get(encoder_, "bitrate", &bitrate, NULL);
    if (bitrate == 0) bitrate = base_bitrate_;

    if (fraction_lost > 0.08 && bitrate > min_bitrate_) {
      unsigned int new_rate = std::max(min_bitrate_, static_cast<unsigned int>(bitrate * 85 / 100));
      if (new_rate < bitrate) {
        g_object_set(encoder_, "bitrate", new_rate, NULL);
        LOG_WARN("QoS: high loss (", fraction_lost * 100.0, "%) -> bitrate ", bitrate, " -> ", new_rate, " kbps");
      }
      stable_count = 0;
    } else if (fraction_lost < 0.01 && bitrate < max_bitrate_) {
      unsigned int new_rate = std::min(max_bitrate_, static_cast<unsigned int>(bitrate * 105 / 100 + 1));
      if (new_rate > bitrate) {
        g_object_set(encoder_, "bitrate", new_rate, NULL);
        LOG_INFO("QoS: network stable (", fraction_lost * 100.0, "%) -> bitrate ", bitrate, " -> ", new_rate, " kbps");
      }
      stable_count++;
    } else {
      stable_count++;
    }
  }
}

}  // namespace ve
