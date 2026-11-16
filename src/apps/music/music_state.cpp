#include "apps/music/music_state.h"

#include "util/file_utils.h"

#include <algorithm>
#include <fstream>
#include <glib.h>
#include <toml++/toml.hpp>

namespace okaru::music {

  namespace fs = std::filesystem;

  namespace {

    void readSources(const toml::table* table, SourceOrder& sources) {
      if (table == nullptr) {
        return;
      }
      if (const auto* order = (*table)["order"].as_array(); order != nullptr && order->size() == kSourceCount) {
        SourceOrder parsed;
        std::array<bool, kSourceCount> seen{};
        bool valid = true;
        for (std::size_t i = 0; i < kSourceCount; ++i) {
          const auto value = (*order)[i].value_or<std::int64_t>(-1);
          valid = valid && value >= 0 && value < static_cast<std::int64_t>(kSourceCount) && !seen[static_cast<std::size_t>(value)];
          if (valid) {
            seen[static_cast<std::size_t>(value)] = true;
            parsed.order[i] = static_cast<Source>(value);
          }
        }
        if (valid) {
          sources.order = parsed.order;
        }
      }
      if (const auto* enabled = (*table)["enabled"].as_array(); enabled != nullptr) {
        for (std::size_t i = 0; i < kSourceCount && i < enabled->size(); ++i) {
          sources.enabled[i] = (*enabled)[i].value_or(true);
        }
      }
    }

    toml::table writeSources(const SourceOrder& sources) {
      toml::array order;
      toml::array enabled;
      for (std::size_t i = 0; i < kSourceCount; ++i) {
        order.push_back(static_cast<std::int64_t>(sources.order[i]));
        enabled.push_back(sources.enabled[i]);
      }
      return toml::table{{"order", std::move(order)}, {"enabled", std::move(enabled)}};
    }

  } // namespace

  void SourceOrder::move(std::size_t index, int delta) {
    const auto target = static_cast<long>(index) + delta;
    if (index < kSourceCount && target >= 0 && target < static_cast<long>(kSourceCount)) {
      std::swap(order[index], order[static_cast<std::size_t>(target)]);
    }
  }

  fs::path statePath() { return fs::path(FileUtils::stateDir()) / "music.toml"; }

  MusicState loadState(const fs::path& path) {
    MusicState state;
    toml::table root;
    try {
      root = toml::parse_file(path.string());
    } catch (const toml::parse_error&) {
      const char* music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
      const char* home = g_get_home_dir();
      state.folders.emplace_back(music != nullptr ? fs::path(music) : fs::path(home) / "Music");
      return state;
    }
    if (const auto* folders = root["folders"].as_array()) {
      for (const auto& node : *folders) {
        if (const auto folder = node.value<std::string>()) {
          state.folders.emplace_back(*folder);
        }
      }
    }
    state.volume = std::clamp(root["volume"].value_or(100.0), 0.0, 100.0);
    state.shuffle = root["shuffle"].value_or(false);
    const auto repeat = root["repeat"].value_or<std::int64_t>(0);
    state.repeat = repeat == 1 ? Repeat::All : repeat == 2 ? Repeat::One : Repeat::Off;
    const auto* sort = root["sort"].as_array();
    const auto* reverse = root["reverse"].as_array();
    for (std::size_t i = 0; i < state.sort.size(); ++i) {
      if (sort != nullptr && i < sort->size()) {
        state.sort[i] = static_cast<std::uint8_t>(std::clamp<std::int64_t>((*sort)[i].value_or<std::int64_t>(0), 0, 9));
      }
      if (reverse != nullptr && i < reverse->size()) {
        state.reverse[i] = (*reverse)[i].value_or(false);
      }
    }
    if (const auto* settings = root["settings"].as_table()) {
      auto& out = state.settings;
      out.crossfade = std::clamp<int>((*settings)["crossfade"].value_or(0), 0, 12);
      out.gapless = (*settings)["gapless"].value_or(true);
      const auto gain = (*settings)["replaygain"].value_or<std::int64_t>(0);
      out.replayGain = gain == 1 ? ReplayGain::Track : gain == 2 ? ReplayGain::Album : ReplayGain::Off;
      out.resume = (*settings)["resume"].value_or(true);
      out.scanOnStart = (*settings)["scan_on_start"].value_or(true);
      out.preferSynced = (*settings)["prefer_synced"].value_or(true);
      readSources((*settings)["lyrics"].as_table(), out.lyrics);
      readSources((*settings)["covers"].as_table(), out.covers);
    }
    if (const auto* queue = root["queue"].as_array()) {
      for (const auto& node : *queue) {
        if (const auto item = node.value<std::string>()) {
          state.queue.push_back(*item);
        }
      }
    }
    state.current = std::min<std::size_t>(
        static_cast<std::size_t>(std::max<std::int64_t>(0, root["current"].value_or<std::int64_t>(0))),
        state.queue.empty() ? 0 : state.queue.size() - 1
    );
    return state;
  }

  bool saveState(const MusicState& state, const fs::path& path) {
    toml::array folders;
    for (const auto& folder : state.folders) {
      folders.push_back(folder.string());
    }
    toml::array queue;
    for (const auto& item : state.queue) {
      queue.push_back(item);
    }
    toml::array sort;
    toml::array reverse;
    for (std::size_t i = 0; i < state.sort.size(); ++i) {
      sort.push_back(static_cast<std::int64_t>(state.sort[i]));
      reverse.push_back(state.reverse[i]);
    }
    const toml::table root{
        {"folders", std::move(folders)},
        {"volume", state.volume},
        {"shuffle", state.shuffle},
        {"repeat", static_cast<std::int64_t>(state.repeat)},
        {"queue", std::move(queue)},
        {"current", static_cast<std::int64_t>(state.current)},
        {"sort", std::move(sort)},
        {"reverse", std::move(reverse)},
        {"settings",
         toml::table{
             {"crossfade", state.settings.crossfade},
             {"gapless", state.settings.gapless},
             {"replaygain", static_cast<std::int64_t>(state.settings.replayGain)},
             {"resume", state.settings.resume},
             {"scan_on_start", state.settings.scanOnStart},
             {"prefer_synced", state.settings.preferSynced},
             {"lyrics", writeSources(state.settings.lyrics)},
             {"covers", writeSources(state.settings.covers)},
         }},
    };
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const auto tmp = fs::path(path.string() + ".tmp");
    {
      std::ofstream out(tmp, std::ios::trunc);
      if (!out) {
        return false;
      }
      out << root << '\n';
      if (!out) {
        return false;
      }
    }
    fs::rename(tmp, path, ec);
    return !ec;
  }

} // namespace okaru::music
