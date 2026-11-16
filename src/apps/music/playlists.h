#pragma once

// OKARU: listas de Valandy (las propias y «Favoritas», que nace con el primer «me gusta»). Son datos del usuario:
// viven en su propia base (~/.local/state/okaru/music-playlists.db), aparte de la caché de la biblioteca, para que
// reconstruir la biblioteca nunca las borre. Guardan rutas; una canción que ya no está simplemente no se muestra.

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

struct sqlite3;

namespace okaru::music {

  struct Playlist {
    std::int64_t id = 0;
    std::string name;
    bool favorites = false;
    std::vector<std::string> items; // rutas, en orden
  };

  class Playlists {
  public:
    explicit Playlists(std::filesystem::path dbPath);
    ~Playlists();

    Playlists(const Playlists&) = delete;
    Playlists& operator=(const Playlists&) = delete;

    // Favoritas primero (si existe) y luego las propias en orden de creación.
    [[nodiscard]] const std::vector<Playlist>& all() const noexcept { return m_lists; }
    [[nodiscard]] const Playlist* find(std::int64_t id) const;
    [[nodiscard]] bool liked(const std::string& path) const { return m_liked.contains(path); }

    // Me gusta / ya no: crea «Favoritas» si hace falta. Devuelve si quedó con me gusta.
    bool toggleLike(const std::string& path);
    std::int64_t create(const std::string& name);
    void rename(std::int64_t id, const std::string& name);
    void remove(std::int64_t id);
    // Agrega al final las que no estén ya en la lista.
    void add(std::int64_t id, const std::vector<std::string>& paths);
    void removeItem(std::int64_t id, std::size_t index);

  private:
    void load();
    void save(const Playlist& list);
    [[nodiscard]] Playlist* findMutable(std::int64_t id);

    sqlite3* m_db = nullptr;
    std::vector<Playlist> m_lists;
    std::unordered_set<std::string> m_liked;
  };

} // namespace okaru::music
