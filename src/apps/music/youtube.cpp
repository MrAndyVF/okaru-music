#include "apps/music/youtube.h"

#include "core/deferred_call.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <glib.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <regex>
#include <spawn.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_set>

extern char** environ;

namespace okaru::music {

  namespace fs = std::filesystem;
  using nlohmann::json;

  namespace {

    constexpr Logger kLog("music-youtube");
    constexpr const char* kSignInUrl =
        "https://accounts.google.com/ServiceLogin?service=youtube&continue=https%3A%2F%2Fmusic.youtube.com%2F";
    constexpr const char* kOrigin = "https://music.youtube.com";
    constexpr auto kPollInterval = std::chrono::milliseconds(1500);
    constexpr int kTimeoutTicks = 15 * 60 * 1000 / 1500; // 15 minutos para iniciar sesión

    struct Candidate {
      const char* id;
      const char* name;
      std::array<const char*, 2> binaries;
      bool chromium;
    };
    constexpr std::array<Candidate, 7> kCandidates{{
        {"zen", "Zen", {"zen-browser", nullptr}, false},
        {"firefox", "Firefox", {"firefox", nullptr}, false},
        {"librewolf", "LibreWolf", {"librewolf", nullptr}, false},
        {"chrome", "Google Chrome", {"google-chrome-stable", "google-chrome"}, true},
        {"chromium", "Chromium", {"chromium", nullptr}, true},
        {"brave", "Brave", {"brave", "brave-browser"}, true},
        {"vivaldi", "Vivaldi", {"vivaldi-stable", "vivaldi"}, true},
    }};

    fs::path loginRoot() { return fs::path(g_get_user_runtime_dir()) / "okaru-music"; }

    // El navegador puede tener la base abierta (y a medio escribir en el -wal): se lee una copia.
    std::vector<std::pair<std::string, std::string>> queryCopy(
        const fs::path& db, const char* sql, const std::function<std::pair<std::string, std::string>(sqlite3_stmt*)>& row
    ) {
      std::vector<std::pair<std::string, std::string>> out;
      std::error_code ec;
      if (!fs::exists(db, ec)) {
        return out;
      }
      const fs::path copy = db.parent_path() / ".valandy-read.sqlite";
      fs::copy_file(db, copy, fs::copy_options::overwrite_existing, ec);
      if (ec) {
        return out;
      }
      for (const char* suffix : {"-wal", "-journal"}) {
        fs::remove(fs::path(copy.string() + suffix), ec);
        if (fs::exists(fs::path(db.string() + suffix), ec)) {
          fs::copy_file(fs::path(db.string() + suffix), fs::path(copy.string() + suffix), fs::copy_options::overwrite_existing, ec);
        }
      }
      sqlite3* handle = nullptr;
      if (sqlite3_open_v2(copy.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) == SQLITE_OK) {
          while (sqlite3_step(stmt) == SQLITE_ROW) {
            out.push_back(row(stmt));
          }
        }
        sqlite3_finalize(stmt);
      }
      sqlite3_close(handle);
      for (const char* suffix : {"", "-wal", "-journal", "-shm"}) {
        fs::remove(fs::path(copy.string() + suffix), ec);
      }
      return out;
    }

    std::string column(sqlite3_stmt* stmt, int index) {
      const auto* text = sqlite3_column_blob(stmt, index);
      return text != nullptr ? std::string(static_cast<const char*>(text), static_cast<std::size_t>(sqlite3_column_bytes(stmt, index)))
                             : std::string();
    }

    // Cabecera "a=b; c=d" sin repetir nombres (la primera gana: la consulta pone primero el dominio general).
    std::string joinCookies(const std::vector<std::pair<std::string, std::string>>& cookies) {
      std::string header;
      std::unordered_set<std::string> seen;
      for (const auto& [name, value] : cookies) {
        if (name.empty() || value.empty() || !seen.insert(name).second) {
          continue;
        }
        if (!header.empty()) {
          header += "; ";
        }
        header += name + "=" + value;
      }
      return header;
    }

    // Chromium con --password-store=basic cifra con una clave fija ("peanuts"): AES-128-CBC, prefijo "v10".
    std::string decryptChromium(const std::string& blob, bool hashedDomain) {
      if (!blob.starts_with("v10") || blob.size() <= 3) {
        return {};
      }
      std::array<unsigned char, 16> key{};
      PKCS5_PBKDF2_HMAC_SHA1("peanuts", 7, reinterpret_cast<const unsigned char*>("saltysalt"), 9, 1, 16, key.data());
      std::array<unsigned char, 16> iv{};
      iv.fill(' ');
      std::string out(blob.size(), '\0');
      int written = 0;
      int last = 0;
      EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
      const bool ok = ctx != nullptr && EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key.data(), iv.data()) == 1
          && EVP_DecryptUpdate(
                 ctx, reinterpret_cast<unsigned char*>(out.data()), &written,
                 reinterpret_cast<const unsigned char*>(blob.data() + 3), static_cast<int>(blob.size() - 3)
             ) == 1
          && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + written, &last) == 1;
      EVP_CIPHER_CTX_free(ctx);
      if (!ok) {
        return {};
      }
      out.resize(static_cast<std::size_t>(written + last));
      // Desde la versión 24 de la base, el valor va precedido por el SHA-256 del dominio (32 bytes).
      if (hashedDomain) {
        out.erase(0, std::min<std::size_t>(32, out.size()));
      }
      return out;
    }

    const json* path(const json& node, std::initializer_list<std::string_view> keys) {
      const json* current = &node;
      for (const auto key : keys) {
        if (!current->is_object()) {
          return nullptr;
        }
        const auto it = current->find(key);
        if (it == current->end()) {
          return nullptr;
        }
        current = &*it;
      }
      return current;
    }

    std::string text(const json* node) { return node != nullptr && node->is_string() ? node->get<std::string>() : std::string(); }

    // Todos los objetos bajo una clave, en orden de documento.
    void collect(const json& node, std::string_view key, std::vector<const json*>& out) {
      if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
          if (it.key() == key) {
            out.push_back(&it.value());
          }
          collect(it.value(), key, out);
        }
      } else if (node.is_array()) {
        for (const auto& child : node) {
          collect(child, key, out);
        }
      }
    }

    const json* first(const json& node, std::string_view key) {
      std::vector<const json*> found;
      collect(node, key, found);
      return found.empty() ? nullptr : found.front();
    }

    std::string lastThumbnail(const json& node) {
      const json* thumbs = first(node, "thumbnails");
      if (thumbs == nullptr || !thumbs->is_array() || thumbs->empty()) {
        return {};
      }
      return text(path(thumbs->back(), {"url"}));
    }

    std::string pageType(const json& run) {
      return text(path(run, {"navigationEndpoint", "browseEndpoint", "browseEndpointContextSupportedConfigs",
                             "browseEndpointContextMusicConfig", "pageType"}));
    }

    bool isSeparator(const std::string& value) {
      return value == " • " || value == " & " || value == ", " || value == " y " || value == " and " || value.empty();
    }

  } // namespace

  std::vector<Browser> installedBrowsers() {
    std::vector<Browser> out;
    for (const Candidate& candidate : kCandidates) {
      for (const char* binary : candidate.binaries) {
        if (binary == nullptr) {
          continue;
        }
        if (gchar* found = g_find_program_in_path(binary); found != nullptr) {
          out.push_back({.id = candidate.id, .name = candidate.name, .binary = found, .chromium = candidate.chromium});
          g_free(found);
          break;
        }
      }
    }
    return out;
  }

  std::string readFirefoxCookies(const fs::path& profile) {
    return joinCookies(queryCopy(
        profile / "cookies.sqlite",
        "SELECT name, value FROM moz_cookies WHERE host = '.youtube.com' OR host LIKE '%.youtube.com' "
        "ORDER BY length(host)",
        [](sqlite3_stmt* stmt) { return std::pair{column(stmt, 0), column(stmt, 1)}; }
    ));
  }

  std::string readChromiumCookies(const fs::path& profile) {
    const fs::path db = profile / "Default" / "Cookies";
    const auto meta = queryCopy(db, "SELECT value, '' FROM meta WHERE key = 'version'", [](sqlite3_stmt* stmt) {
      return std::pair{column(stmt, 0), std::string()};
    });
    const bool hashed = !meta.empty() && std::strtol(meta.front().first.c_str(), nullptr, 10) >= 24;
    auto rows = queryCopy(
        db,
        "SELECT name, value, encrypted_value FROM cookies WHERE host_key = '.youtube.com' OR host_key LIKE "
        "'%.youtube.com' ORDER BY length(host_key)",
        [hashed](sqlite3_stmt* stmt) {
          std::string value = column(stmt, 1);
          if (value.empty()) {
            value = decryptChromium(column(stmt, 2), hashed);
          }
          return std::pair{column(stmt, 0), value};
        }
    );
    return joinCookies(rows);
  }

  std::string cookieValue(const std::string& header, const std::string& name) {
    std::size_t start = 0;
    while (start < header.size()) {
      const std::size_t end = std::min(header.find("; ", start), header.size());
      const std::string_view pair(header.data() + start, end - start);
      if (pair.size() > name.size() && pair.starts_with(name) && pair[name.size()] == '=') {
        return std::string(pair.substr(name.size() + 1));
      }
      start = end + 2;
    }
    return {};
  }

  bool hasYouTubeSession(const std::string& header) {
    const bool apisid = !cookieValue(header, "SAPISID").empty() || !cookieValue(header, "__Secure-3PAPISID").empty();
    const bool sid = !cookieValue(header, "SID").empty() || !cookieValue(header, "__Secure-3PSID").empty();
    return apisid && sid;
  }

  std::string sapisidAuthorization(const std::string& cookies, long long timestamp) {
    const auto hash = [&](const std::string& value) {
      const std::string input = std::format("{} {} {}", timestamp, value, kOrigin);
      gchar* sha = g_compute_checksum_for_string(G_CHECKSUM_SHA1, input.c_str(), static_cast<gssize>(input.size()));
      std::string out = std::format("{}_{}", timestamp, sha);
      g_free(sha);
      return out;
    };
    std::string sapisid = cookieValue(cookies, "SAPISID");
    const std::string first = cookieValue(cookies, "__Secure-1PAPISID");
    const std::string third = cookieValue(cookies, "__Secure-3PAPISID");
    if (sapisid.empty()) {
      sapisid = third;
    }
    if (sapisid.empty()) {
      return {};
    }
    std::string out = "SAPISIDHASH " + hash(sapisid);
    if (!first.empty()) {
      out += " SAPISID1PHASH " + hash(first);
    }
    if (!third.empty()) {
      out += " SAPISID3PHASH " + hash(third);
    }
    return out;
  }

  std::string netscapeCookies(const std::string& header) {
    // Vencen en un año: yt-dlp descarta las de sesión (sin vencimiento) al cargar el archivo.
    const long long expires = static_cast<long long>(std::time(nullptr)) + 365LL * 24 * 3600;
    std::string out = "# Netscape HTTP Cookie File\n";
    std::size_t start = 0;
    while (start < header.size()) {
      const std::size_t end = std::min(header.find("; ", start), header.size());
      const std::string pair = header.substr(start, end - start);
      if (const auto eq = pair.find('='); eq != std::string::npos && eq > 0) {
        out += std::format(".youtube.com\tTRUE\t/\tTRUE\t{}\t{}\t{}\n", expires, pair.substr(0, eq), pair.substr(eq + 1));
      }
      start = end + 2;
    }
    return out;
  }

  double parseYtDuration(const std::string& value) {
    static const std::regex kDuration(R"(^(?:(\d+):)?(\d{1,2}):(\d{2})$)");
    std::smatch m;
    if (!std::regex_match(value, m, kDuration)) {
      return 0.0;
    }
    const int hours = m[1].matched ? std::stoi(m[1].str()) : 0;
    return hours * 3600.0 + std::stoi(m[2].str()) * 60.0 + std::stoi(m[3].str());
  }

  std::string ytCoverSize(const std::string& url, int size) {
    static const std::regex kSize(R"(=w\d+-h\d+)");
    return std::regex_replace(url, kSize, std::format("=w{}-h{}", size, size));
  }

  std::vector<YtTrack> parseYtTracks(const json& response) {
    std::vector<const json*> rows;
    collect(response, "musicResponsiveListItemRenderer", rows);
    std::vector<YtTrack> out;
    std::unordered_set<std::string> seen;
    for (const json* row : rows) {
      const json* columns = path(*row, {"flexColumns"});
      if (columns == nullptr || !columns->is_array() || columns->empty()) {
        continue;
      }
      const auto runsOf = [](const json& col) -> const json* {
        const json* runs = path(col, {"musicResponsiveListItemFlexColumnRenderer", "text", "runs"});
        return runs != nullptr && runs->is_array() ? runs : nullptr;
      };
      const json* titleRuns = runsOf((*columns)[0]);
      YtTrack track;
      track.videoId = text(path(*row, {"playlistItemData", "videoId"}));
      if (track.videoId.empty() && titleRuns != nullptr && !titleRuns->empty()) {
        track.videoId = text(path((*titleRuns)[0], {"navigationEndpoint", "watchEndpoint", "videoId"}));
      }
      if (track.videoId.empty() || titleRuns == nullptr || titleRuns->empty() || !seen.insert(track.videoId).second) {
        continue; // un artista, un álbum o una lista en los resultados, no una canción
      }
      track.title = text(path((*titleRuns)[0], {"text"}));
      std::vector<std::string> artists;
      std::string plain; // primer texto suelto (artista sin enlace)
      for (std::size_t c = 1; c < columns->size(); ++c) {
        const json* runs = runsOf((*columns)[c]);
        if (runs == nullptr) {
          continue;
        }
        for (const json& run : *runs) {
          const std::string value = text(path(run, {"text"}));
          const std::string type = pageType(run);
          if (type == "MUSIC_PAGE_TYPE_ARTIST" || type == "MUSIC_PAGE_TYPE_USER_CHANNEL") {
            artists.push_back(value);
          } else if (type == "MUSIC_PAGE_TYPE_ALBUM") {
            track.album = value;
            track.albumId = text(path(run, {"navigationEndpoint", "browseEndpoint", "browseId"}));
          } else if (const double d = parseYtDuration(value); d > 0.0) {
            track.duration = d;
          } else if (c == 1 && plain.empty() && !isSeparator(value)) {
            plain = value;
          }
        }
      }
      track.artistLinked = !artists.empty();
      track.artist = artists.empty() ? plain : artists.front();
      for (std::size_t i = 1; i < artists.size(); ++i) {
        track.artist += ", " + artists[i];
      }
      if (track.duration <= 0.0) {
        if (const json* fixed = path(*row, {"fixedColumns"}); fixed != nullptr && fixed->is_array() && !fixed->empty()) {
          const json* runs = path((*fixed)[0], {"musicResponsiveListItemFixedColumnRenderer", "text", "runs"});
          if (runs != nullptr && runs->is_array() && !runs->empty()) {
            track.duration = parseYtDuration(text(path((*runs)[0], {"text"})));
          }
        }
      }
      if (const json* thumb = path(*row, {"thumbnail"}); thumb != nullptr) {
        track.thumbnail = lastThumbnail(*thumb);
      }
      out.push_back(std::move(track));
    }
    return out;
  }

  YtHeader parseYtHeader(const json& response) {
    YtHeader header;
    for (const char* key : {"musicResponsiveHeaderRenderer", "musicDetailHeaderRenderer", "musicImmersiveHeaderRenderer"}) {
      const json* node = first(response, key);
      if (node == nullptr) {
        continue;
      }
      if (const json* runs = path(*node, {"title", "runs"}); runs != nullptr && runs->is_array() && !runs->empty()) {
        header.title = text(path((*runs)[0], {"text"}));
      }
      for (const char* line : {"straplineTextOne", "subtitle"}) {
        const json* runs = path(*node, {line, "runs"});
        if (runs == nullptr || !runs->is_array()) {
          continue;
        }
        for (const json& run : *runs) {
          if (pageType(run) == "MUSIC_PAGE_TYPE_ARTIST" || std::string_view(line) == "straplineTextOne") {
            header.artist = text(path(run, {"text"}));
            break;
          }
        }
        if (!header.artist.empty()) {
          break;
        }
      }
      header.thumbnail = lastThumbnail(*node);
      break;
    }
    return header;
  }

  std::vector<std::string> parseYtShelfIds(const json& response, const std::string& prefix) {
    std::vector<const json*> items;
    collect(response, "musicTwoRowItemRenderer", items);
    std::vector<std::string> out;
    for (const json* item : items) {
      const std::string id = text(path(*item, {"navigationEndpoint", "browseEndpoint", "browseId"}));
      if (id.starts_with(prefix) && !std::ranges::contains(out, id)) {
        out.push_back(id);
      }
    }
    return out;
  }

  YtContinuation parseYtContinuation(const json& response) {
    if (const json* item = first(response, "continuationItemRenderer"); item != nullptr) {
      if (std::string token = text(first(*item, "token")); !token.empty()) {
        return {.token = std::move(token), .inBody = true};
      }
    }
    if (const json* next = first(response, "nextContinuationData"); next != nullptr) {
      return {.token = text(path(*next, {"continuation"})), .inBody = false};
    }
    return {};
  }

  std::vector<YtTrack> parseYtPanel(const json& response) {
    std::vector<const json*> items;
    collect(response, "playlistPanelVideoRenderer", items);
    std::vector<YtTrack> out;
    std::unordered_set<std::string> seen;
    for (const json* item : items) {
      YtTrack track;
      track.videoId = text(path(*item, {"videoId"}));
      const json* title = path(*item, {"title", "runs"});
      if (!isVideoId(track.videoId) || title == nullptr || !title->is_array() || title->empty() || !seen.insert(track.videoId).second) {
        continue;
      }
      track.title = text(path((*title)[0], {"text"}));
      std::vector<std::string> artists;
      if (const json* runs = path(*item, {"longBylineText", "runs"}); runs != nullptr && runs->is_array()) {
        for (const json& run : *runs) {
          const std::string type = pageType(run);
          if (type == "MUSIC_PAGE_TYPE_ARTIST" || type == "MUSIC_PAGE_TYPE_USER_CHANNEL") {
            artists.push_back(text(path(run, {"text"})));
          } else if (type == "MUSIC_PAGE_TYPE_ALBUM") {
            track.album = text(path(run, {"text"}));
            track.albumId = text(path(run, {"navigationEndpoint", "browseEndpoint", "browseId"}));
          }
        }
        if (artists.empty() && !runs->empty()) {
          track.artist = text(path((*runs)[0], {"text"}));
        }
      }
      track.artistLinked = !artists.empty();
      for (const std::string& artist : artists) {
        track.artist += (track.artist.empty() ? "" : ", ") + artist;
      }
      if (const json* length = path(*item, {"lengthText", "runs"}); length != nullptr && length->is_array() && !length->empty()) {
        track.duration = parseYtDuration(text(path((*length)[0], {"text"})));
      }
      if (const json* thumb = path(*item, {"thumbnail"}); thumb != nullptr) {
        track.thumbnail = lastThumbnail(*thumb);
      }
      out.push_back(std::move(track));
    }
    return out;
  }

  std::vector<YtShelf> parseYtHome(const json& response) {
    std::vector<const json*> shelves;
    collect(response, "musicCarouselShelfRenderer", shelves);
    std::vector<YtShelf> out;
    for (const json* node : shelves) {
      YtShelf shelf;
      if (const json* runs = path(*node, {"header", "musicCarouselShelfBasicHeaderRenderer", "title", "runs"});
          runs != nullptr && runs->is_array()) {
        for (const json& run : *runs) {
          shelf.title += text(path(run, {"text"}));
        }
      }
      const json* contents = path(*node, {"contents"});
      if (contents == nullptr || !contents->is_array()) {
        continue;
      }
      for (const json& entry : *contents) {
        if (entry.contains("musicResponsiveListItemRenderer")) {
          // Fila de canción («Mixes de canciones»): la misma forma que la búsqueda.
          for (const YtTrack& track : parseYtTracks(entry)) {
            shelf.cards.push_back({.kind = YtCard::Kind::Song, .title = track.title,
                                   .subtitle = track.album.empty() ? track.artist : track.artist + " • " + track.album,
                                   .artist = track.artist, .album = track.album, .thumbnail = track.thumbnail,
                                   .videoId = track.videoId});
          }
          continue;
        }
        const json* item = path(entry, {"musicTwoRowItemRenderer"});
        if (item == nullptr) {
          continue;
        }
        YtCard card;
        if (const json* runs = path(*item, {"title", "runs"}); runs != nullptr && runs->is_array() && !runs->empty()) {
          card.title = text(path((*runs)[0], {"text"}));
        }
        if (const json* runs = path(*item, {"subtitle", "runs"}); runs != nullptr && runs->is_array()) {
          for (const json& run : *runs) {
            const std::string value = text(path(run, {"text"}));
            card.subtitle += value;
            const std::string type = pageType(run);
            if (type == "MUSIC_PAGE_TYPE_ARTIST" || type == "MUSIC_PAGE_TYPE_USER_CHANNEL") {
              card.artist += (card.artist.empty() ? "" : ", ") + value;
            } else if (type == "MUSIC_PAGE_TYPE_ALBUM") {
              card.album = value;
            }
          }
          // Sin artista enlazado: lo último del subtítulo («Canción • Artista»).
          if (card.artist.empty()) {
            const auto dot = card.subtitle.rfind(" • ");
            card.artist = dot == std::string::npos ? card.subtitle : card.subtitle.substr(dot + 5);
          }
        }
        if (const json* thumb = path(*item, {"thumbnailRenderer"}); thumb != nullptr) {
          card.thumbnail = lastThumbnail(*thumb);
        }
        const json* nav = path(*item, {"navigationEndpoint"});
        if (nav == nullptr) {
          continue;
        }
        card.videoId = text(path(*nav, {"watchEndpoint", "videoId"}));
        const std::string browse = text(path(*nav, {"browseEndpoint", "browseId"}));
        const std::string type = text(path(*nav, {"browseEndpoint", "browseEndpointContextSupportedConfigs",
                                                   "browseEndpointContextMusicConfig", "pageType"}));
        if (isVideoId(card.videoId)) {
          card.kind = YtCard::Kind::Song;
        } else if (type == "MUSIC_PAGE_TYPE_ALBUM" && !browse.empty()) {
          card.kind = YtCard::Kind::Album;
          card.browseId = browse;
        } else if (type == "MUSIC_PAGE_TYPE_PLAYLIST" && !browse.empty()) {
          card.kind = YtCard::Kind::Playlist;
          card.browseId = browse;
        } else {
          continue; // artistas, podcasts y lo demás: no se reproducen como una lista de canciones
        }
        shelf.cards.push_back(std::move(card));
      }
      if (!shelf.cards.empty()) {
        out.push_back(std::move(shelf));
      }
    }
    return out;
  }

  bool isVideoId(const std::string& id) {
    return id.size() == 11 && std::ranges::all_of(id, [](char c) {
             return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
           });
  }

  // ── URL directas ──────────────────────────────────────────────────────────────────────────────────

  namespace {

    // Salida estándar de un programa (sin shell); vacía si no terminó bien. `child` = su pid mientras corre.
    std::string capture(const std::vector<std::string>& args, std::atomic<pid_t>& child) {
      int fds[2];
      if (pipe2(fds, O_CLOEXEC) != 0) {
        return {};
      }
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
      posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
      std::vector<char*> argv;
      for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
      }
      argv.push_back(nullptr);
      pid_t pid = -1;
      const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      ::close(fds[1]);
      std::string out;
      if (rc == 0) {
        child = pid;
        std::array<char, 4096> buffer{};
        ssize_t n = 0;
        while ((n = ::read(fds[0], buffer.data(), buffer.size())) > 0) {
          out.append(buffer.data(), static_cast<std::size_t>(n));
        }
      }
      ::close(fds[0]);
      int status = 0;
      if (rc == 0) {
        waitpid(pid, &status, 0);
        child = -1;
      }
      return rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? out : std::string();
    }

  } // namespace

  StreamResolver::StreamResolver(ReadyFn ready) : m_ready(std::move(ready)), m_thread([this] { run(); }) {}

  StreamResolver::~StreamResolver() {
    {
      const std::lock_guard lock(m_mutex);
      m_stop = true;
    }
    *m_alive = false;
    m_wake.notify_all();
    if (const pid_t child = m_child; child > 0) {
      ::kill(child, SIGTERM); // no esperar a que yt-dlp termine para cerrar la app
    }
    m_thread.join();
  }

  void StreamResolver::setCookies(std::string file) {
    const std::lock_guard lock(m_mutex);
    m_cookies = std::move(file);
  }

  std::string StreamResolver::cached(const std::string& videoId) const {
    const std::lock_guard lock(m_mutex);
    const auto it = m_cache.find(videoId);
    return it != m_cache.end() && std::chrono::system_clock::now() < it->second.expires ? it->second.url : std::string();
  }

  void StreamResolver::prefetch(const std::vector<std::string>& videoIds) {
    const std::lock_guard lock(m_mutex);
    const auto now = std::chrono::system_clock::now();
    for (const std::string& id : videoIds) {
      const auto it = m_cache.find(id);
      if (!isVideoId(id) || m_pending.contains(id) || (it != m_cache.end() && now < it->second.expires)) {
        continue;
      }
      m_pending.insert(id);
      m_queue.push_back(id);
    }
    m_wake.notify_all();
  }

  void StreamResolver::forget(const std::string& url) {
    const std::lock_guard lock(m_mutex);
    std::erase_if(m_cache, [&](const auto& entry) { return entry.second.url == url; });
  }

  void StreamResolver::run() {
    static const std::regex kExpire(R"([?&]expire=(\d+))");
    while (true) {
      std::string id;
      std::string cookies;
      {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
        if (m_stop) {
          return;
        }
        id = std::move(m_queue.front());
        m_queue.pop_front();
        cookies = m_cookies;
      }
      std::vector<std::string> args{"yt-dlp", "-q", "--no-warnings", "--no-playlist", "-f", "bestaudio/best", "-g"};
      if (!cookies.empty()) {
        args.insert(args.end(), {"--cookies", cookies});
      }
      args.insert(args.end(), {"--", "https://music.youtube.com/watch?v=" + id});
      std::string url = capture(args, m_child);
      url = url.substr(0, url.find('\n'));
      std::smatch m;
      const bool ok = url.starts_with("https://") && std::regex_search(url, m, kExpire);
      std::unique_lock lock(m_mutex);
      if (ok) {
        // Recién resuelta YouTube la rechaza (403) unos 2 s: se ofrece después de 3.
        m_wake.wait_for(lock, std::chrono::seconds(3), [this] { return m_stop; });
        if (m_stop) {
          return;
        }
        const auto expires = std::chrono::system_clock::time_point(std::chrono::seconds(std::stoll(m[1].str())));
        m_cache[id] = {.url = url, .expires = expires - std::chrono::minutes(10)};
      } else {
        kLog.debug("yt-dlp could not resolve {}", id);
      }
      m_pending.erase(id);
      lock.unlock();
      if (ok) {
        DeferredCall::callLater([alive = m_alive, ready = m_ready, id] {
          if (*alive) {
            ready(id);
          }
        });
      }
    }
  }

  // ── Inicio de sesión ──────────────────────────────────────────────────────────────────────────────

  YouTubeLogin::~YouTubeLogin() { cancel(); }

  void YouTubeLogin::cleanLeftovers() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(loginRoot(), ec)) {
      if (entry.path().filename().string().starts_with("login-")) {
        fs::remove_all(entry.path(), ec);
      }
    }
  }

  bool YouTubeLogin::start(const Browser& browser, DoneFn done) {
    cancel();
    m_browser = browser;
    m_done = std::move(done);
    std::error_code ec;
    m_profile = loginRoot() / std::format("login-{:08x}", g_random_int());
    fs::create_directories(m_profile, ec);
    fs::permissions(loginRoot(), fs::perms::owner_all, ec);
    fs::permissions(m_profile, fs::perms::owner_all, ec);

    std::vector<std::string> args{browser.binary};
    if (browser.chromium) {
      args.insert(args.end(), {"--user-data-dir=" + m_profile.string(), "--password-store=basic", "--no-first-run",
                               "--no-default-browser-check", "--disable-sync", std::string("--app=") + kSignInUrl});
    } else {
      // Perfil nuevo sin pantallas de bienvenida ni telemetría: directo a la página de Google.
      std::ofstream(m_profile / "user.js") << "user_pref(\"browser.aboutwelcome.enabled\", false);\n"
                                              "user_pref(\"browser.startup.homepage_override.mstone\", \"ignore\");\n"
                                              "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n"
                                              "user_pref(\"datareporting.policy.dataSubmissionEnabled\", false);\n"
                                              "user_pref(\"toolkit.telemetry.reportingpolicy.firstRun\", false);\n"
                                              "user_pref(\"trailhead.firstrun.didSeeAboutWelcome\", true);\n"
                                              "user_pref(\"zen.welcome-screen.seen\", true);\n"
                                              "user_pref(\"signon.rememberSignons\", false);\n";
      args.insert(args.end(), {"--profile", m_profile.string(), "--no-remote", "--new-instance", kSignInUrl});
    }
    std::vector<char*> argv;
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    // Grupo de procesos propio: al terminar se cierran el navegador y todos sus hijos.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    const int rc = posix_spawn(&m_pid, argv[0], nullptr, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
      kLog.warn("could not launch {}: {}", browser.binary, std::strerror(rc));
      m_pid = -1;
      fs::remove_all(m_profile, ec);
      return false;
    }
    m_ticks = 0;
    m_poll.startRepeating(kPollInterval, [this] { poll(); });
    return true;
  }

  void YouTubeLogin::poll() {
    ++m_ticks;
    const std::string cookies = m_browser.chromium ? readChromiumCookies(m_profile) : readFirefoxCookies(m_profile);
    if (hasYouTubeSession(cookies)) {
      finish(true, cookies, {});
      return;
    }
    int status = 0;
    if (waitpid(m_pid, &status, WNOHANG) == m_pid) {
      m_pid = -1; // el usuario cerró el navegador sin terminar
      finish(false, {}, "closed");
      return;
    }
    if (m_ticks >= kTimeoutTicks) {
      finish(false, {}, "timeout");
    }
  }

  void YouTubeLogin::finish(bool ok, std::string cookies, std::string error) {
    m_poll.stop();
    if (m_pid > 0) {
      // Se le pide cerrar y, si no lo hace en 2 s, se termina: el perfil se borra enseguida.
      ::kill(-m_pid, SIGTERM);
      int status = 0;
      for (int i = 0; i < 20 && waitpid(m_pid, &status, WNOHANG) != m_pid; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      ::kill(-m_pid, SIGKILL);
      waitpid(m_pid, &status, 0); // ya terminado o recién matado: no bloquea; sin esto queda un zombi
      m_pid = -1;
    }
    std::error_code ec;
    if (!m_profile.empty()) {
      fs::remove_all(m_profile, ec);
      m_profile.clear();
    }
    if (auto done = std::move(m_done)) {
      m_done = nullptr;
      done(ok, std::move(cookies), std::move(error));
    }
  }

  void YouTubeLogin::cancel() {
    if (m_pid > 0 || !m_profile.empty()) {
      m_done = nullptr; // cancelado a propósito: no se avisa
      finish(false, {}, "cancelled");
    }
  }

} // namespace okaru::music
