#include "apps/music/servers.h"

#include "apps/common/okaru_app.h"
#include "apps/music/library.h"
#include "apps/music/youtube.h"
#include "core/log.h"

#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <unordered_set>
#include <glib.h>
#include <libsecret/secret.h>
#include <nlohmann/json.hpp>

namespace okaru::music {

  namespace {

    constexpr Logger kLog("music-servers");
    constexpr int kJellyfinPage = 2000;
    constexpr int kSubsonicPage = 500;
    constexpr const char* kClient = "Valandy";
    constexpr const char* kVersion = "1.0.0";
    constexpr const char* kYtOrigin = "https://music.youtube.com";
    constexpr const char* kYtClientVersion = "1.20251110.01.00";
    constexpr const char* kYtUserAgent = "Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0";
    constexpr const char* kYtSongsOnly = "EgWKAQIIAWoQEAMQBBAJEAoQBRAREBAQFQ%3D%3D"; // filtro «Canciones» de la búsqueda
    constexpr int kYtMaxPages = 200; // tope de páginas por sincronización (unas 20 000 canciones)

    const SecretSchema* schema() {
      static const SecretSchema kSchema = {
          "dev.okaru.Music",
          SECRET_SCHEMA_NONE,
          {{"server", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
          0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
      };
      return &kSchema;
    }

    std::string escape(const std::string& text) {
      gchar* out = g_uri_escape_string(text.c_str(), nullptr, false);
      std::string result = out != nullptr ? out : "";
      g_free(out);
      return result;
    }

    std::string md5(const std::string& text) {
      gchar* sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, text.c_str(), static_cast<gssize>(text.size()));
      std::string out = sum != nullptr ? sum : "";
      g_free(sum);
      return out;
    }

    std::string str(const nlohmann::json& object, const char* key) {
      const auto it = object.find(key);
      return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    int num(const nlohmann::json& object, const char* key) {
      const auto it = object.find(key);
      return it != object.end() && it->is_number() ? it->get<int>() : 0;
    }

    // Una sola cadena con los valores de una lista JSON ("Rock; Pop").
    std::string joined(const nlohmann::json& object, const char* key) {
      std::string out;
      const auto it = object.find(key);
      if (it != object.end() && it->is_array()) {
        for (const auto& value : *it) {
          if (value.is_string()) {
            out += (out.empty() ? "" : "; ") + value.get<std::string>();
          }
        }
      }
      return out;
    }

    Track jellyfinTrack(const Server& server, const nlohmann::json& item) {
      Track t;
      const std::string album = str(item, "AlbumId");
      t.path = remotePrefix(server) + (album.empty() ? "_" : album) + "/" + str(item, "Id");
      t.title = str(item, "Name");
      t.artist = joined(item, "Artists");
      t.albumArtist = str(item, "AlbumArtist");
      t.album = str(item, "Album");
      t.genre = joined(item, "Genres");
      t.year = num(item, "ProductionYear");
      t.disc = num(item, "ParentIndexNumber");
      t.number = num(item, "IndexNumber");
      if (const auto ticks = item.find("RunTimeTicks"); ticks != item.end() && ticks->is_number()) {
        t.duration = ticks->get<double>() / 10'000'000.0;
      }
      return t;
    }

    Track subsonicTrack(const Server& server, const nlohmann::json& song) {
      Track t;
      const std::string album = str(song, "albumId");
      t.path = remotePrefix(server) + (album.empty() ? "_" : album) + "/" + str(song, "id");
      t.title = str(song, "title");
      t.artist = str(song, "artist");
      t.album = str(song, "album");
      t.genre = str(song, "genre");
      t.year = num(song, "year");
      t.disc = num(song, "discNumber");
      t.number = num(song, "track");
      t.duration = num(song, "duration");
      return t;
    }

    // subsonic-response, o null si el servidor respondió con error.
    const nlohmann::json* subsonicBody(const nlohmann::json& json, std::string* error = nullptr) {
      const auto it = json.find("subsonic-response");
      if (it == json.end() || !it->is_object()) {
        return nullptr;
      }
      if (str(*it, "status") != "ok") {
        if (error != nullptr && it->contains("error")) {
          *error = str((*it)["error"], "message");
        }
        return nullptr;
      }
      return &*it;
    }

  } // namespace

  std::string remotePrefix(const Server& server) {
    const char* scheme = server.kind == ServerKind::Jellyfin ? "jf://" : server.kind == ServerKind::Subsonic ? "sub://" : "yt://";
    return scheme + server.id + "/";
  }

  bool isRemote(const std::string& path) {
    return path.starts_with("jf://") || path.starts_with("sub://") || path.starts_with("yt://");
  }

  std::string normalizeUrl(std::string url) {
    url.erase(0, url.find_first_not_of(" \t"));
    url.erase(url.find_last_not_of(" \t") + 1);
    while (url.ends_with('/')) {
      url.pop_back();
    }
    if (!url.empty() && !url.contains("://")) {
      url = "https://" + url;
    }
    return url;
  }

  bool parseRemote(const std::string& path, RemoteRef& out) {
    const auto scheme = path.find("://");
    if (!isRemote(path) || scheme == std::string::npos) {
      return false;
    }
    const std::string rest = path.substr(scheme + 3);
    const auto first = rest.find('/');
    const auto second = first == std::string::npos ? std::string::npos : rest.find('/', first + 1);
    if (second == std::string::npos) {
      return false;
    }
    out.server = rest.substr(0, first);
    out.album = rest.substr(first + 1, second - first - 1);
    out.item = rest.substr(second + 1);
    return !out.server.empty() && !out.item.empty();
  }

  // ── Llavero ──────────────────────────────────────────────────────────────────────────────────────

  std::string loadSecret(const std::string& serverId) {
    GError* error = nullptr;
    gchar* value = secret_password_lookup_sync(schema(), nullptr, &error, "server", serverId.c_str(), nullptr);
    if (error != nullptr) {
      kLog.warn("keyring: {}", error->message);
      g_error_free(error);
    }
    std::string out = value != nullptr ? value : "";
    secret_password_free(value);
    return out;
  }

  bool saveSecret(const std::string& serverId, const std::string& label, const std::string& secret) {
    GError* error = nullptr;
    const bool ok = secret_password_store_sync(
        schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), secret.c_str(), nullptr, &error, "server", serverId.c_str(),
        nullptr
    );
    if (error != nullptr) {
      kLog.warn("keyring: {}", error->message);
      g_error_free(error);
    }
    return ok;
  }

  void forgetSecret(const std::string& serverId) {
    GError* error = nullptr;
    secret_password_clear_sync(schema(), nullptr, &error, "server", serverId.c_str(), nullptr);
    if (error != nullptr) {
      g_error_free(error);
    }
  }

  // ── Servidores ───────────────────────────────────────────────────────────────────────────────────

  struct RemoteSources::YtSync {
    Server server;
    SyncFn done;
    std::deque<std::string> pending;
    std::vector<Track> tracks;
    std::unordered_set<std::string> seen;
    int pages = 0;
  };

  RemoteSources::RemoteSources(apps::OkaruApp& app, std::string deviceId) : m_deviceId(std::move(deviceId)) {
    app.addPollSource(&m_source);
  }

  RemoteSources::~RemoteSources() { *m_alive = false; }

  void RemoteSources::setServers(std::vector<Server> servers) {
    m_servers = std::move(servers);
    for (const Server& server : m_servers) {
      // shortcut: lectura bloqueante del llavero al abrir (unos ms, sin pedir nada si ya está desbloqueado); pasar a
      // la API asíncrona de libsecret si con el llavero bloqueado traba el arranque.
      if (!m_secrets.contains(server.id)) {
        m_secrets[server.id] = loadSecret(server.id);
      }
    }
    writeYtdlCookies();
  }

  const Server* RemoteSources::find(const std::string& id) const {
    for (const Server& server : m_servers) {
      if (server.id == id) {
        return &server;
      }
    }
    return nullptr;
  }

  std::string RemoteSources::jellyfinAuth(const std::string& token) const {
    std::string header = std::format(
        "Authorization: MediaBrowser Client=\"{}\", Device=\"OKARU\", DeviceId=\"{}\", Version=\"{}\"", kClient,
        m_deviceId, kVersion
    );
    if (!token.empty()) {
      header += std::format(", Token=\"{}\"", token);
    }
    return header;
  }

  std::string RemoteSources::subsonicQuery(const Server& server) const {
    // Autenticación por token: md5(contraseña + sal) y la sal; la contraseña nunca viaja.
    const auto it = m_secrets.find(server.id);
    const std::string password = it != m_secrets.end() ? it->second : "";
    const std::string salt = std::format("{:08x}{:08x}", g_random_int(), g_random_int());
    return std::format(
        "u={}&t={}&s={}&v=1.16.1&c={}&f=json", escape(server.user), md5(password + salt), salt, kClient
    );
  }

  void RemoteSources::connect(Server server, const std::string& password, ConnectFn done) {
    server.url = normalizeUrl(server.url);
    if (server.kind == ServerKind::Subsonic) {
      m_secrets[server.id] = password;
      const std::string url = std::format("{}/rest/ping.view?{}", server.url, subsonicQuery(server));
      m_http.request({.url = url, .followRedirects = true}, [this, server, password, done = std::move(done), alive = m_alive](HttpResponse r) {
        if (!*alive) {
          return;
        }
        std::string error;
        const auto json = nlohmann::json::parse(r.body, nullptr, false);
        if (!r.transportOk || subsonicBody(json, &error) == nullptr) {
          m_secrets.erase(server.id);
          done(false, r.transportOk ? (error.empty() ? std::format("HTTP {}", r.status) : error) : "unreachable", server);
          return;
        }
        saveSecret(server.id, "Valandy · " + server.name, password);
        done(true, {}, server);
      });
      return;
    }
    const nlohmann::json body{{"Username", server.user}, {"Pw", password}};
    m_http.request(
        {.method = "POST",
         .url = server.url + "/Users/AuthenticateByName",
         .headers = {jellyfinAuth({}), "Content-Type: application/json"},
         .body = body.dump(),
         .followRedirects = true},
        [this, server, done = std::move(done), alive = m_alive](HttpResponse r) mutable {
          if (!*alive) {
            return;
          }
          const auto json = nlohmann::json::parse(r.body, nullptr, false);
          const std::string token = json.is_object() ? str(json, "AccessToken") : "";
          if (!r.transportOk || r.status != 200 || token.empty()) {
            done(false, r.transportOk ? std::format("HTTP {}", r.status) : "unreachable", server);
            return;
          }
          server.userId = json.contains("User") ? str(json["User"], "Id") : "";
          m_secrets[server.id] = token;
          saveSecret(server.id, "Valandy · " + server.name, token);
          done(true, {}, server);
        }
    );
  }

  void RemoteSources::sync(const Server& server, SyncFn done) {
    auto out = std::make_shared<std::vector<Track>>();
    if (server.kind == ServerKind::YouTube) {
      // «Me gusta», luego las listas y los álbumes guardados (sus ids se agregan a la fila al leerlos).
      auto sync = std::make_shared<YtSync>();
      sync->server = server;
      sync->done = std::move(done);
      sync->pending = {"FEmusic_liked_videos", "FEmusic_liked_playlists", "FEmusic_liked_albums"};
      ytNext(std::move(sync));
      return;
    }
    if (server.kind == ServerKind::Jellyfin) {
      syncJellyfin(server, out, 0, std::move(done));
    } else {
      syncSubsonicSearch(server, out, 0, std::move(done));
    }
  }

  void RemoteSources::syncJellyfin(const Server& server, std::shared_ptr<std::vector<Track>> out, int start, SyncFn done) {
    const auto token = m_secrets.find(server.id);
    const std::string url = std::format(
        "{}/Users/{}/Items?IncludeItemTypes=Audio&Recursive=true&Fields=Genres&EnableImages=false&StartIndex={}&Limit={}",
        server.url, server.userId, start, kJellyfinPage
    );
    m_http.request(
        {.url = url, .headers = {jellyfinAuth(token != m_secrets.end() ? token->second : "")}, .followRedirects = true},
        [this, server, out, start, done = std::move(done), alive = m_alive](HttpResponse r) mutable {
          if (!*alive) {
            return;
          }
          const auto json = nlohmann::json::parse(r.body, nullptr, false);
          if (!r.transportOk || r.status != 200 || !json.is_object() || !json.contains("Items")) {
            kLog.warn("jellyfin {}: sync failed (HTTP {})", server.name, r.status);
            done(false, {});
            return;
          }
          for (const auto& item : json["Items"]) {
            out->push_back(jellyfinTrack(server, item));
          }
          const int total = num(json, "TotalRecordCount");
          const int next = start + static_cast<int>(json["Items"].size());
          if (json["Items"].empty() || next >= total) {
            done(true, std::move(*out));
            return;
          }
          syncJellyfin(server, out, next, std::move(done));
        }
    );
  }

  void RemoteSources::syncSubsonicSearch(
      const Server& server, std::shared_ptr<std::vector<Track>> out, int offset, SyncFn done
  ) {
    // search3 con consulta vacía devuelve todas las canciones (Navidrome y OpenSubsonic); si no, por álbumes.
    const std::string url = std::format(
        "{}/rest/search3.view?query=&artistCount=0&albumCount=0&songCount={}&songOffset={}&{}", server.url,
        kSubsonicPage, offset, subsonicQuery(server)
    );
    m_http.request({.url = url, .followRedirects = true}, [this, server, out, offset, done = std::move(done), alive = m_alive](HttpResponse r) mutable {
      if (!*alive) {
        return;
      }
      const auto json = nlohmann::json::parse(r.body, nullptr, false);
      const auto* body = r.transportOk ? subsonicBody(json) : nullptr;
      const nlohmann::json* songs = nullptr;
      if (body != nullptr && body->contains("searchResult3") && (*body)["searchResult3"].contains("song")) {
        songs = &(*body)["searchResult3"]["song"];
      }
      if (songs == nullptr || !songs->is_array() || songs->empty()) {
        if (offset == 0) {
          syncSubsonicAlbums(server, out, std::make_shared<std::vector<std::string>>(), 0, std::move(done));
        } else {
          done(true, std::move(*out));
        }
        return;
      }
      for (const auto& song : *songs) {
        out->push_back(subsonicTrack(server, song));
      }
      if (static_cast<int>(songs->size()) < kSubsonicPage) {
        done(true, std::move(*out));
        return;
      }
      syncSubsonicSearch(server, out, offset + kSubsonicPage, std::move(done));
    });
  }

  void RemoteSources::syncSubsonicAlbums(
      const Server& server, std::shared_ptr<std::vector<Track>> out, std::shared_ptr<std::vector<std::string>> albums,
      int offset, SyncFn done
  ) {
    const std::string url = std::format(
        "{}/rest/getAlbumList2.view?type=alphabeticalByName&size={}&offset={}&{}", server.url, kSubsonicPage, offset,
        subsonicQuery(server)
    );
    m_http.request({.url = url, .followRedirects = true}, [this, server, out, albums, offset, done = std::move(done), alive = m_alive](HttpResponse r) mutable {
      if (!*alive) {
        return;
      }
      const auto json = nlohmann::json::parse(r.body, nullptr, false);
      const auto* body = r.transportOk ? subsonicBody(json) : nullptr;
      if (body == nullptr) {
        done(false, {});
        return;
      }
      std::size_t got = 0;
      if (body->contains("albumList2") && (*body)["albumList2"].contains("album")) {
        for (const auto& album : (*body)["albumList2"]["album"]) {
          albums->push_back(str(album, "id"));
          ++got;
        }
      }
      if (got == static_cast<std::size_t>(kSubsonicPage)) {
        syncSubsonicAlbums(server, out, albums, offset + kSubsonicPage, std::move(done));
        return;
      }
      syncSubsonicAlbum(server, out, albums, 0, std::move(done));
    });
  }

  void RemoteSources::syncSubsonicAlbum(
      const Server& server, std::shared_ptr<std::vector<Track>> out, std::shared_ptr<std::vector<std::string>> albums,
      std::size_t index, SyncFn done
  ) {
    if (index >= albums->size()) {
      done(true, std::move(*out));
      return;
    }
    const std::string url = std::format(
        "{}/rest/getAlbum.view?id={}&{}", server.url, escape((*albums)[index]), subsonicQuery(server)
    );
    m_http.request({.url = url, .followRedirects = true}, [this, server, out, albums, index, done = std::move(done), alive = m_alive](HttpResponse r) mutable {
      if (!*alive) {
        return;
      }
      const auto json = nlohmann::json::parse(r.body, nullptr, false);
      if (const auto* body = r.transportOk ? subsonicBody(json) : nullptr;
          body != nullptr && body->contains("album") && (*body)["album"].contains("song")) {
        for (const auto& song : (*body)["album"]["song"]) {
          out->push_back(subsonicTrack(server, song));
        }
      }
      syncSubsonicAlbum(server, out, albums, index + 1, std::move(done));
    });
  }

  std::string RemoteSources::streamUrl(const std::string& path) const {
    RemoteRef ref;
    const Server* server = parseRemote(path, ref) ? find(ref.server) : nullptr;
    const auto secret = server != nullptr ? m_secrets.find(server->id) : m_secrets.end();
    if (server != nullptr && server->kind == ServerKind::YouTube) {
      // Lo resuelve yt-dlp dentro de mpv (con las cookies de la sesión, si hay).
      return "ytdl://https://music.youtube.com/watch?v=" + ref.item;
    }
    if (server == nullptr || secret == m_secrets.end() || secret->second.empty()) {
      return {};
    }
    if (server->kind == ServerKind::Jellyfin) {
      // El archivo tal cual (mpv lo abre en cualquier formato), sin recodificar.
      return std::format("{}/Audio/{}/stream?static=true&api_key={}", server->url, ref.item, secret->second);
    }
    return std::format("{}/rest/stream.view?id={}&{}", server->url, escape(ref.item), subsonicQuery(*server));
  }

  std::string RemoteSources::coverUrl(const std::string& path) const {
    RemoteRef ref;
    const Server* server = parseRemote(path, ref) ? find(ref.server) : nullptr;
    if (server == nullptr) {
      return {};
    }
    const std::string& id = ref.album != "_" ? ref.album : ref.item;
    if (server->kind == ServerKind::YouTube) {
      for (const std::string& key : {ref.item, ref.album}) {
        if (const auto it = m_thumbs.find(key); it != m_thumbs.end() && !it->second.empty()) {
          return ytCoverSize(it->second, 544);
        }
      }
      return "https://i.ytimg.com/vi/" + ref.item + "/hqdefault.jpg"; // sin la de la sincronización (otra sesión)
    }
    if (server->kind == ServerKind::Jellyfin) {
      return std::format("{}/Items/{}/Images/Primary?maxHeight=512&quality=90", server->url, id);
    }
    return std::format("{}/rest/getCoverArt.view?id={}&size=512&{}", server->url, escape(id), subsonicQuery(*server));
  }

  void RemoteSources::forget(const std::string& serverId) {
    m_secrets.erase(serverId);
    forgetSecret(serverId);
    std::erase_if(m_servers, [&](const Server& s) { return s.id == serverId; });
    writeYtdlCookies();
  }

  // ── YouTube Music ─────────────────────────────────────────────────────────────────────────────────

  void RemoteSources::setYouTubeSession(const Server& server, const std::string& cookies) {
    m_secrets[server.id] = cookies;
    saveSecret(server.id, "Valandy: " + server.name, cookies);
    if (!std::ranges::contains(m_servers, server.id, &Server::id)) {
      m_servers.push_back(server);
    }
    writeYtdlCookies();
  }

  void RemoteSources::writeYtdlCookies() {
    // Las de la primera cuenta con sesión; el archivo vive en XDG_RUNTIME_DIR (memoria, solo del usuario).
    namespace fs = std::filesystem;
    const fs::path file = fs::path(g_get_user_runtime_dir()) / "okaru-music" / "yt-cookies.txt";
    std::error_code ec;
    for (const Server& server : m_servers) {
      const auto secret = m_secrets.find(server.id);
      if (server.kind != ServerKind::YouTube || secret == m_secrets.end() || !hasYouTubeSession(secret->second)) {
        continue;
      }
      fs::create_directories(file.parent_path(), ec);
      fs::permissions(file.parent_path(), fs::perms::owner_all, ec);
      {
        std::ofstream out(file, std::ios::trunc);
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, ec);
        out << netscapeCookies(secret->second);
      }
      m_ytdlCookies = file.string();
      return;
    }
    fs::remove(file, ec);
    m_ytdlCookies.clear();
  }

  void RemoteSources::ytRequest(
      const Server& server, const std::string& endpoint, const std::string& extra, nlohmann::json body,
      std::function<void(bool ok, nlohmann::json response)> done
  ) {
    const char* const* languages = g_get_language_names();
    const std::string hl = languages != nullptr && languages[0] != nullptr ? std::string(languages[0]).substr(0, 2) : "en";
    body["context"]["client"] = {{"clientName", "WEB_REMIX"}, {"clientVersion", kYtClientVersion}, {"hl", hl == "C" ? "en" : hl}};
    std::vector<std::string> headers{
        "Content-Type: application/json", std::string("Origin: ") + kYtOrigin, std::string("X-Origin: ") + kYtOrigin,
        std::string("Referer: ") + kYtOrigin + "/", std::string("User-Agent: ") + kYtUserAgent, "X-Goog-AuthUser: 0",
    };
    if (const auto secret = m_secrets.find(server.id); secret != m_secrets.end() && hasYouTubeSession(secret->second)) {
      headers.push_back("Cookie: " + secret->second);
      headers.push_back("Authorization: " + sapisidAuthorization(secret->second, static_cast<long long>(std::time(nullptr))));
    }
    m_http.request(
        {.method = "POST",
         .url = std::format("{}/youtubei/v1/{}?prettyPrint=false{}", kYtOrigin, endpoint, extra),
         .headers = std::move(headers),
         .body = body.dump(),
         .followRedirects = true},
        [done = std::move(done), alive = m_alive](HttpResponse r) {
          if (!*alive) {
            return;
          }
          auto json = nlohmann::json::parse(r.body, nullptr, false);
          const bool ok = r.transportOk && r.status == 200 && json.is_object();
          if (!ok) {
            kLog.warn("youtube music: HTTP {}", r.status);
          }
          done(ok, ok ? std::move(json) : nlohmann::json());
        }
    );
  }

  Track RemoteSources::ytTrack(const Server& server, const YtTrack& yt) {
    Track t;
    t.path = remotePrefix(server) + (yt.albumId.empty() ? "_" : yt.albumId) + "/" + yt.videoId;
    t.title = yt.title;
    t.artist = yt.artist;
    t.albumArtist = yt.artist.substr(0, yt.artist.find(", ")); // el álbum se agrupa por el primer artista
    t.album = yt.album;
    t.duration = yt.duration;
    if (!yt.thumbnail.empty()) {
      m_thumbs[yt.videoId] = yt.thumbnail;
      if (!yt.albumId.empty()) {
        m_thumbs.try_emplace(yt.albumId, yt.thumbnail);
      }
    }
    return t;
  }

  void RemoteSources::ytNext(std::shared_ptr<YtSync> sync) {
    if (sync->pending.empty()) {
      auto done = std::move(sync->done);
      done(true, std::move(sync->tracks));
      return;
    }
    std::string id = std::move(sync->pending.front());
    sync->pending.pop_front();
    ytBrowse(std::move(sync), std::move(id), {}, true, 0);
  }

  void RemoteSources::ytBrowse(std::shared_ptr<YtSync> sync, std::string browseId, std::string token, bool tokenInBody, int page) {
    nlohmann::json body = nlohmann::json::object();
    std::string extra;
    if (token.empty()) {
      body["browseId"] = browseId;
    } else if (tokenInBody) {
      body["continuation"] = token;
    } else {
      extra = std::format("&ctoken={0}&continuation={0}&type=next", token);
    }
    const Server server = sync->server;
    ytRequest(server, "browse", extra, std::move(body), [this, sync, browseId, page](bool ok, nlohmann::json response) mutable {
      ++sync->pages;
      if (!ok) {
        // Sin la primera página de «me gusta» la sesión no sirve: se avisa. Lo demás se salta.
        if (browseId == "FEmusic_liked_videos" && page == 0) {
          auto done = std::move(sync->done);
          done(false, {});
          return;
        }
        ytNext(std::move(sync));
        return;
      }
      if (browseId == "FEmusic_liked_playlists" || browseId == "FEmusic_liked_albums") {
        const bool playlists = browseId == "FEmusic_liked_playlists";
        for (std::string& id : parseYtShelfIds(response, playlists ? "VL" : "MPREb")) {
          if (id != "VLLM") { // «Música que me gusta»: ya está en FEmusic_liked_videos
            sync->pending.push_back(std::move(id));
          }
        }
      } else {
        // En un álbum las filas no traen artista, álbum ni carátula: van las del encabezado.
        const bool album = browseId.starts_with("MPREb");
        const YtHeader header = album && page == 0 ? parseYtHeader(response) : YtHeader{};
        for (YtTrack& yt : parseYtTracks(response)) {
          if (album) {
            if (!header.title.empty()) {
              yt.album = header.title;
              yt.albumId = browseId;
            }
            if (!yt.artistLinked && !header.artist.empty()) {
              yt.artist = header.artist;
            }
            if (yt.thumbnail.empty()) {
              yt.thumbnail = header.thumbnail;
            }
          }
          if (sync->seen.insert(yt.videoId).second) {
            sync->tracks.push_back(ytTrack(sync->server, yt));
          }
        }
      }
      const YtContinuation next = parseYtContinuation(response);
      if (!next.token.empty() && sync->pages < kYtMaxPages) {
        ytBrowse(std::move(sync), browseId, next.token, next.inBody, page + 1);
      } else {
        ytNext(std::move(sync));
      }
    });
  }

  void RemoteSources::searchYouTube(const Server& server, const std::string& query, SearchFn done) {
    ytRequest(
        server, "search", "", {{"query", query}, {"params", kYtSongsOnly}},
        [this, server, done = std::move(done)](bool ok, nlohmann::json response) {
          std::vector<Track> tracks;
          if (ok) {
            for (const YtTrack& yt : parseYtTracks(response)) {
              tracks.push_back(ytTrack(server, yt));
            }
          }
          done(std::move(tracks));
        }
    );
  }

} // namespace okaru::music
