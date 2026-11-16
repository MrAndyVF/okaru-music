#pragma once

// OKARU: biblioteca de Valandy. Las etiquetas se leen una vez (TagLib) y se guardan en SQLite; al volver a escanear
// solo se leen los archivos nuevos o modificados y se borran los que ya no están. Las vistas (artistas, álbumes,
// géneros, búsqueda) se arman en memoria desde esas filas: con decenas de miles de canciones siguen siendo pocos MB.

#include "apps/music/music_state.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;

namespace okaru::music {

  struct Track {
    std::int64_t id = 0;
    std::string path;
    std::string title;
    std::string artist;
    std::string albumArtist;
    std::string album;
    std::string genre;
    std::string lyrics; // incrustada en el archivo (USLT, LYRICS…); vacía si no trae
    std::string cover;  // carátula ya reducida en la caché; vacía si no hay
    int year = 0;
    int disc = 0;
    int number = 0;
    double duration = 0.0;
    std::int64_t mtime = 0;
    std::int64_t size = 0;
  };

  struct Album {
    std::string title;
    std::string artist; // artista del álbum
    std::string cover;
    int year = 0;
    double duration = 0.0;
    std::vector<std::size_t> tracks; // índices en Model::tracks, en orden de disco y pista
  };

  struct Artist {
    std::string name;
    std::string cover;               // la del álbum más reciente
    std::vector<std::size_t> albums; // índices en Model::albums, del más reciente al más viejo
    std::size_t trackCount = 0;
  };

  struct Genre {
    std::string name;
    std::vector<std::size_t> tracks;
  };

  // Todo lo que muestran las vistas.
  struct Model {
    std::vector<Track> tracks; // por artista, álbum, disco y pista
    std::vector<Album> albums; // por artista y nombre (cada artista los lista por año)
    std::vector<Artist> artists;
    std::vector<Genre> genres;
  };

  // Artista bajo el que se agrupa una canción: el del álbum, o el de la pista, o "desconocido".
  [[nodiscard]] std::string groupingArtist(const Track& track);
  // Clave de orden sin mayúsculas ni artículo inicial ("The Beatles" → "beatles").
  [[nodiscard]] std::string sortKey(std::string_view text);
  // Arma álbumes, artistas y géneros. Un álbum con varios artistas y sin artista de álbum queda como uno solo
  // (artista vacío → "Varios artistas" en la vista), no partido en uno por artista.
  [[nodiscard]] Model organize(std::vector<Track> tracks);
  // Canciones que coinciden con todas las palabras de `query` (título, artista o álbum, sin mayúsculas).
  [[nodiscard]] std::vector<std::size_t> search(const Model& model, std::string_view query);

  [[nodiscard]] bool isAudioFile(const std::filesystem::path& path);

  // Órdenes de las vistas. El primero de cada uno es el del modelo (sin costo); `reverse` lo invierte.
  enum class SongSort : std::uint8_t { Artist, Title, Album, Duration, Added };
  enum class AlbumSort : std::uint8_t { Artist, Title, Year, Added };
  enum class NameSort : std::uint8_t { Name, Songs };
  [[nodiscard]] std::vector<std::size_t> sortedTracks(const Model& model, SongSort sort, bool reverse);
  [[nodiscard]] std::vector<std::size_t> sortedAlbums(const Model& model, AlbumSort sort, bool reverse);
  [[nodiscard]] std::vector<std::size_t> sortedArtists(const Model& model, NameSort sort, bool reverse);
  [[nodiscard]] std::vector<std::size_t> sortedGenres(const Model& model, NameSort sort, bool reverse);

  class Library {
  public:
    struct Progress {
      std::size_t scanned = 0; // archivos vistos
      std::size_t updated = 0; // leídos de nuevo
    };
    using ProgressFn = std::function<void(Progress)>;
    using DoneFn = std::function<void(Model)>;

    Library(std::filesystem::path dbPath, std::filesystem::path coversDir);
    ~Library();

    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    // Lo que ya está en la base, sin tocar el disco (al abrir, antes de escanear).
    [[nodiscard]] Model load();
    // Escanea en otro hilo. `progress` y `done` llegan al hilo principal. Un escaneo en curso se cancela.
    void scan(std::vector<std::filesystem::path> folders, SourceOrder covers, ProgressFn progress, DoneFn done);
    // Borra la base y las carátulas guardadas (el próximo escaneo lee todo de nuevo).
    void reset();
    // Carátula descargada para esas canciones (hilo principal; es una escritura corta).
    void setCover(const std::vector<std::string>& paths, const std::string& cover);
    [[nodiscard]] const std::filesystem::path& coversDir() const noexcept { return m_coversDir; }
    [[nodiscard]] bool scanning() const noexcept { return m_scanning; }

  private:
    void cancel();
    void runScan(
        const std::vector<std::filesystem::path>& folders, const SourceOrder& covers, const ProgressFn& progress,
        const DoneFn& done
    );

    std::filesystem::path m_dbPath;
    std::filesystem::path m_coversDir;
    std::thread m_worker;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_scanning{false};
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
  };

  // Lee etiquetas, duración, letra y carátula de un archivo (TagLib). La carátula se guarda en `coversDir`, una por
  // álbum, reducida a 512 px, de la primera fuente activa en el orden de `sources`: la incrustada o
  // cover/folder/front.* de su carpeta (la de internet la busca la ventana).
  [[nodiscard]] Track
  readTrack(const std::filesystem::path& path, const std::filesystem::path& coversDir, const SourceOrder& sources = {});
  // Dónde va la carátula de esa canción (una por álbum; sin álbum, una por archivo).
  [[nodiscard]] std::filesystem::path coverPathFor(const Track& track, const std::filesystem::path& coversDir);
  // Reduce `sourceImage` a 512 px (recorte cuadrado) y la guarda como JPEG en `target`.
  bool storeCover(const std::string& sourceImage, const std::filesystem::path& target);

} // namespace okaru::music
