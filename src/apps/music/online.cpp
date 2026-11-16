#include "apps/music/online.h"

#include "apps/common/okaru_app.h"
#include "apps/music/library.h"
#include "core/log.h"

#include <cmath>
#include <format>
#include <fstream>
#include <glib.h>
#include <nlohmann/json.hpp>

namespace okaru::music {

  namespace {

    namespace fs = std::filesystem;
    constexpr Logger kLog("music-online");
    // MusicBrainz exige un User-Agent que identifique la aplicación.
    constexpr const char* kUserAgent = "User-Agent: Valandy/1.0 ( https://github.com/MrAndyVF/okaru-music )";
    constexpr auto kMusicBrainzGap = std::chrono::milliseconds(1100);

    std::string escape(const std::string& text) {
      gchar* out = g_uri_escape_string(text.c_str(), nullptr, false);
      std::string result = out != nullptr ? out : "";
      g_free(out);
      return result;
    }

    // Comillas fuera: el valor va entre comillas en la consulta de Lucene de MusicBrainz.
    std::string quoted(std::string text) {
      std::erase(text, '"');
      std::erase(text, '\\');
      return "\"" + text + "\"";
    }

    // El primer artista de "A, B feat. C": LRCLIB y MusicBrainz buscan mejor así.
    std::string mainArtist(const std::string& artist) {
      std::string out = artist;
      for (const char* sep : {", ", " feat. ", " ft. ", " & ", " x ", "; ", "/"}) {
        if (const auto at = out.find(sep); at != std::string::npos && at > 0) {
          out.erase(at);
        }
      }
      return out;
    }

    void markMissing(const fs::path& cached) {
      std::error_code ec;
      fs::create_directories(cached.parent_path(), ec);
      std::ofstream(cached.string() + ".none").put('\n');
    }

    bool writeText(const fs::path& path, const std::string& text) {
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << text;
      return static_cast<bool>(out);
    }

    // syncedLyrics si hay; si no plainLyrics; vacío si es instrumental o no trae nada.
    std::string lyricsFrom(const nlohmann::json& entry) {
      if (!entry.is_object()) {
        return {};
      }
      for (const char* key : {"syncedLyrics", "plainLyrics"}) {
        if (const auto it = entry.find(key); it != entry.end() && it->is_string() && !it->get<std::string>().empty()) {
          return it->get<std::string>();
        }
      }
      return {};
    }

  } // namespace

  OnlineService::OnlineService(apps::OkaruApp& app) { app.addPollSource(&m_source); }

  OnlineService::~OnlineService() { *m_alive = false; }

  bool OnlineService::knownMissing(const fs::path& cached) {
    std::error_code ec;
    return fs::exists(cached.string() + ".none", ec);
  }

  void OnlineService::fetchLyrics(const Track& track, const fs::path& cache, std::function<void(std::string)> done) {
    const std::string artist = mainArtist(track.artist);
    std::string url = std::format(
        "https://lrclib.net/api/get?artist_name={}&track_name={}", escape(artist), escape(track.title)
    );
    if (!track.album.empty()) {
      url += "&album_name=" + escape(track.album);
    }
    if (track.duration > 0.0) {
      url += std::format("&duration={}", std::lround(track.duration));
    }
    const std::string search = std::format(
        "https://lrclib.net/api/search?artist_name={}&track_name={}", escape(artist), escape(track.title)
    );
    auto finish = [cache, done = std::move(done), alive = m_alive](std::string text) {
      if (!*alive) {
        return;
      }
      if (text.empty()) {
        markMissing(cache);
      } else {
        writeText(cache, text);
      }
      done(std::move(text));
    };
    // Primero la coincidencia exacta (con álbum y duración); si no está, la búsqueda.
    m_http.request(
        {.url = url, .headers = {kUserAgent}, .followRedirects = true},
        [this, search, finish = std::move(finish), alive = m_alive](HttpResponse response) mutable {
          if (!*alive) {
            return;
          }
          if (!response.transportOk) {
            kLog.debug("lrclib unreachable");
            return; // sin red: no se marca como «no encontrada», se reintenta otro día
          }
          if (response.status == 200) {
            finish(lyricsFrom(nlohmann::json::parse(response.body, nullptr, false)));
            return;
          }
          m_http.request(
              {.url = search, .headers = {kUserAgent}, .followRedirects = true},
              [finish = std::move(finish)](HttpResponse found) mutable {
                if (!found.transportOk) {
                  return;
                }
                const auto list = nlohmann::json::parse(found.body, nullptr, false);
                std::string text;
                if (list.is_array()) {
                  for (const auto& entry : list) {
                    if (text = lyricsFrom(entry); !text.empty()) {
                      break;
                    }
                  }
                }
                finish(std::move(text));
              }
          );
        }
    );
  }

  void OnlineService::fetchCover(const Track& track, const fs::path& target, std::function<void(bool)> done) {
    const std::string artist = quoted(mainArtist(!track.albumArtist.empty() ? track.albumArtist : track.artist));
    // Con álbum, el lanzamiento; sin álbum, los lanzamientos donde está la canción.
    const bool byAlbum = !track.album.empty();
    const std::string query = byAlbum ? std::format("release:{} AND artist:{}", quoted(track.album), artist)
                                      : std::format("recording:{} AND artist:{}", quoted(track.title), artist);
    const std::string url = std::format(
        "https://musicbrainz.org/ws/2/{}/?query={}&fmt=json&limit=5", byAlbum ? "release" : "recording", escape(query)
    );
    enqueueMusicBrainz([this, url, byAlbum, target, done = std::move(done)]() mutable {
      m_http.request(
          {.url = url, .headers = {kUserAgent}, .followRedirects = true},
          [this, byAlbum, target, done = std::move(done), alive = m_alive](HttpResponse response) mutable {
            if (!*alive) {
              return;
            }
            if (!response.transportOk || response.status == 503) {
              done(false); // sin red o MusicBrainz saturado: se intenta en otro momento
              return;
            }
            auto releases = std::make_shared<std::vector<std::string>>();
            const auto json = nlohmann::json::parse(response.body, nullptr, false);
            const auto collect = [&](const nlohmann::json& list) {
              if (!list.is_array()) {
                return;
              }
              for (const auto& release : list) {
                // Solo coincidencias buenas: un álbum homónimo de otro artista no sirve.
                const int score = release.value("score", 100);
                if (release.contains("id") && score >= 90) {
                  releases->push_back(release["id"].get<std::string>());
                }
              }
            };
            if (byAlbum) {
              collect(json.value("releases", nlohmann::json::array()));
            } else if (json.contains("recordings") && json["recordings"].is_array()) {
              for (const auto& recording : json["recordings"]) {
                if (recording.value("score", 0) >= 90) {
                  collect(recording.value("releases", nlohmann::json::array()));
                }
              }
            }
            if (releases->empty()) {
              markMissing(target);
              done(false);
              return;
            }
            tryCoverArt(releases, 0, target, std::move(done));
          }
      );
    });
  }

  void OnlineService::tryCoverArt(
      std::shared_ptr<std::vector<std::string>> releases, std::size_t index, fs::path target,
      std::function<void(bool)> done
  ) {
    if (index >= releases->size() || index >= 5) {
      markMissing(target);
      done(false);
      return;
    }
    const fs::path raw = target.string() + ".download";
    m_http.download(
        std::format("https://coverartarchive.org/release/{}/front-500", (*releases)[index]), raw,
        [this, releases, index, target, raw, done = std::move(done), alive = m_alive](bool ok) mutable {
          if (!*alive) {
            return;
          }
          // Ese lanzamiento no tiene portada: el siguiente.
          if (!ok || !storeCover(raw.string(), target)) {
            std::error_code ec;
            fs::remove(raw, ec);
            tryCoverArt(releases, index + 1, target, std::move(done));
            return;
          }
          std::error_code ec;
          fs::remove(raw, ec);
          done(true);
        }
    );
  }

  void OnlineService::enqueueMusicBrainz(std::function<void()> request) {
    m_mbQueue.push_back(std::move(request));
    if (!m_mbTimer.active()) {
      pumpMusicBrainz();
    }
  }

  void OnlineService::pumpMusicBrainz() {
    if (m_mbQueue.empty()) {
      return;
    }
    auto request = std::move(m_mbQueue.front());
    m_mbQueue.pop_front();
    request();
    m_mbTimer.start(kMusicBrainzGap, [this] { pumpMusicBrainz(); });
  }

} // namespace okaru::music
