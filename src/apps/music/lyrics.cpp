#include "apps/music/lyrics.h"

#include "apps/music/library.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <glib.h>
#include <sstream>

namespace okaru::music {

  namespace {

    namespace fs = std::filesystem;

    // "[mm:ss.xx]" o "[mm:ss]" al principio de `line`: segundos y largo de la marca; -1 si no es una marca de tiempo.
    std::pair<double, std::size_t> timestamp(std::string_view line) {
      if (line.size() < 6 || line[0] != '[') {
        return {-1.0, 0};
      }
      const std::size_t close = line.find(']');
      const std::size_t colon = line.find(':');
      if (close == std::string_view::npos || colon == std::string_view::npos || colon > close) {
        return {-1.0, 0};
      }
      int minutes = 0;
      if (std::from_chars(line.data() + 1, line.data() + colon, minutes).ec != std::errc{}) {
        return {-1.0, 0};
      }
      double seconds = 0.0;
      const std::string rest(line.substr(colon + 1, close - colon - 1));
      std::istringstream in(rest);
      in.imbue(std::locale::classic());
      if (!(in >> seconds)) {
        return {-1.0, 0};
      }
      return {minutes * 60.0 + seconds, close + 1};
    }

  } // namespace

  Lyrics parseLyrics(const std::string& text) {
    Lyrics lyrics;
    std::vector<LyricLine> timed;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      std::string_view rest = line;
      std::vector<double> times;
      for (auto [time, length] = timestamp(rest); length > 0; std::tie(time, length) = timestamp(rest)) {
        times.push_back(time);
        rest.remove_prefix(length);
      }
      // Etiquetas de cabecera ([ar:…], [ti:…], [length:…]) sin texto: fuera.
      if (times.empty() && rest.starts_with('[') && rest.ends_with(']') && rest.find(':') != std::string_view::npos) {
        continue;
      }
      while (rest.starts_with(' ')) {
        rest.remove_prefix(1);
      }
      for (double time : times) {
        timed.push_back({time, std::string(rest)});
      }
      if (times.empty()) {
        lyrics.lines.push_back({-1.0, std::string(rest)});
      }
    }
    if (!timed.empty()) {
      std::ranges::stable_sort(timed, {}, &LyricLine::time);
      lyrics.lines = std::move(timed);
      lyrics.synced = true;
    }
    // Sin líneas vacías al principio ni al final.
    while (!lyrics.lines.empty() && lyrics.lines.back().text.empty()) {
      lyrics.lines.pop_back();
    }
    const auto first = std::ranges::find_if(lyrics.lines, [](const LyricLine& l) { return !l.text.empty(); });
    if (!lyrics.synced) {
      lyrics.lines.erase(lyrics.lines.begin(), first);
    }
    return lyrics;
  }

  long currentLine(const Lyrics& lyrics, double seconds) {
    if (!lyrics.synced) {
      return -1;
    }
    const auto it = std::ranges::upper_bound(lyrics.lines, seconds, {}, &LyricLine::time);
    return static_cast<long>(it - lyrics.lines.begin()) - 1;
  }

  std::string sidecarLyrics(const fs::path& audio) {
    for (const char* ext : {".lrc", ".LRC", ".txt"}) {
      fs::path candidate = audio;
      candidate.replace_extension(ext);
      std::ifstream in(candidate, std::ios::binary);
      if (in) {
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
      }
    }
    return {};
  }

  fs::path lyricsCachePath(const Track& track) {
    // Por artista + título + duración: la misma canción en otra carpeta reusa la descarga.
    const std::string key = std::format("{}\n{}\n{}", sortKey(track.artist), sortKey(track.title), std::lround(track.duration));
    return fs::path(g_get_user_cache_dir()) / "okaru" / "music" / "lyrics"
         / std::format("{:016x}.lrc", std::hash<std::string>{}(key));
  }

} // namespace okaru::music
