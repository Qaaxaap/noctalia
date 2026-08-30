#include "shell/osd/media_osd.h"

#include "config/config_types.h"
#include "dbus/mpris/mpris_service.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

  constexpr double kVolumeChangeEpsilon = 0.003;

  OsdContent makeMprisContent(const MediaOsdData& data) {
    return OsdContent{
        .kind = OsdKind::Media,
        .icon = "disc-filled",
        .value = data.artist.empty() ? data.title : data.title + " — " + data.artist,
        .showProgress = false,
    };
  }

  // Text-only, like the track OSD: the value carries a player identity of arbitrary length, which the
  // overlay only ellipsizes to the card interior when there is no progress bar beside it.
  OsdContent makeVolumeContent(const std::string& playerName, double volume) {
    const int percent = static_cast<int>(std::round(std::max(0.0, volume) * 100.0));
    const std::string level = std::to_string(percent) + "%";
    return OsdContent{
        .kind = OsdKind::Media,
        .icon = "disc-filled",
        .value = playerName.empty() ? level : playerName + " — " + level,
        .showProgress = false,
        .overLimit = percent > 100,
    };
  }

} // namespace

void MediaOsd::bindOverlay(OsdOverlay& overlay) { m_overlay = &overlay; }

void MediaOsd::configure(const Config& config) {
  m_cooldown = std::chrono::milliseconds(std::max<std::int64_t>(0, config.osd.mediaCooldownMs));

  // A reload may have shortened the cooldown below what the pending flush is
  // waiting on; release anything that is no longer suppressed.
  if (m_pendingContent.has_value() && !inCooldown(std::chrono::steady_clock::now())) {
    flushPending();
  }
}

bool MediaOsd::inCooldown(std::chrono::steady_clock::time_point now) const noexcept {
  return m_cooldown.count() > 0 && now < m_cooldownUntil;
}

void MediaOsd::pushTrackContent(const OsdContent& content) {
  if (m_overlay == nullptr) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (inCooldown(now)) {
    // Keep only the most recent trigger; it re-appears once the cooldown ends.
    m_pendingContent = content;
    if (m_pendingFlushTimer == 0) {
      schedulePendingFlush(m_cooldownUntil);
    }
    return;
  }
  m_overlay->show(content);
  m_cooldownUntil = now + m_cooldown;
}

void MediaOsd::schedulePendingFlush(std::chrono::steady_clock::time_point when) {
  const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(when - std::chrono::steady_clock::now());
  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  m_pendingFlushTimer = TimerManager::instance().start(m_pendingFlushTimer, delay, [this, aliveGuard]() {
    if (aliveGuard.expired()) {
      return;
    }
    m_pendingFlushTimer = 0;
    flushPending();
  });
}

void MediaOsd::flushPending() {
  if (!m_pendingContent.has_value()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (inCooldown(now)) {
    schedulePendingFlush(m_cooldownUntil);
    return;
  }
  OsdContent content = std::move(*m_pendingContent);
  m_pendingContent.reset();
  pushTrackContent(content);
}

void MediaOsd::onMprisChanged(const MprisService& service) {
  const auto activePlayerOpt = service.activePlayer();
  if (activePlayerOpt.has_value()) {
    const auto& activePlayer = activePlayerOpt.value();
    const MediaOsdData osdData = {.title = activePlayer.title, .artist = joinedArtists(activePlayer.artists)};

    // First snapshot seeds the baseline; it is not a user-visible transition.
    if (!m_hasData) {
      m_lastData = osdData;
      m_hasData = true;
    } else if (activePlayer.playbackStatus == "Playing" && osdData != m_lastData) {
      m_lastData = osdData;
      pushTrackContent(makeMprisContent(osdData));
    }
  }

  // Volume is tracked per player rather than for the active one only: turning down a background
  // player is exactly when its name matters. listPlayers() is the blacklist-filtered view.
  const auto players = service.listPlayers();
  for (const auto& player : players) {
    const auto it = m_lastVolumes.find(player.busName);
    if (it == m_lastVolumes.end()) {
      m_lastVolumes.emplace(player.busName, player.volume);
      continue;
    }
    if (std::abs(player.volume - it->second) <= kVolumeChangeEpsilon) {
      continue;
    }
    it->second = player.volume;
    if (m_overlay != nullptr) {
      m_overlay->show(makeVolumeContent(player.identity, player.volume));
    }
  }

  std::erase_if(m_lastVolumes, [&players](const auto& entry) {
    return std::ranges::none_of(players, [&entry](const MprisPlayerInfo& player) {
      return player.busName == entry.first;
    });
  });
}
