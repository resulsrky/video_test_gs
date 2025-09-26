#include "qos_controller.h"
#include "logger.h"

#include <gst/gst.h>
#include <chrono>
#include <thread>

namespace ve {

QosController::QosController() = default;
QosController::~QosController() { stop(); }

void QosController::attach(GstElement* rtpbin, GstElement* encoder, GstBus* bus) {
  rtpbin_ = rtpbin;
  encoder_ = encoder;
  bus_ = bus;
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

    // Attempt to read a few informative properties from rtpbin in the future.
    // Placeholder logging to show where stats would be consumed.
    if ((++stable_count % 10) == 0) {
      LOG_DEBUG("QoS: periodic check OK (stub)");
    }

    // Potential adaptation example (disabled by default):
    // guint bitrate = 0; g_object_get(encoder_, "bitrate", &bitrate, NULL);
    // if (observed_loss > 0.05 && bitrate > 800) {
    //   guint new_bitrate = static_cast<guint>(bitrate * 0.85);
    //   g_object_set(encoder_, "bitrate", new_bitrate, NULL);
    //   LOG_WARN("QoS: high loss -> reduce bitrate ", bitrate, " -> ", new_bitrate);
    // }
  }
}

}  // namespace ve
