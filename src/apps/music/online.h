#pragma once

// OKARU: lo que Valandy busca en internet, solo si los ajustes lo permiten: letras en LRCLIB y carátulas en
// MusicBrainz + Cover Art Archive (servicios abiertos, sin cuenta ni clave). Todo es asíncrono en el bucle de la app
// y queda en caché en disco, también lo que no se encontró (para no volver a preguntar en cada inicio).

#include "core/timer_manager.h"
#include "net/http_client.h"
#include "net/http_client_poll_source.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace okaru::apps {
  class OkaruApp;
}

namespace okaru::music {

  struct Track;

  class OnlineService {
  public:
    explicit OnlineService(apps::OkaruApp& app);
    ~OnlineService();

    OnlineService(const OnlineService&) = delete;
    OnlineService& operator=(const OnlineService&) = delete;

    // Letra de LRCLIB (sincronizada si la hay, si no plana) guardada en `cache`; `done` recibe el texto o vacío.
    void fetchLyrics(const Track& track, const std::filesystem::path& cache, std::function<void(std::string)> done);
    // Portada del álbum (o de la canción si no tiene álbum) guardada reducida en `target`; `done(true)` si se encontró.
    void fetchCover(const Track& track, const std::filesystem::path& target, std::function<void(bool)> done);
    // Ya se buscó y no estaba (marca `<archivo>.none`).
    [[nodiscard]] static bool knownMissing(const std::filesystem::path& cached);

  private:
    // MusicBrainz pide como mucho una consulta por segundo: van en fila.
    void enqueueMusicBrainz(std::function<void()> request);
    void pumpMusicBrainz();
    void tryCoverArt(std::shared_ptr<std::vector<std::string>> releases, std::size_t index,
                     std::filesystem::path target, std::function<void(bool)> done);

    HttpClient m_http;
    HttpClientPollSource m_source{m_http};
    std::deque<std::function<void()>> m_mbQueue;
    Timer m_mbTimer;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace okaru::music
