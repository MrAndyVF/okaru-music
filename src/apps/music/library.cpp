#include "apps/music/library.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "render/core/image_encoder.h"
#include "render/core/image_file_loader.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <glib.h>
#include <map>
#include <sqlite3.h>
#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tdebuglistener.h>
#include <taglib/tpropertymap.h>
#include <unordered_map>
#include <unordered_set>

namespace okaru::music {

  namespace {

    namespace fs = std::filesystem;
    constexpr Logger kLog("music");
    constexpr int kSchemaVersion = 1;
    constexpr int kCoverSize = 512;
    constexpr std::size_t kBatch = 500;
    constexpr std::size_t kProgressEvery = 25;

    // TagLib avisa por stderr de cada etiqueta rara que no va a guardar (nunca se guarda): fuera del registro.
    class QuietTagLib final : public TagLib::DebugListener {
    public:
      void printMessage(const TagLib::String& /*msg*/) override {}
    };

    const bool kQuietTagLib = [] {
      static QuietTagLib listener;
      TagLib::setDebugListener(&listener);
      return true;
    }();

    constexpr std::array<std::string_view, 22> kAudioExtensions{
        "mp3", "flac", "ogg", "oga", "opus", "m4a", "m4b", "aac", "alac", "wav", "wma",
        "aiff", "aif", "ape", "wv", "mka", "mpc", "spx", "dsf", "dff", "tta", "caf",
    };
    constexpr std::array<std::string_view, 8> kFolderCovers{"cover", "folder", "front", "album",
                                                            "Cover", "Folder", "Front", "Album"};
    constexpr std::array<std::string_view, 4> kImageExtensions{".jpg", ".jpeg", ".png", ".webp"};

    // Minúsculas, sin acentos (NFD sin marcas combinantes): "Canción" y "cancion" son lo mismo.
    std::string fold(std::string_view text) {
      gchar* lower = g_utf8_casefold(text.data(), static_cast<gssize>(text.size()));
      gchar* nfd = g_utf8_normalize(lower, -1, G_NORMALIZE_NFD);
      g_free(lower);
      std::string out;
      if (nfd == nullptr) {
        return out;
      }
      for (const gchar* p = nfd; *p != '\0'; p = g_utf8_next_char(p)) {
        const gunichar c = g_utf8_get_char(p);
        if (g_unichar_combining_class(c) == 0) {
          out.append(p, static_cast<std::size_t>(g_utf8_next_char(p) - p));
        }
      }
      g_free(nfd);
      return out;
    }

    int leadingInt(const std::string& text) {
      int value = 0;
      const char* begin = text.data();
      while (begin < text.data() + text.size() && *begin == ' ') {
        ++begin;
      }
      std::from_chars(begin, text.data() + text.size(), value);
      return value;
    }

    std::string join(const TagLib::StringList& values) {
      std::string out;
      for (const auto& value : values) {
        if (!out.empty()) {
          out += "; ";
        }
        out += value.to8Bit(true);
      }
      return out;
    }

    // Partes de un género guardado como "Rock; Pop" (o "Rock/Pop", común en ID3v1 migrado).
    std::vector<std::string> splitGenres(const std::string& genre) {
      std::vector<std::string> out;
      std::size_t start = 0;
      while (start <= genre.size()) {
        const std::size_t end = genre.find_first_of(";/", start);
        std::string part = genre.substr(start, end == std::string::npos ? std::string::npos : end - start);
        part.erase(0, part.find_first_not_of(' '));
        part.erase(part.find_last_not_of(' ') + 1);
        if (!part.empty() && std::ranges::find(out, part) == out.end()) {
          out.push_back(std::move(part));
        }
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
      return out;
    }

    std::string coverKey(const Track& track) {
      // Sin álbum, cada archivo tiene la suya.
      const std::string source = track.album.empty() ? track.path : fold(groupingArtist(track)) + "\n" + fold(track.album);
      return std::format("{:016x}", std::hash<std::string>{}(source));
    }

  } // namespace

  bool storeCover(const std::string& sourceImage, const fs::path& target) {
    auto image = loadImageFile(sourceImage, kCoverSize, true);
    if (!image) {
      return false;
    }
    const auto jpeg = encodeJpeg(image->rgba.data(), image->width, image->height, 90);
    if (jpeg.empty()) {
      return false;
    }
    const fs::path tmp = target.string() + ".part";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
      if (!out) {
        return false;
      }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    return !ec;
  }

  namespace {

    // ── SQLite ──────────────────────────────────────────────────────────────────────────────────────

    struct Db {
      sqlite3* handle = nullptr;
      ~Db() { sqlite3_close(handle); }
      bool exec(const char* sql) const {
        char* error = nullptr;
        if (sqlite3_exec(handle, sql, nullptr, nullptr, &error) != SQLITE_OK) {
          kLog.warn("sqlite: {}", error != nullptr ? error : "?");
          sqlite3_free(error);
          return false;
        }
        return true;
      }
    };

    struct Statement {
      sqlite3_stmt* handle = nullptr;
      Statement(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &handle, nullptr); }
      ~Statement() { sqlite3_finalize(handle); }
      Statement(const Statement&) = delete;
      Statement& operator=(const Statement&) = delete;
      [[nodiscard]] std::string text(int column) const {
        const auto* value = sqlite3_column_text(handle, column);
        return value != nullptr ? reinterpret_cast<const char*>(value) : "";
      }
      void bind(int index, const std::string& value) const {
        sqlite3_bind_text(handle, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
      }
    };

    bool openDb(Db& db, const fs::path& path) {
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      if (sqlite3_open(path.c_str(), &db.handle) != SQLITE_OK) {
        kLog.error("cannot open {}: {}", path.string(), sqlite3_errmsg(db.handle));
        return false;
      }
      sqlite3_busy_timeout(db.handle, 5000);
      int current = 0;
      {
        // Cerrada antes de seguir: una consulta abierta deja una transacción y WAL no se puede activar.
        Statement version(db.handle, "PRAGMA user_version");
        current = sqlite3_step(version.handle) == SQLITE_ROW ? sqlite3_column_int(version.handle, 0) : 0;
      }
      if (current != kSchemaVersion) {
        // Es una caché de etiquetas: con otro esquema se rehace leyendo los archivos.
        db.exec("DROP TABLE IF EXISTS tracks");
      }
      return db.exec("PRAGMA journal_mode=WAL")
          && db.exec("CREATE TABLE IF NOT EXISTS tracks ("
                     "id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, title TEXT, artist TEXT, album_artist TEXT,"
                     "album TEXT, genre TEXT, lyrics TEXT, cover TEXT, year INTEGER, disc INTEGER, number INTEGER,"
                     "duration REAL, mtime INTEGER, size INTEGER)")
          && db.exec(std::format("PRAGMA user_version={}", kSchemaVersion).c_str());
    }

    std::vector<Track> readAll(sqlite3* db) {
      std::vector<Track> tracks;
      Statement select(
          db, "SELECT id, path, title, artist, album_artist, album, genre, lyrics, cover, year, disc, number, duration,"
              " mtime, size FROM tracks"
      );
      while (sqlite3_step(select.handle) == SQLITE_ROW) {
        Track t;
        t.id = sqlite3_column_int64(select.handle, 0);
        t.path = select.text(1);
        t.title = select.text(2);
        t.artist = select.text(3);
        t.albumArtist = select.text(4);
        t.album = select.text(5);
        t.genre = select.text(6);
        t.lyrics = select.text(7);
        t.cover = select.text(8);
        t.year = sqlite3_column_int(select.handle, 9);
        t.disc = sqlite3_column_int(select.handle, 10);
        t.number = sqlite3_column_int(select.handle, 11);
        t.duration = sqlite3_column_double(select.handle, 12);
        t.mtime = sqlite3_column_int64(select.handle, 13);
        t.size = sqlite3_column_int64(select.handle, 14);
        tracks.push_back(std::move(t));
      }
      return tracks;
    }

    std::int64_t mtimeOf(const fs::directory_entry& entry, std::error_code& ec) {
      return std::chrono::duration_cast<std::chrono::seconds>(entry.last_write_time(ec).time_since_epoch()).count();
    }

  } // namespace

  // ── Modelo ────────────────────────────────────────────────────────────────────────────────────────

  std::string groupingArtist(const Track& track) {
    return !track.albumArtist.empty() ? track.albumArtist : track.artist;
  }

  std::string sortKey(std::string_view text) {
    std::string key = fold(text);
    for (std::string_view article : {"the ", "los ", "las ", "el ", "la "}) {
      if (key.starts_with(article) && key.size() > article.size()) {
        key.erase(0, article.size());
        break;
      }
    }
    return key;
  }

  bool isAudioFile(const fs::path& path) {
    std::string ext = path.extension().string();
    if (ext.size() < 2) {
      return false;
    }
    ext.erase(0, 1);
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::ranges::contains(kAudioExtensions, ext);
  }

  Model organize(std::vector<Track> tracks) {
    Model model;
    // Claves de orden calculadas una vez (fold es lo caro).
    struct Keys {
      std::string artist;
      std::string album;
      std::string title;
    };
    std::vector<Keys> keys(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      keys[i] = {sortKey(groupingArtist(tracks[i])), sortKey(tracks[i].album), sortKey(tracks[i].title)};
    }
    std::vector<std::size_t> order(tracks.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::ranges::sort(order, [&](std::size_t a, std::size_t b) {
      const Track& x = tracks[a];
      const Track& y = tracks[b];
      return std::tie(keys[a].artist, keys[a].album, x.disc, x.number, keys[a].title)
           < std::tie(keys[b].artist, keys[b].album, y.disc, y.number, keys[b].title);
    });
    model.tracks.reserve(tracks.size());
    std::vector<Keys> sortedKeys;
    sortedKeys.reserve(tracks.size());
    for (std::size_t i : order) {
      model.tracks.push_back(std::move(tracks[i]));
      sortedKeys.push_back(std::move(keys[i]));
    }

    // Álbumes: mismo nombre y mismo artista de álbum; sin artista de álbum, misma carpeta (así un recopilatorio no se
    // parte en uno por artista, y dos discos homónimos de artistas distintos no se juntan).
    std::map<std::pair<std::string, std::string>, std::size_t> albumIndex;
    for (std::size_t i = 0; i < model.tracks.size(); ++i) {
      const Track& t = model.tracks[i];
      std::string owner = !t.albumArtist.empty() ? fold(t.albumArtist)
          : t.album.empty()                      ? sortedKeys[i].artist
                                                 : fs::path(t.path).parent_path().string();
      auto [it, inserted] = albumIndex.try_emplace({sortedKeys[i].album, std::move(owner)}, model.albums.size());
      if (inserted) {
        model.albums.push_back({.title = t.album, .artist = groupingArtist(t), .cover = t.cover, .year = t.year});
      }
      Album& album = model.albums[it->second];
      // Pistas de artistas distintos sin artista de álbum: recopilatorio (artista vacío).
      if (t.albumArtist.empty() && !album.tracks.empty() && album.artist != t.artist) {
        album.artist.clear();
      }
      if (album.cover.empty()) {
        album.cover = t.cover;
      }
      album.year = std::max(album.year, t.year);
      album.duration += t.duration;
      album.tracks.push_back(i);
    }
    for (Album& album : model.albums) {
      std::ranges::stable_sort(album.tracks, [&](std::size_t a, std::size_t b) {
        return std::tie(model.tracks[a].disc, model.tracks[a].number) < std::tie(model.tracks[b].disc, model.tracks[b].number);
      });
    }

    // Artistas: cada álbum va con todos los artistas de sus pistas (un recopilatorio aparece en cada uno).
    std::map<std::string, std::size_t> artistIndex;
    for (std::size_t a = 0; a < model.albums.size(); ++a) {
      std::unordered_set<std::string> seen;
      for (std::size_t i : model.albums[a].tracks) {
        const std::string name = groupingArtist(model.tracks[i]);
        auto [it, inserted] = artistIndex.try_emplace(sortedKeys[i].artist, model.artists.size());
        if (inserted) {
          model.artists.push_back({.name = name});
        }
        Artist& artist = model.artists[it->second];
        ++artist.trackCount;
        if (seen.insert(it->first).second) {
          artist.albums.push_back(a);
        }
      }
    }
    std::vector<std::string> artistKeys(model.artists.size());
    for (const auto& [key, index] : artistIndex) {
      artistKeys[index] = key;
    }
    for (Artist& artist : model.artists) {
      std::ranges::stable_sort(artist.albums, [&](std::size_t a, std::size_t b) {
        return model.albums[a].year > model.albums[b].year;
      });
      for (std::size_t a : artist.albums) {
        if (!model.albums[a].cover.empty()) {
          artist.cover = model.albums[a].cover;
          break;
        }
      }
    }
    std::vector<std::size_t> artistOrder(model.artists.size());
    for (std::size_t i = 0; i < artistOrder.size(); ++i) {
      artistOrder[i] = i;
    }
    std::ranges::sort(artistOrder, [&](std::size_t a, std::size_t b) { return artistKeys[a] < artistKeys[b]; });
    std::vector<Artist> sortedArtists;
    sortedArtists.reserve(model.artists.size());
    for (std::size_t i : artistOrder) {
      sortedArtists.push_back(std::move(model.artists[i]));
    }
    model.artists = std::move(sortedArtists);

    std::map<std::string, Genre> genres;
    for (std::size_t i = 0; i < model.tracks.size(); ++i) {
      for (auto& name : splitGenres(model.tracks[i].genre)) {
        auto& genre = genres[sortKey(name)];
        if (genre.name.empty()) {
          genre.name = std::move(name);
        }
        genre.tracks.push_back(i);
      }
    }
    for (auto& [key, genre] : genres) {
      model.genres.push_back(std::move(genre));
    }
    return model;
  }

  namespace {

    // Índices 0..n-1 ordenados por `key(i)` (calculada una vez por elemento), estable; invertidos si `reverse`.
    template <typename KeyFn> std::vector<std::size_t> orderBy(std::size_t count, KeyFn key, bool reverse) {
      using Key = decltype(key(std::size_t{}));
      std::vector<Key> keys;
      keys.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        keys.push_back(key(i));
      }
      std::vector<std::size_t> order(count);
      for (std::size_t i = 0; i < count; ++i) {
        order[i] = i;
      }
      std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });
      if (reverse) {
        std::ranges::reverse(order);
      }
      return order;
    }

    std::vector<std::size_t> identity(std::size_t count, bool reverse) {
      return orderBy(count, [](std::size_t i) { return i; }, reverse);
    }

  } // namespace

  std::vector<std::size_t> sortedTracks(const Model& model, SongSort sort, bool reverse) {
    const auto& t = model.tracks;
    switch (sort) {
    case SongSort::Title:
      return orderBy(t.size(), [&](std::size_t i) { return sortKey(t[i].title); }, reverse);
    case SongSort::Album:
      return orderBy(t.size(), [&](std::size_t i) { return std::tuple(sortKey(t[i].album), t[i].disc, t[i].number); }, reverse);
    case SongSort::Duration:
      return orderBy(t.size(), [&](std::size_t i) { return t[i].duration; }, reverse);
    case SongSort::Added: // lo más nuevo primero
      return orderBy(t.size(), [&](std::size_t i) { return -t[i].mtime; }, reverse);
    case SongSort::Artist:
      break;
    }
    return identity(t.size(), reverse);
  }

  std::vector<std::size_t> sortedAlbums(const Model& model, AlbumSort sort, bool reverse) {
    const auto& albums = model.albums;
    switch (sort) {
    case AlbumSort::Title:
      return orderBy(albums.size(), [&](std::size_t i) { return sortKey(albums[i].title); }, reverse);
    case AlbumSort::Year: // lo más nuevo primero
      return orderBy(albums.size(), [&](std::size_t i) { return -albums[i].year; }, reverse);
    case AlbumSort::Added:
      return orderBy(
          albums.size(),
          [&](std::size_t i) {
            std::int64_t newest = 0;
            for (std::size_t track : albums[i].tracks) {
              newest = std::max(newest, model.tracks[track].mtime);
            }
            return -newest;
          },
          reverse
      );
    case AlbumSort::Artist:
      break;
    }
    return identity(albums.size(), reverse);
  }

  std::vector<std::size_t> sortedArtists(const Model& model, NameSort sort, bool reverse) {
    if (sort == NameSort::Songs) { // los que más tienen primero
      return orderBy(
          model.artists.size(), [&](std::size_t i) { return -static_cast<long>(model.artists[i].trackCount); }, reverse
      );
    }
    return identity(model.artists.size(), reverse);
  }

  std::vector<std::size_t> sortedGenres(const Model& model, NameSort sort, bool reverse) {
    if (sort == NameSort::Songs) {
      return orderBy(
          model.genres.size(), [&](std::size_t i) { return -static_cast<long>(model.genres[i].tracks.size()); }, reverse
      );
    }
    return identity(model.genres.size(), reverse);
  }

  std::vector<std::size_t> search(const Model& model, std::string_view query) {
    std::vector<std::string> words;
    const std::string folded = fold(query);
    for (std::size_t start = 0; start < folded.size();) {
      const std::size_t end = std::min(folded.find(' ', start), folded.size());
      if (end > start) {
        words.push_back(folded.substr(start, end - start));
      }
      start = end + 1;
    }
    std::vector<std::size_t> out;
    if (words.empty()) {
      return out;
    }
    for (std::size_t i = 0; i < model.tracks.size(); ++i) {
      const Track& t = model.tracks[i];
      const std::string haystack = fold(t.title + "\n" + t.artist + "\n" + t.albumArtist + "\n" + t.album);
      if (std::ranges::all_of(words, [&](const std::string& w) { return haystack.contains(w); })) {
        out.push_back(i);
      }
    }
    return out;
  }

  // ── Lectura de archivos ───────────────────────────────────────────────────────────────────────────

  Track readTrack(const fs::path& path, const fs::path& coversDir, const SourceOrder& sources) {
    Track track;
    track.path = path.string();
    std::error_code ec;
    track.size = static_cast<std::int64_t>(fs::file_size(path, ec));
    track.mtime = mtimeOf(fs::directory_entry(path, ec), ec);

    const TagLib::FileRef file(path.c_str(), true, TagLib::AudioProperties::Fast);
    if (!file.isNull()) {
      const TagLib::PropertyMap props = file.properties();
      const auto value = [&](const char* key) {
        const auto it = props.find(key);
        return it == props.end() ? std::string() : join(it->second);
      };
      track.title = value("TITLE");
      track.artist = value("ARTIST");
      track.albumArtist = value("ALBUMARTIST");
      track.album = value("ALBUM");
      track.genre = value("GENRE");
      track.lyrics = value("LYRICS");
      track.year = leadingInt(value("DATE"));
      track.disc = leadingInt(value("DISCNUMBER"));
      track.number = leadingInt(value("TRACKNUMBER"));
      if (const auto* audio = file.audioProperties(); audio != nullptr) {
        track.duration = audio->lengthInMilliseconds() / 1000.0;
      }
    }
    if (track.title.empty()) {
      track.title = path.stem().string();
    }

    fs::create_directories(coversDir, ec);
    const fs::path cover = coverPathFor(track, coversDir);
    // Ya guardada (de este archivo, de otro del álbum o descargada).
    if (fs::exists(cover, ec)) {
      track.cover = cover.string();
      return track;
    }
    const auto embedded = [&] {
      if (file.isNull()) {
        return false;
      }
      // La portada si está marcada; si no, la primera imagen.
      const auto pictures = file.complexProperties("PICTURE");
      const TagLib::VariantMap* chosen = nullptr;
      for (const auto& picture : pictures) {
        if (chosen == nullptr || picture.value("pictureType").toString() == "Front Cover") {
          chosen = &picture;
        }
      }
      if (chosen == nullptr) {
        return false;
      }
      const TagLib::ByteVector data = chosen->value("data").toByteVector();
      const fs::path raw = cover.string() + ".raw";
      {
        std::ofstream out(raw, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
      }
      const bool ok = storeCover(raw.string(), cover);
      fs::remove(raw, ec);
      return ok;
    };
    const auto folder = [&] {
      for (std::string_view name : kFolderCovers) {
        for (std::string_view ext : kImageExtensions) {
          const fs::path candidate = path.parent_path() / (std::string(name) + std::string(ext));
          if (fs::is_regular_file(candidate, ec) && storeCover(candidate.string(), cover)) {
            return true;
          }
        }
      }
      return false;
    };
    // En el orden elegido; la de internet la busca la ventana después del escaneo.
    for (Source source : sources.order) {
      if (!sources.on(source) || source == Source::Online) {
        continue;
      }
      if (source == Source::Embedded ? embedded() : folder()) {
        track.cover = cover.string();
        break;
      }
    }
    return track;
  }

  fs::path coverPathFor(const Track& track, const fs::path& coversDir) { return coversDir / (coverKey(track) + ".jpg"); }

  // ── Library ───────────────────────────────────────────────────────────────────────────────────────

  Library::Library(fs::path dbPath, fs::path coversDir) : m_dbPath(std::move(dbPath)), m_coversDir(std::move(coversDir)) {}

  Library::~Library() {
    *m_alive = false;
    cancel();
  }

  void Library::cancel() {
    m_cancel = true;
    if (m_worker.joinable()) {
      m_worker.join();
    }
    m_cancel = false;
  }

  Model Library::load() {
    Db db;
    if (!openDb(db, m_dbPath)) {
      return {};
    }
    return organize(readAll(db.handle));
  }

  void Library::scan(std::vector<fs::path> folders, SourceOrder covers, ProgressFn progress, DoneFn done) {
    cancel();
    m_scanning = true;
    m_worker = std::thread([this, folders = std::move(folders), covers, progress = std::move(progress),
                            done = std::move(done)] { runScan(folders, covers, progress, done); });
  }

  void Library::reset() {
    // Caché pura: base de etiquetas y carátulas; el próximo escaneo lo vuelve a leer todo.
    cancel();
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm"}) {
      fs::remove(m_dbPath.string() + suffix, ec);
    }
    fs::remove_all(m_coversDir, ec);
  }

  void Library::replaceRemote(const std::string& prefix, const std::vector<Track>& tracks) {
    Db db;
    if (!openDb(db, m_dbPath)) {
      return;
    }
    db.exec("BEGIN");
    {
      // Las carátulas ya descargadas se conservan (misma ruta → misma carátula).
      std::unordered_map<std::string, std::string> covers;
      Statement select(db.handle, "SELECT path, cover FROM tracks WHERE path LIKE ?1 || '%'");
      select.bind(1, prefix);
      while (sqlite3_step(select.handle) == SQLITE_ROW) {
        covers.emplace(select.text(0), select.text(1));
      }
      Statement remove(db.handle, "DELETE FROM tracks WHERE path LIKE ?1 || '%'");
      remove.bind(1, prefix);
      sqlite3_step(remove.handle);
      Statement insert(
          db.handle,
          "INSERT OR REPLACE INTO tracks (path, title, artist, album_artist, album, genre, lyrics, cover, year, disc,"
          " number, duration, mtime, size) VALUES (?1,?2,?3,?4,?5,?6,'',?7,?8,?9,?10,?11,0,0)"
      );
      for (const Track& t : tracks) {
        sqlite3_reset(insert.handle);
        insert.bind(1, t.path);
        insert.bind(2, t.title);
        insert.bind(3, t.artist);
        insert.bind(4, t.albumArtist);
        insert.bind(5, t.album);
        insert.bind(6, t.genre);
        const auto cover = covers.find(t.path);
        insert.bind(7, cover != covers.end() ? cover->second : t.cover);
        sqlite3_bind_int(insert.handle, 8, t.year);
        sqlite3_bind_int(insert.handle, 9, t.disc);
        sqlite3_bind_int(insert.handle, 10, t.number);
        sqlite3_bind_double(insert.handle, 11, t.duration);
        sqlite3_step(insert.handle);
      }
    }
    db.exec("COMMIT");
  }

  void Library::setCover(const std::vector<std::string>& paths, const std::string& cover) {
    Db db;
    if (!openDb(db, m_dbPath)) {
      return;
    }
    Statement update(db.handle, "UPDATE tracks SET cover = ?1 WHERE path = ?2");
    db.exec("BEGIN");
    for (const auto& path : paths) {
      sqlite3_reset(update.handle);
      update.bind(1, cover);
      update.bind(2, path);
      sqlite3_step(update.handle);
    }
    db.exec("COMMIT");
  }

  void Library::runScan(
      const std::vector<fs::path>& folders, const SourceOrder& covers, const ProgressFn& progress, const DoneFn& done
  ) {
    const auto finish = [&](Model model) {
      DeferredCall::callLater([this, alive = m_alive, done, model = std::move(model)]() mutable {
        if (*alive) {
          m_scanning = false;
          done(std::move(model));
        }
      });
    };
    Db db;
    if (!openDb(db, m_dbPath)) {
      finish({});
      return;
    }
    std::unordered_map<std::string, std::pair<std::int64_t, std::int64_t>> known; // ruta → (mtime, tamaño)
    for (auto& t : readAll(db.handle)) {
      // Las de los servidores no son de las carpetas: las maneja su sincronización.
      if (t.path.starts_with('/')) {
        known.emplace(std::move(t.path), std::pair{t.mtime, t.size});
      }
    }

    Statement upsert(
        db.handle,
        "INSERT INTO tracks (path, title, artist, album_artist, album, genre, lyrics, cover, year, disc, number, duration,"
        " mtime, size) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14) ON CONFLICT(path) DO UPDATE SET"
        " title=?2, artist=?3, album_artist=?4, album=?5, genre=?6, lyrics=?7, cover=?8, year=?9, disc=?10,"
        " number=?11, duration=?12, mtime=?13, size=?14"
    );
    Progress counts;
    std::unordered_set<std::string> seen;
    std::size_t pending = 0;
    db.exec("BEGIN");
    for (const fs::path& root : folders) {
      std::error_code ec;
      fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
      for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (m_cancel) {
          db.exec("COMMIT");
          return;
        }
        const auto& entry = *it;
        if (entry.path().filename().string().starts_with('.')) {
          if (entry.is_directory(ec)) {
            it.disable_recursion_pending();
          }
          continue;
        }
        if (!entry.is_regular_file(ec) || !isAudioFile(entry.path())) {
          continue;
        }
        const std::string path = entry.path().string();
        seen.insert(path);
        if (++counts.scanned % kProgressEvery == 0) {
          DeferredCall::callLater([alive = m_alive, progress, counts] {
            if (*alive) {
              progress(counts);
            }
          });
        }
        const auto size = static_cast<std::int64_t>(entry.file_size(ec));
        const std::int64_t mtime = mtimeOf(entry, ec);
        if (const auto found = known.find(path); found != known.end() && found->second == std::pair{mtime, size}) {
          continue;
        }
        const Track t = readTrack(entry.path(), m_coversDir, covers);
        sqlite3_reset(upsert.handle);
        upsert.bind(1, t.path);
        upsert.bind(2, t.title);
        upsert.bind(3, t.artist);
        upsert.bind(4, t.albumArtist);
        upsert.bind(5, t.album);
        upsert.bind(6, t.genre);
        upsert.bind(7, t.lyrics);
        upsert.bind(8, t.cover);
        sqlite3_bind_int(upsert.handle, 9, t.year);
        sqlite3_bind_int(upsert.handle, 10, t.disc);
        sqlite3_bind_int(upsert.handle, 11, t.number);
        sqlite3_bind_double(upsert.handle, 12, t.duration);
        sqlite3_bind_int64(upsert.handle, 13, t.mtime);
        sqlite3_bind_int64(upsert.handle, 14, t.size);
        if (sqlite3_step(upsert.handle) != SQLITE_DONE) {
          kLog.warn("cannot store {}: {}", path, sqlite3_errmsg(db.handle));
        }
        ++counts.updated;
        if (++pending >= kBatch) {
          db.exec("COMMIT");
          db.exec("BEGIN");
          pending = 0;
        }
      }
    }

    // Fuera de la biblioteca: lo que ya no está. Un disco sin montar (carpeta que no existe) conserva lo suyo.
    std::vector<fs::path> missingRoots;
    for (const fs::path& root : folders) {
      std::error_code ec;
      if (!fs::is_directory(root, ec)) {
        missingRoots.push_back(root);
      }
    }
    Statement remove(db.handle, "DELETE FROM tracks WHERE path = ?1");
    for (const auto& [path, stamp] : known) {
      const bool keep = seen.contains(path) || std::ranges::any_of(missingRoots, [&](const fs::path& root) {
                          return path.starts_with(root.string() + "/");
                        });
      if (!keep) {
        sqlite3_reset(remove.handle);
        remove.bind(1, path);
        sqlite3_step(remove.handle);
      }
    }
    db.exec("COMMIT");
    kLog.info("scan: {} files, {} read", counts.scanned, counts.updated);
    finish(organize(readAll(db.handle)));
  }

} // namespace okaru::music
