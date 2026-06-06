#pragma once

#include "esphome/core/component.h"
#include "esphome/components/speaker/speaker.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace wyoming_announce {

class WyomingAnnounce : public Component {
 public:
  void set_speaker(speaker::Speaker *spkr) { this->speaker_ = spkr; }
  void set_port(uint16_t port) { this->port_ = port; }

  void setup() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  speaker::Speaker *speaker_{nullptr};
  uint16_t port_{10301};
  int server_fd_{-1};

  static void server_task_entry(void *param);
  void run_server();
  void handle_client(int fd);
};

}  // namespace wyoming_announce
}  // namespace esphome
