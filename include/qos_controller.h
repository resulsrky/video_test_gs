// QoS controller stub to adapt encoder based on observed loss
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

// Forward declare GStreamer types to avoid hard dependency in header
typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;

namespace ve {

class QosController {
 public:
  QosController();
  ~QosController();

  // Provide handles; controller may read stats and adjust encoder properties periodically.
  void attach(GstElement* rtpbin, GstElement* encoder, GstBus* bus);

  // Starts periodic monitoring with given interval (ms).
  void start(int interval_ms = 1000);
  void stop();

 private:
  void run_loop();

  GstElement* rtpbin_ = nullptr;
  GstElement* encoder_ = nullptr;
  GstBus* bus_ = nullptr;

  std::atomic<bool> running_{false};
  int interval_ms_ = 1000;
  std::thread worker_;
};

}  // namespace ve
