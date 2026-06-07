#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <string>

namespace esphome {
namespace wyoming_satellite {

// Each callback delivers ~16ms of 16kHz mono int16 = 256 samples = 512 bytes.
static const size_t AUDIO_CHUNK_SAMPLES = 256;
static const size_t AUDIO_CHUNK_BYTES = AUDIO_CHUNK_SAMPLES * sizeof(int16_t);
static const size_t AUDIO_QUEUE_DEPTH = 40;  // ~640ms of audio buffer

struct AudioChunk {
  uint8_t data[AUDIO_CHUNK_BYTES];
  size_t len;
};

enum class State { IDLE, CONNECTING, LISTENING, THINKING, REPLYING };

enum CallbackBit : uint8_t {
  CB_NONE = 0,
  CB_LISTENING = 1,
  CB_THINKING = 2,
  CB_REPLYING = 4,
  CB_IDLE = 8,
  CB_ERROR = 16,
};

class WyomingSatellite : public Component {
 public:
  void set_host(const std::string &host) { host_ = host; }
  void set_port(uint16_t port) { port_ = port; }
  void set_microphone(microphone::Microphone *mic) { mic_ = mic; }
  void set_speaker(speaker::Speaker *spkr) { spkr_ = spkr; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void start_pipeline();

  bool is_idle() const { return state_ == State::IDLE; }

  void add_on_idle_callback(std::function<void()> cb) { idle_callbacks_.add(std::move(cb)); }
  void add_on_listening_callback(std::function<void()> cb) { listening_callbacks_.add(std::move(cb)); }
  void add_on_thinking_callback(std::function<void()> cb) { thinking_callbacks_.add(std::move(cb)); }
  void add_on_replying_callback(std::function<void()> cb) { replying_callbacks_.add(std::move(cb)); }
  void add_on_error_callback(std::function<void()> cb) { error_callbacks_.add(std::move(cb)); }

 protected:
  static void pipeline_task_entry(void *param);
  void run_pipeline_();

  static bool write_all(int fd, const uint8_t *buf, size_t len);
  static bool read_exact(int fd, uint8_t *buf, size_t len);
  static bool read_line(int fd, char *buf, size_t max);
  static int find_int(const char *s, const char *key);

  std::string host_;
  uint16_t port_{10300};
  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *spkr_{nullptr};

  QueueHandle_t audio_queue_{nullptr};
  TaskHandle_t pipeline_task_{nullptr};

  // Written by start_pipeline() (main loop) and run_pipeline_() (pipeline task).
  // Declared volatile; for a single-producer/single-consumer bool on Xtensa this is safe.
  volatile bool is_listening_{false};

  // Pipeline task posts state changes here; loop() drains them on the main task.
  std::atomic<uint8_t> pending_callbacks_{CB_NONE};

  // Only written by loop() on the main task.
  State state_{State::IDLE};

  CallbackManager<void()> idle_callbacks_;
  CallbackManager<void()> listening_callbacks_;
  CallbackManager<void()> thinking_callbacks_;
  CallbackManager<void()> replying_callbacks_;
  CallbackManager<void()> error_callbacks_;
};

// ── Action ───────────────────────────────────────────────────────────────────

template<typename... Ts>
class StartPipelineAction : public Action<Ts...> {
 public:
  explicit StartPipelineAction(WyomingSatellite *parent) : parent_(parent) {}
  void play(Ts... x) override { parent_->start_pipeline(); }

 private:
  WyomingSatellite *parent_;
};

// ── Triggers ─────────────────────────────────────────────────────────────────

class IdleTrigger : public Trigger<> {
 public:
  explicit IdleTrigger(WyomingSatellite *parent) {
    parent->add_on_idle_callback([this]() { trigger(); });
  }
};

class ListeningTrigger : public Trigger<> {
 public:
  explicit ListeningTrigger(WyomingSatellite *parent) {
    parent->add_on_listening_callback([this]() { trigger(); });
  }
};

class ThinkingTrigger : public Trigger<> {
 public:
  explicit ThinkingTrigger(WyomingSatellite *parent) {
    parent->add_on_thinking_callback([this]() { trigger(); });
  }
};

class ReplyingTrigger : public Trigger<> {
 public:
  explicit ReplyingTrigger(WyomingSatellite *parent) {
    parent->add_on_replying_callback([this]() { trigger(); });
  }
};

class ErrorTrigger : public Trigger<> {
 public:
  explicit ErrorTrigger(WyomingSatellite *parent) {
    parent->add_on_error_callback([this]() { trigger(); });
  }
};

}  // namespace wyoming_satellite
}  // namespace esphome
