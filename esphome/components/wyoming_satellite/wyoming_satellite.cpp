#include "wyoming_satellite.h"
#include "esphome/core/log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

namespace esphome {
namespace wyoming_satellite {

static const char *TAG = "wyoming_satellite";

// VAD: energy threshold for speech detection (RMS of int16 samples).
// Tune lower if the room is very quiet; tune higher to avoid noise false-starts.
static const uint32_t VAD_THRESHOLD = 400;

// Consecutive silent chunks (each = 16ms) to trigger end-of-speech.
// 35 × 16ms ≈ 560ms silence = stop listening.
static const uint32_t VAD_SILENCE_CHUNKS = 35;

// Minimum speech chunks before VAD silence counter is armed.
// 5 × 16ms = 80ms — prevents a quiet breath from immediately terminating.
static const uint32_t VAD_MIN_SPEECH_CHUNKS = 5;

// Hard timeout while listening (before end-of-speech is detected).
static const uint32_t LISTEN_TIMEOUT_MS = 12000;

// Timeout waiting for the server response after audio-stop.
static const uint32_t RESPONSE_TIMEOUT_MS = 30000;

// ── Socket helpers ────────────────────────────────────────────────────────────

bool WyomingSatellite::write_all(int fd, const uint8_t *buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, buf + sent, len - sent, 0);
    if (n <= 0)
      return false;
    sent += n;
  }
  return true;
}

bool WyomingSatellite::read_exact(int fd, uint8_t *buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    ssize_t n = recv(fd, buf + got, len - got, 0);
    if (n <= 0)
      return false;
    got += n;
  }
  return true;
}

bool WyomingSatellite::read_line(int fd, char *buf, size_t max) {
  size_t n = 0;
  while (n < max - 1) {
    char c;
    if (recv(fd, &c, 1, 0) != 1)
      return false;
    if (c == '\n') {
      buf[n] = '\0';
      return true;
    }
    if (c != '\r')
      buf[n++] = c;
  }
  return false;
}

// Find "key":N (integer) in a JSON string, searching both flat and "data":{...} forms.
int WyomingSatellite::find_int(const char *s, const char *key) {
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char *p = strstr(s, pat);
  if (!p)
    return -1;
  p += strlen(pat);
  while (*p == ' ')
    p++;
  return (int) strtol(p, nullptr, 10);
}

static uint32_t rms16(const uint8_t *data, size_t len) {
  const int16_t *s = (const int16_t *) data;
  size_t n = len / sizeof(int16_t);
  if (n == 0)
    return 0;
  uint64_t sum = 0;
  for (size_t i = 0; i < n; i++)
    sum += (int64_t) s[i] * s[i];
  return (uint32_t) sqrt((double) sum / n);
}

// ── Component lifecycle ───────────────────────────────────────────────────────

void WyomingSatellite::setup() {
  this->audio_queue_ = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(AudioChunk));
  if (!this->audio_queue_) {
    ESP_LOGE(TAG, "failed to allocate audio queue");
    this->mark_failed();
    return;
  }

  // Register the mic callback once; use is_listening_ to gate data flow.
  // The callback runs on the mic FreeRTOS task (possibly a different core).
  // We take the left channel from the stereo 32-bit output: even-indexed int32 values
  // correspond to L samples at the correct 16 kHz rate after 3× decimation.
  this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
    if (!this->is_listening_)
      return;

    const int32_t *s32 = (const int32_t *) data.data();
    size_t n32 = data.size() / sizeof(int32_t);

    AudioChunk chunk;
    chunk.len = 0;

    for (size_t i = 0; i < n32; i += 2) {
      // Right-shift Q31 → int16 (takes the upper 16 bits = the audio data).
      int16_t s = (int16_t) (s32[i] >> 16);
      memcpy(chunk.data + chunk.len, &s, sizeof(int16_t));
      chunk.len += sizeof(int16_t);
    }

    if (chunk.len > 0)
      xQueueSend(this->audio_queue_, &chunk, 0);  // drop if full
  });

  this->mic_->start();
  ESP_LOGI(TAG, "ready — server=%s:%d", this->host_.c_str(), (int) this->port_);
}

void WyomingSatellite::loop() {
  uint8_t bits = this->pending_callbacks_.exchange(CB_NONE);
  if (bits == CB_NONE)
    return;

  // Fire callbacks in pipeline order so LED transitions are logical.
  if (bits & CB_LISTENING) {
    this->state_ = State::LISTENING;
    this->listening_callbacks_.call();
  }
  if (bits & CB_THINKING) {
    this->state_ = State::THINKING;
    this->thinking_callbacks_.call();
  }
  if (bits & CB_REPLYING) {
    this->state_ = State::REPLYING;
    this->replying_callbacks_.call();
  }
  if (bits & CB_IDLE) {
    this->state_ = State::IDLE;
    this->idle_callbacks_.call();
  }
  if (bits & CB_ERROR) {
    this->state_ = State::IDLE;
    this->error_callbacks_.call();
  }
}

void WyomingSatellite::start_pipeline() {
  if (this->state_ != State::IDLE) {
    ESP_LOGD(TAG, "start_pipeline ignored — pipeline already active");
    return;
  }

  // Reset audio state before enabling the callback.
  xQueueReset(this->audio_queue_);
  this->state_ = State::CONNECTING;

  // Enable mic collection immediately so we capture audio while connecting.
  this->is_listening_ = true;

  if (this->pipeline_task_) {
    vTaskDelete(this->pipeline_task_);
    this->pipeline_task_ = nullptr;
  }

  xTaskCreate(pipeline_task_entry, "wyoming_sat", 8192, this, 5, &this->pipeline_task_);
}

// ── Pipeline task ─────────────────────────────────────────────────────────────

void WyomingSatellite::pipeline_task_entry(void *param) {
  static_cast<WyomingSatellite *>(param)->run_pipeline_();
  vTaskDelete(nullptr);
}

void WyomingSatellite::run_pipeline_() {
  char hdr[512];
  // Static so it doesn't blow the 8 KB task stack.
  static uint8_t chunk_buf[4096];

  int fd = -1;
  bool err = false;
  bool spkr_started = false;

  do {
    // ── DNS resolve + connect ─────────────────────────────────────────────
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *ai = nullptr;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", (int) this->port_);

    if (getaddrinfo(this->host_.c_str(), port_str, &hints, &ai) != 0 || !ai) {
      ESP_LOGE(TAG, "DNS lookup failed for %s", this->host_.c_str());
      err = true;
      break;
    }

    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      freeaddrinfo(ai);
      err = true;
      break;
    }

    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (rc != 0) {
      ESP_LOGE(TAG, "connect to %s:%d failed", this->host_.c_str(), (int) this->port_);
      err = true;
      break;
    }

    // 30-second receive timeout — long enough for the LLM + TTS pipeline.
    struct timeval tv{.tv_sec = 30};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "connected to %s:%d", this->host_.c_str(), (int) this->port_);

    // ── Handshake: run-pipeline + audio-start ────────────────────────────
    static const char *run_msg =
        "{\"type\":\"run-pipeline\",\"data\":{\"start_stage\":\"asr\",\"end_stage\":\"tts\"}}\n";
    if (!write_all(fd, (const uint8_t *) run_msg, strlen(run_msg))) {
      err = true;
      break;
    }

    static const char *audio_start_msg =
        "{\"type\":\"audio-start\",\"data\":{\"rate\":16000,\"width\":2,\"channels\":1}}\n";
    if (!write_all(fd, (const uint8_t *) audio_start_msg, strlen(audio_start_msg))) {
      err = true;
      break;
    }

    this->pending_callbacks_.fetch_or(CB_LISTENING);

    // ── Stream audio with energy-based VAD ───────────────────────────────
    uint32_t speech_chunks = 0;
    uint32_t silent_run = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(LISTEN_TIMEOUT_MS);

    while (xTaskGetTickCount() < deadline) {
      AudioChunk chunk;
      if (xQueueReceive(this->audio_queue_, &chunk, pdMS_TO_TICKS(50)) != pdTRUE)
        continue;

      // Send audio-chunk header (nested "data" format that 9r9r's Wyoming reader expects).
      int hdr_len = snprintf(hdr, sizeof(hdr),
                             "{\"type\":\"audio-chunk\","
                             "\"data\":{\"rate\":16000,\"width\":2,\"channels\":1},"
                             "\"payload_length\":%zu}\n",
                             chunk.len);
      if (!write_all(fd, (const uint8_t *) hdr, hdr_len)) {
        err = true;
        break;
      }
      if (!write_all(fd, chunk.data, chunk.len)) {
        err = true;
        break;
      }

      // Energy VAD.
      uint32_t energy = rms16(chunk.data, chunk.len);
      if (energy >= VAD_THRESHOLD) {
        speech_chunks++;
        silent_run = 0;
      } else if (speech_chunks >= VAD_MIN_SPEECH_CHUNKS) {
        silent_run++;
        if (silent_run >= VAD_SILENCE_CHUNKS) {
          ESP_LOGI(TAG, "VAD: end of speech (energy=%u)", energy);
          break;
        }
      }
    }

    if (err)
      break;

    this->is_listening_ = false;

    // ── audio-stop ───────────────────────────────────────────────────────
    static const char *audio_stop_msg = "{\"type\":\"audio-stop\"}\n";
    if (!write_all(fd, (const uint8_t *) audio_stop_msg, strlen(audio_stop_msg))) {
      err = true;
      break;
    }

    this->pending_callbacks_.fetch_or(CB_THINKING);
    ESP_LOGI(TAG, "audio-stop sent, waiting for response");

    // ── Receive server response ───────────────────────────────────────────
    TickType_t resp_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS);

    while (xTaskGetTickCount() < resp_deadline) {
      if (!read_line(fd, hdr, sizeof(hdr))) {
        ESP_LOGW(TAG, "connection closed by server");
        break;
      }

      if (strstr(hdr, "\"run-pipeline-error\"") || strstr(hdr, "\"error\"")) {
        ESP_LOGE(TAG, "server error: %.80s", hdr);
        err = true;
        break;
      }

      // transcript / synthesize / intent: may carry a data_length blob (new Wyoming format).
      // Read and discard the blob; we only need the audio events.
      if (strstr(hdr, "\"transcript\"") || strstr(hdr, "\"synthesize\"") ||
          strstr(hdr, "\"intent\"") || strstr(hdr, "\"tts-start\"") ||
          strstr(hdr, "\"tts-end\"") || strstr(hdr, "\"pipeline-run-start\"")) {
        int blob = find_int(hdr, "data_length");
        if (blob > 0) {
          uint8_t *tmp = (uint8_t *) malloc(blob);
          if (tmp) {
            read_exact(fd, tmp, blob);
            free(tmp);
          }
        }
        // Extend deadline on each meaningful server event.
        resp_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS);
        continue;
      }

      if (strstr(hdr, "\"audio-start\"")) {
        ESP_LOGI(TAG, "TTS audio starting");
        this->spkr_->start();
        spkr_started = true;
        this->pending_callbacks_.fetch_or(CB_REPLYING);
        resp_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS);
        continue;
      }

      if (strstr(hdr, "\"audio-chunk\"")) {
        int payload_len = find_int(hdr, "payload_length");
        if (payload_len <= 0)
          continue;

        int remaining = payload_len;
        while (remaining > 0) {
          int to_read = std::min(remaining, (int) sizeof(chunk_buf));
          if (!read_exact(fd, chunk_buf, to_read)) {
            err = true;
            break;
          }
          size_t written = 0;
          while (written < (size_t) to_read) {
            size_t n = this->spkr_->play(chunk_buf + written, to_read - written);
            if (n == 0)
              vTaskDelay(pdMS_TO_TICKS(5));
            written += n;
          }
          remaining -= to_read;
        }
        if (err)
          break;
        resp_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS);
        continue;
      }

      if (strstr(hdr, "\"audio-stop\"")) {
        ESP_LOGI(TAG, "TTS audio complete, draining speaker");
        vTaskDelay(pdMS_TO_TICKS(300));
        this->spkr_->stop();
        spkr_started = false;
        break;
      }
    }

  } while (false);

  // ── Cleanup ───────────────────────────────────────────────────────────────
  this->is_listening_ = false;
  if (spkr_started)
    this->spkr_->stop();
  if (fd >= 0)
    close(fd);

  this->pipeline_task_ = nullptr;

  if (err) {
    ESP_LOGE(TAG, "pipeline error — returning to idle");
    this->pending_callbacks_.fetch_or(CB_ERROR);
  } else {
    this->pending_callbacks_.fetch_or(CB_IDLE);
  }
}

}  // namespace wyoming_satellite
}  // namespace esphome
