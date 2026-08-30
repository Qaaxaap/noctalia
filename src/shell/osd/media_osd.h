#pragma once

#include "core/timer_manager.h"
#include "shell/osd/osd_overlay.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

class MprisService;
struct Config;

struct MediaOsdData {
  std::string title;
  std::string artist;

  bool operator==(const MediaOsdData& d) const { return d.artist == artist && d.title == title; }
};

class MediaOsd {
public:
  void bindOverlay(OsdOverlay& overlay);
  void configure(const Config& config);
  void onMprisChanged(const MprisService& service);

private:
  [[nodiscard]] bool inCooldown(std::chrono::steady_clock::time_point now) const noexcept;
  void pushTrackContent(const OsdContent& content);
  void schedulePendingFlush(std::chrono::steady_clock::time_point when);
  void flushPending();

  OsdOverlay* m_overlay = nullptr;
  MediaOsdData m_lastData;
  std::unordered_map<std::string, double> m_lastVolumes;
  std::chrono::milliseconds m_cooldown{0};
  std::chrono::steady_clock::time_point m_cooldownUntil;
  std::optional<OsdContent> m_pendingContent;
  TimerManager::TimerId m_pendingFlushTimer = 0;
  bool m_hasData = false;
  // Keeps TimerManager callbacks from touching a destroyed MediaOsd.
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
};
