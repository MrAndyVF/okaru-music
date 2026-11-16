#include "apps/music/music_state.h"

#include "util/file_utils.h"

#include <algorithm>
#include <fstream>
#include <glib.h>
#include <toml++/toml.hpp>

namespace okaru::music {

  namespace fs = std::filesystem;

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
