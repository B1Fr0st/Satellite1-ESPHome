#include "wyoming_announce.h"
#include "esphome/core/log.h"
#include "esphome/components/audio/audio.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace esphome {
namespace wyoming_announce {

static const char *TAG = "wyoming_announce";

// ── Socket helpers ────────────────────────────────────────────────────────────

static bool read_line(int fd, char *buf, size_t max) {
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

static bool read_exact(int fd, uint8_t *buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    ssize_t n = recv(fd, buf + got, len - got, 0);
    if (n <= 0)
      return false;
    got += n;
  }
  return true;
}

// Find "key":N in a flat JSON line and return N, or -1 if not found.
static int find_int(const char *s, const char *key) {
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

// ── Component ─────────────────────────────────────────────────────────────────

void WyomingAnnounce::setup() {
  xTaskCreate(server_task_entry, "wyoming_ann", 8192, this, 5, nullptr);
}

void WyomingAnnounce::server_task_entry(void *param) {
  static_cast<WyomingAnnounce *>(param)->run_server();
  vTaskDelete(nullptr);
}

void WyomingAnnounce::run_server() {
  this->server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (this->server_fd_ < 0) {
    ESP_LOGE(TAG, "socket() failed");
    return;
  }

  int opt = 1;
  setsockopt(this->server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(this->port_);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(this->server_fd_, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind() failed on port %d", this->port_);
    close(this->server_fd_);
    return;
  }

  listen(this->server_fd_, 1);
  ESP_LOGI(TAG, "Wyoming announce listener on :%d", this->port_);

  while (true) {
    int client = accept(this->server_fd_, nullptr, nullptr);
    if (client < 0)
      continue;
    ESP_LOGI(TAG, "announce connection from 9r9r");
    this->handle_client(client);
    close(client);
  }
}

void WyomingAnnounce::handle_client(int fd) {
  char line[512];
  // Static buffer avoids blowing the 8K task stack with large local arrays.
  static uint8_t chunk_buf[4096];

  while (read_line(fd, line, sizeof(line))) {
    if (strstr(line, "\"audio-start\"")) {
      int rate     = find_int(line, "rate");
      int width    = find_int(line, "width");
      int channels = find_int(line, "channels");
      if (rate     <= 0) rate     = 24000;
      if (width    <= 0) width    = 2;
      if (channels <= 0) channels = 1;
      ESP_LOGI(TAG, "audio-start rate=%d width=%d ch=%d | %.100s", rate, width, channels, line);
      this->speaker_->set_audio_stream_info(
          audio::AudioStreamInfo((uint8_t)(width * 8), (uint8_t)channels, (uint32_t)rate));
      this->speaker_->start();

    } else if (strstr(line, "\"audio-chunk\"")) {
      int payload_len = find_int(line, "payload_length");
      if (payload_len <= 0)
        continue;

      int remaining = payload_len;
      while (remaining > 0) {
        int to_read = std::min(remaining, (int) sizeof(chunk_buf));
        if (!read_exact(fd, chunk_buf, to_read))
          return;

        // play() may accept fewer bytes than offered when the buffer is full.
        size_t written = 0;
        while (written < (size_t) to_read) {
          size_t n = this->speaker_->play(chunk_buf + written, to_read - written);
          if (n == 0)
            vTaskDelay(pdMS_TO_TICKS(5));
          written += n;
        }
        remaining -= to_read;
      }

    } else if (strstr(line, "\"audio-stop\"")) {
      ESP_LOGI(TAG, "audio-stop — draining buffer");
      // Allow DMA buffer to drain before stopping the peripheral.
      vTaskDelay(pdMS_TO_TICKS(300));
      this->speaker_->stop();
    }
  }

  // Connection closed before audio-stop — clean up.
  this->speaker_->stop();
}

}  // namespace wyoming_announce
}  // namespace esphome
