#pragma once

// OKARU: servidores de música de Valandy (Jellyfin y Subsonic: Navidrome, Gonic, Airsonic…). Su catálogo (solo los
// datos, no la música) se sincroniza a la biblioteca con rutas propias ("jf://servidor/álbum/pista",
// "sub://servidor/álbum/pista"), así álbumes, artistas, búsqueda, listas y cola funcionan igual que con lo local;
// al reproducir, la ruta se convierte en la URL de streaming. El token de Jellyfin y la contraseña de Subsonic van
// al llavero del sistema (libsecret), nunca a music.toml.

#include "net/http_client.h"
#include "net/http_client_poll_source.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace okaru::apps {
  class OkaruApp;
}

namespace okaru::music {

  struct Track;
  struct YtShelf;
  struct YtHeader;

  // YouTube Music se trata como un servidor más: su «secreto» son las cookies de la sesión (ver youtube.h).
  enum class ServerKind : std::uint8_t { Jellyfin, Subsonic, YouTube };

  struct Server {
    std::string id; // propio, estable (forma parte de las rutas de sus canciones)
    ServerKind kind = ServerKind::Jellyfin;
    std::string name;
    std::string url; // sin "/" final
    std::string user;
    std::string userId; // Jellyfin
  };

  // Prefijo de las rutas de las canciones de un servidor ("jf://id/" o "sub://id/").
  [[nodiscard]] std::string remotePrefix(const Server& server);
  [[nodiscard]] bool isRemote(const std::string& path);
  // Dirección escrita por el usuario → sin espacios ni "/" final, con https:// si no trae esquema.
  [[nodiscard]] std::string normalizeUrl(std::string url);
  // Partes de una ruta remota: servidor, álbum (o "_") y pista.
  struct RemoteRef {
    std::string server;
    std::string album;
    std::string item;
  };
  [[nodiscard]] bool parseRemote(const std::string& path, RemoteRef& out);

  // Llavero del sistema (libsecret, esquema dev.okaru.Music). Bloqueantes: se llaman pocas veces.
  [[nodiscard]] std::string loadSecret(const std::string& serverId);
  bool saveSecret(const std::string& serverId, const std::string& label, const std::string& secret);
  void forgetSecret(const std::string& serverId);

  class RemoteSources {
  public:
    using ConnectFn = std::function<void(bool ok, std::string error, Server server)>;
    using SyncFn = std::function<void(bool ok, std::vector<Track> tracks)>;
    using SearchFn = std::function<void(std::vector<Track> tracks)>;

    RemoteSources(apps::OkaruApp& app, std::string deviceId);
    ~RemoteSources();

    RemoteSources(const RemoteSources&) = delete;
    RemoteSources& operator=(const RemoteSources&) = delete;

    void setServers(std::vector<Server> servers);
    [[nodiscard]] const std::vector<Server>& servers() const noexcept { return m_servers; }
    // Inicia sesión (Jellyfin: usuario/contraseña → token; Subsonic: ping) y guarda el secreto en el llavero.
    void connect(Server server, const std::string& password, ConnectFn done);
    // Todo el catálogo de canciones del servidor, por páginas.
    void sync(const Server& server, SyncFn done);
    // Lo que mpv abre para una ruta remota (vacío si no es remota o falta el secreto).
    [[nodiscard]] std::string streamUrl(const std::string& path) const;
    // Carátula del álbum (o de la pista) en el servidor.
    [[nodiscard]] std::string coverUrl(const std::string& path) const;
    void forget(const std::string& serverId);
    // YouTube Music: guarda las cookies de una sesión recién iniciada (llavero y memoria).
    void setYouTubeSession(const Server& server, const std::string& cookies);
    // Canciones del catálogo de YouTube Music (no solo de la biblioteca).
    void searchYouTube(const Server& server, const std::string& query, SearchFn done);
    // Recomendaciones: los estantes de la portada de YouTube Music (varias páginas).
    void youTubeHome(const Server& server, std::function<void(bool ok, std::vector<YtShelf> shelves)> done);
    // Canciones de un álbum ("MPREb_…") o una lista ("VL…"; las automáticas "VLRD…" van por la radio).
    void youTubeTracks(const Server& server, const std::string& browseId, SearchFn done);
    // Radio de una canción: ella primero y luego las que YouTube Music le pone después.
    void youTubeRadio(const Server& server, const std::string& videoId, SearchFn done);
    // Archivo de cookies para yt-dlp (en XDG_RUNTIME_DIR, solo del usuario); vacío sin sesión.
    [[nodiscard]] const std::string& ytdlCookies() const noexcept { return m_ytdlCookies; }

  private:
    [[nodiscard]] const Server* find(const std::string& id) const;
    [[nodiscard]] std::string jellyfinAuth(const std::string& token) const;
    [[nodiscard]] std::string subsonicQuery(const Server& server) const;
    void syncJellyfin(const Server& server, std::shared_ptr<std::vector<Track>> out, int start, SyncFn done);
    void syncSubsonicSearch(const Server& server, std::shared_ptr<std::vector<Track>> out, int offset, SyncFn done);
    void syncSubsonicAlbums(
        const Server& server, std::shared_ptr<std::vector<Track>> out, std::shared_ptr<std::vector<std::string>> albums,
        int offset, SyncFn done
    );
    struct YtSync;
    void ytRequest(const Server& server, const std::string& endpoint, const std::string& extra, nlohmann::json body,
                   std::function<void(bool ok, nlohmann::json response)> done);
    void ytNext(std::shared_ptr<YtSync> sync);
    void ytHomePage(const Server& server, std::shared_ptr<std::vector<YtShelf>> shelves, std::string token, int page,
                    std::function<void(bool ok, std::vector<YtShelf> shelves)> done);
    void ytPages(const Server& server, std::string browseId, std::string token, bool inBody, int page,
                 std::shared_ptr<std::vector<Track>> out, struct YtHeader header, SearchFn done);
    void ytBrowse(std::shared_ptr<YtSync> sync, std::string browseId, std::string token, bool tokenInBody, int page);
    [[nodiscard]] Track ytTrack(const Server& server, const struct YtTrack& yt);
    void writeYtdlCookies();
    void syncSubsonicAlbum(
        const Server& server, std::shared_ptr<std::vector<Track>> out, std::shared_ptr<std::vector<std::string>> albums,
        std::size_t index, SyncFn done
    );

    HttpClient m_http;
    HttpClientPollSource m_source{m_http};
    std::string m_deviceId;
    std::vector<Server> m_servers;
    std::unordered_map<std::string, std::string> m_secrets; // id → token, contraseña o cookies (en memoria)
    std::unordered_map<std::string, std::string> m_thumbs;  // YouTube: videoId o albumId → URL de la carátula
    std::string m_ytdlCookies;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace okaru::music
