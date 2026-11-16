#pragma once

// OKARU: interfaz de Valandy. Cabecera con búsqueda, barra lateral (Álbumes, Artistas, Canciones, Géneros, Cola y
// las carpetas de música), contenido con listas virtuales (miles de canciones sin trabarse) y abajo la canción que
// suena. Los cambios del reproductor actualizan la barra de abajo y la fila actual sin reconstruir la escena.

#include "apps/music/audio_player.h"
#include "apps/music/library.h"
#include "apps/music/mpris_server.h"
#include "apps/music/music_state.h"
#include "ui/controls/scroll_view.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Button;
class Input;
class Label;
class Node;
class Slider;
class VirtualListView;
struct KeyboardEvent;

namespace okaru::apps {
  class OkaruApp;
  class PortalFileChooser;
} // namespace okaru::apps

namespace okaru::music {

  class CoverImage;
  class TrackListAdapter;
  class AlbumGridAdapter;
  class NameListAdapter;

  enum class View : std::uint8_t { Albums, Artists, Songs, Genres, Queue };

  class MusicWindow {
  public:
    explicit MusicWindow(apps::OkaruApp& app);
    ~MusicWindow();

  private:
    // Qué se está mirando dentro de una vista: la lista, o un álbum/artista/género abierto.
    struct Detail {
      enum class Kind : std::uint8_t { None, Album, Artist, Genre } kind = Kind::None;
      std::size_t index = 0;
    };

    [[nodiscard]] std::unique_ptr<Node> build(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildHeader(float width, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildSidebar(float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildContent(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildNowPlaying(float width, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildEmpty(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildTracks(std::vector<std::size_t> tracks, bool numbers, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildAlbums(std::vector<std::size_t> albums, float scale);
    [[nodiscard]] std::unique_ptr<Node>
    buildDetailHeader(const std::string& cover, std::string title, std::string subtitle, std::vector<std::size_t> tracks, float width, float scale);
    // Selector de orden y botón para invertirlo de la vista actual (Álbumes, Artistas, Canciones, Géneros).
    [[nodiscard]] std::unique_ptr<Node> buildSortControls(float scale);
    // Índices de la vista actual en el orden elegido (en caché hasta que cambie la biblioteca).
    [[nodiscard]] const std::vector<std::size_t>& sorted();
    [[nodiscard]] bool onKey(const KeyboardEvent& event);

    void setModel(Model model);
    void rescan();
    void addFolder();
    void removeFolder(std::size_t index);
    void saveState();
    void show(View view);
    void open(Detail detail);
    void back();
    void playTracks(const std::vector<std::size_t>& tracks, std::size_t start, bool shuffle);
    void queueTracks(const std::vector<std::size_t>& tracks);
    void onPlayerChange(bool structural);
    void refreshNowPlaying();
    [[nodiscard]] const Track* trackForItem(const std::string& item) const;
    [[nodiscard]] const Track* currentTrack() const;
    [[nodiscard]] std::string albumArtistName(const Album& album) const;

    apps::OkaruApp& m_app;
    Library m_library;
    Model m_model;
    MusicState m_state;
    AudioPlayer m_player;
    std::unique_ptr<apps::PortalFileChooser> m_chooser;
    std::unordered_map<std::string, std::size_t> m_byPath; // ruta → índice en m_model.tracks

    View m_view = View::Albums;
    Detail m_detail;
    std::string m_query;
    bool m_focusSearch = false;
    Library::Progress m_progress;

    // Por vista (y detalle): la posición del scroll sobrevive a las reconstrucciones.
    std::unordered_map<std::uint64_t, ScrollViewState> m_scroll;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> m_sorted; // (vista, orden, invertido) → índices
    std::unique_ptr<TrackListAdapter> m_trackAdapter;
    std::unique_ptr<AlbumGridAdapter> m_albumAdapter;
    std::unique_ptr<NameListAdapter> m_nameAdapter;

    // Nodos de la escena que se actualizan sin reconstruir.
    VirtualListView* m_trackList = nullptr;
    Input* m_search = nullptr;
    CoverImage* m_nowCover = nullptr;
    Label* m_nowTitle = nullptr;
    Label* m_nowArtist = nullptr;
    Button* m_playButton = nullptr;
    Button* m_shuffleButton = nullptr;
    Button* m_repeatButton = nullptr;
    Slider* m_timeline = nullptr;
    Label* m_positionLabel = nullptr;
    Label* m_durationLabel = nullptr;
    bool m_syncingTimeline = false; // Slider::setValue avisa como si lo moviera el usuario
    std::string m_shownItem;        // canción que muestra la barra de abajo

    std::unique_ptr<MprisServer> m_mpris; // último: se va antes que m_player
  };

} // namespace okaru::music
