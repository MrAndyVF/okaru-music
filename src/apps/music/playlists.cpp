#include "apps/music/playlists.h"

#include "core/log.h"

#include <algorithm>
#include <sqlite3.h>

namespace okaru::music {

  namespace {

    namespace fs = std::filesystem;
    constexpr Logger kLog("music-playlists");

    void exec(sqlite3* db, const char* sql) {
      char* error = nullptr;
      if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        kLog.warn("sqlite: {}", error != nullptr ? error : "?");
        sqlite3_free(error);
      }
    }

    struct Stmt {
      sqlite3_stmt* handle = nullptr;
      Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &handle, nullptr); }
      ~Stmt() { sqlite3_finalize(handle); }
      Stmt(const Stmt&) = delete;
      Stmt& operator=(const Stmt&) = delete;
      void text(int index, const std::string& value) const {
        sqlite3_bind_text(handle, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
      }
    };

  } // namespace

  Playlists::Playlists(fs::path dbPath) {
    std::error_code ec;
    fs::create_directories(dbPath.parent_path(), ec);
    if (sqlite3_open(dbPath.c_str(), &m_db) != SQLITE_OK) {
      kLog.error("cannot open {}", dbPath.string());
      sqlite3_close(m_db);
      m_db = nullptr;
      return;
    }
    sqlite3_busy_timeout(m_db, 5000);
    exec(m_db, "PRAGMA journal_mode=WAL");
    exec(m_db, "CREATE TABLE IF NOT EXISTS playlists (id INTEGER PRIMARY KEY, name TEXT NOT NULL, favorites INTEGER NOT NULL DEFAULT 0)");
    exec(m_db, "CREATE TABLE IF NOT EXISTS items (playlist INTEGER NOT NULL, position INTEGER NOT NULL, path TEXT NOT NULL)");
    exec(m_db, "CREATE INDEX IF NOT EXISTS items_playlist ON items(playlist, position)");
    // Versión 1: origen por lista. Las de antes quedan mixtas ('').
    if (Stmt version(m_db, "PRAGMA user_version"); sqlite3_step(version.handle) == SQLITE_ROW && sqlite3_column_int(version.handle, 0) < 1) {
      exec(m_db, "ALTER TABLE playlists ADD COLUMN origin TEXT NOT NULL DEFAULT ''");
      exec(m_db, "PRAGMA user_version = 1");
    }
    load();
  }

  Playlists::~Playlists() { sqlite3_close(m_db); }

  void Playlists::load() {
    m_lists.clear();
    m_liked.clear();
    if (m_db == nullptr) {
      return;
    }
    {
      Stmt select(m_db, "SELECT id, name, favorites, origin FROM playlists ORDER BY favorites DESC, id");
      while (sqlite3_step(select.handle) == SQLITE_ROW) {
        const auto* name = sqlite3_column_text(select.handle, 1);
        const auto* origin = sqlite3_column_text(select.handle, 3);
        m_lists.push_back({
            .id = sqlite3_column_int64(select.handle, 0),
            .name = name != nullptr ? reinterpret_cast<const char*>(name) : "",
            .favorites = sqlite3_column_int(select.handle, 2) != 0,
            .origin = origin != nullptr ? reinterpret_cast<const char*>(origin) : "",
        });
      }
    }
    Stmt items(m_db, "SELECT path FROM items WHERE playlist = ?1 ORDER BY position");
    for (Playlist& list : m_lists) {
      sqlite3_reset(items.handle);
      sqlite3_bind_int64(items.handle, 1, list.id);
      while (sqlite3_step(items.handle) == SQLITE_ROW) {
        list.items.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(items.handle, 0)));
      }
      if (list.favorites) {
        m_liked.insert(list.items.begin(), list.items.end());
      }
    }
  }

  void Playlists::save(const Playlist& list) {
    if (m_db == nullptr) {
      return;
    }
    exec(m_db, "BEGIN");
    {
      Stmt clear(m_db, "DELETE FROM items WHERE playlist = ?1");
      sqlite3_bind_int64(clear.handle, 1, list.id);
      sqlite3_step(clear.handle);
      Stmt insert(m_db, "INSERT INTO items (playlist, position, path) VALUES (?1, ?2, ?3)");
      for (std::size_t i = 0; i < list.items.size(); ++i) {
        sqlite3_reset(insert.handle);
        sqlite3_bind_int64(insert.handle, 1, list.id);
        sqlite3_bind_int64(insert.handle, 2, static_cast<sqlite3_int64>(i));
        insert.text(3, list.items[i]);
        sqlite3_step(insert.handle);
      }
    }
    exec(m_db, "COMMIT");
  }

  const Playlist* Playlists::find(std::int64_t id) const {
    const auto it = std::ranges::find(m_lists, id, &Playlist::id);
    return it == m_lists.end() ? nullptr : &*it;
  }

  Playlist* Playlists::findMutable(std::int64_t id) {
    const auto it = std::ranges::find(m_lists, id, &Playlist::id);
    return it == m_lists.end() ? nullptr : &*it;
  }

  bool Playlists::toggleLike(const std::string& path, const std::string& origin) {
    if (m_liked.contains(path)) {
      // De cualquier «Favoritas»: también de la única mixta de antes de la versión 1.
      for (Playlist& list : m_lists) {
        if (list.favorites && std::erase(list.items, path) > 0) {
          save(list);
        }
      }
      m_liked.erase(path);
      return false;
    }
    auto it = std::ranges::find_if(m_lists, [&](const Playlist& p) { return p.favorites && p.origin == origin; });
    if (it == m_lists.end()) {
      if (m_db == nullptr) {
        return false;
      }
      // El nombre visible lo pone la interfaz en el idioma activo; este solo identifica la fila.
      Stmt insert(m_db, "INSERT INTO playlists (name, favorites, origin) VALUES ('favorites', 1, ?1)");
      insert.text(1, origin);
      sqlite3_step(insert.handle);
      it = m_lists.insert(
          m_lists.begin(),
          {.id = sqlite3_last_insert_rowid(m_db), .name = "favorites", .favorites = true, .origin = origin}
      );
    }
    it->items.insert(it->items.begin(), path); // la más reciente arriba
    m_liked.insert(path);
    save(*it);
    return true;
  }

  std::int64_t Playlists::create(const std::string& name, const std::string& origin) {
    if (m_db == nullptr) {
      return 0;
    }
    Stmt insert(m_db, "INSERT INTO playlists (name, favorites, origin) VALUES (?1, 0, ?2)");
    insert.text(1, name);
    insert.text(2, origin);
    sqlite3_step(insert.handle);
    const std::int64_t id = sqlite3_last_insert_rowid(m_db);
    m_lists.push_back({.id = id, .name = name, .origin = origin});
    return id;
  }

  void Playlists::rename(std::int64_t id, const std::string& name) {
    Playlist* list = findMutable(id);
    if (list == nullptr || list->favorites || m_db == nullptr) {
      return;
    }
    list->name = name;
    Stmt update(m_db, "UPDATE playlists SET name = ?1 WHERE id = ?2");
    update.text(1, name);
    sqlite3_bind_int64(update.handle, 2, id);
    sqlite3_step(update.handle);
  }

  void Playlists::remove(std::int64_t id) {
    if (m_db == nullptr) {
      return;
    }
    for (const char* sql : {"DELETE FROM items WHERE playlist = ?1", "DELETE FROM playlists WHERE id = ?1"}) {
      Stmt del(m_db, sql);
      sqlite3_bind_int64(del.handle, 1, id);
      sqlite3_step(del.handle);
    }
    std::erase_if(m_lists, [id](const Playlist& p) { return p.id == id; });
    m_liked.clear();
    for (const Playlist& list : m_lists) {
      if (list.favorites) {
        m_liked.insert(list.items.begin(), list.items.end());
      }
    }
  }

  void Playlists::add(std::int64_t id, const std::vector<std::string>& paths) {
    Playlist* list = findMutable(id);
    if (list == nullptr) {
      return;
    }
    for (const auto& path : paths) {
      if (std::ranges::find(list->items, path) == list->items.end()) {
        list->items.push_back(path);
        if (list->favorites) {
          m_liked.insert(path);
        }
      }
    }
    save(*list);
  }

  void Playlists::removeItem(std::int64_t id, std::size_t index) {
    Playlist* list = findMutable(id);
    if (list == nullptr || index >= list->items.size()) {
      return;
    }
    if (list->favorites) {
      m_liked.erase(list->items[index]);
    }
    list->items.erase(list->items.begin() + static_cast<std::ptrdiff_t>(index));
    save(*list);
  }

} // namespace okaru::music
