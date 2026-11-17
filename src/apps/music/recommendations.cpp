#include "apps/music/recommendations.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <utility>

namespace okaru::music {

  std::vector<std::size_t> recentAlbums(const Model& model, std::size_t count) {
    std::vector<std::pair<std::int64_t, std::size_t>> dated;
    for (std::size_t a = 0; a < model.albums.size(); ++a) {
      std::int64_t newest = 0;
      for (std::size_t t : model.albums[a].tracks) {
        newest = std::max(newest, model.tracks[t].mtime); // las de servidores no tienen fecha (0)
      }
      if (newest > 0) {
        dated.emplace_back(newest, a);
      }
    }
    std::ranges::sort(dated, std::greater{});
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < dated.size() && i < count; ++i) {
      out.push_back(dated[i].second);
    }
    return out;
  }

  std::vector<std::size_t> rediscoverAlbums(const Model& model, std::size_t count, std::uint32_t seed) {
    std::vector<std::size_t> all(model.albums.size());
    std::iota(all.begin(), all.end(), 0);
    std::shuffle(all.begin(), all.end(), std::mt19937(seed));
    all.resize(std::min(count, all.size()));
    return all;
  }

} // namespace okaru::music
