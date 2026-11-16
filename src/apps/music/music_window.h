#pragma once

// OKARU: interfaz de Valandy. Cabecera con búsqueda, barra lateral (Álbumes, Artistas, Canciones, Géneros y las
// carpetas de música), contenido con listas virtuales (miles de canciones sin trabarse) y abajo la canción que suena.
// Al tocar la canción de abajo se abre el reproductor a pantalla completa (como YouTube Music): carátula grande y un
// panel con la cola («A continuación») y la letra. Los cambios del reproductor actualizan la barra de abajo y la fila
// actual sin reconstruir la escena.

#include "apps/music/audio_player.h"
#include "apps/music/library.h"
#include "apps/music/lyrics.h"
#include "apps/music/mpris_server.h"
#include "apps/music/music_state.h"
#include "apps/music/online.h"
#include "ui/controls/scroll_view.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class AnimationManager;
class Button;
class ScrollView;
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
  class LyricsBlock;
  class TrackListAdapter;
  class AlbumGridAdapter;
  class NameListAdapter;

  enum class View : std::uint8_t { Albums, Artists, Songs, Genres, Settings };
  // Pestañas del reproductor a pantalla completa.
  enum class PlayerTab : std::uint8_t { Queue, Lyrics };

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
    [[nodiscard]] std::unique_ptr<Node> buildFullPlayer(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildQueuePanel(float width, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildLyricsPanel(float width, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildSettings(float width, float height, float scale);
    // Fuentes de letra o carátula en orden, con interruptor y flechas para subir/bajar.
    [[nodiscard]] std::unique_ptr<Node> buildSourceList(SourceOrder& sources, bool lyrics, float width, float scale);

    // Ajustes: los aplica al reproductor y los guarda.
    void applySettings();
    // Letra de la canción actual según el orden de fuentes; si toca internet, la pide y vuelve a resolver al llegar.
    void resolveLyrics();
    void updateLyricsHighlight();
    void centerLyricLine();
    [[nodiscard]] double estimatedPosition() const;
    // Carátulas que faltan, de internet (si está activo), una a una.
    void fetchMissingCovers();
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
    void setFullPlayer(bool open);
    // Entrada con rebote del reproductor completo, de la canción nueva y de la letra (tras construir la escena).
    void startAnimations();
    void hideLibraryUnderPlayer();
    // Lleva el reproductor completo a visible u oculto desde donde esté ahora (interrumpible).
    void settleFullPlayer(AnimationManager& anim, bool visible, std::function<void()> done);
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
    OnlineService m_online;
    Model m_model;
    MusicState m_state;
    AudioPlayer m_player;
    std::unique_ptr<apps::PortalFileChooser> m_chooser;
    std::unordered_map<std::string, std::size_t> m_byPath; // ruta → índice en m_model.tracks

    View m_view = View::Albums;
    Detail m_detail;
    std::string m_query;
    bool m_focusSearch = false;
    bool m_fullPlayer = false;
    PlayerTab m_playerTab = PlayerTab::Queue;
    std::string m_fullPlayerItem; // canción que muestra el reproductor completo (si cambia, se rehace)
    bool m_scrollQueueToCurrent = false;
    ScrollViewState m_queueScroll;
    // Animaciones: qué toca animar tras la próxima construcción y los nodos que se mueven.
    bool m_animateOpen = false;
    bool m_animateTrack = false;
    bool m_animateLyrics = false;
    bool m_closing = false;
    Node* m_fullRoot = nullptr;
    Node* m_libraryView = nullptr; // barra lateral y contenido: debajo del reproductor completo, oculta mientras está abierto
    Button* m_playerToggle = nullptr;
    Node* m_fullCover = nullptr;
    Node* m_fullInfo = nullptr;
    Node* m_fullPanel = nullptr;
    LyricsBlock* m_lyricsBlock = nullptr;
    bool m_centerLyric = false;
    float m_lyricT = 0.0F;     // avance de la transición entre líneas (con rebote pasa de 1)
    float m_scrollFrom = 0.0F; // desplazamiento al empezar la transición
    long m_shrinking = -1;
    double m_stalePosition = -1.0; // posición de la canción anterior, hasta que llegue la de la nueva     // etiqueta que vuelve a su tamaño en la transición en curso
    Timer m_lyricsTick;
    double m_lastPosition = -1.0;
    std::chrono::steady_clock::time_point m_positionAt;
    float m_fullY = 0.0F;
    float m_coverX = 0.0F;
    float m_infoX = 0.0F;
    float m_panelX = 0.0F;
    float m_animScale = 1.0F;
    float m_windowWidth = 0.0F;
    bool m_rail = false; // barra lateral solo con iconos
    float m_libraryScale = 1.0F;
    ScrollViewState m_lyricsScroll;
    Library::Progress m_progress;

    // Por vista (y detalle): la posición del scroll sobrevive a las reconstrucciones.
    std::unordered_map<std::uint64_t, ScrollViewState> m_scroll;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> m_sorted; // (vista, orden, invertido) → índices
    std::unique_ptr<TrackListAdapter> m_trackAdapter;
    std::unique_ptr<TrackListAdapter> m_queueAdapter; // la cola del reproductor completo
    std::unique_ptr<AlbumGridAdapter> m_albumAdapter;
    std::unique_ptr<NameListAdapter> m_nameAdapter;

    // Nodos de la escena que se actualizan sin reconstruir.
    VirtualListView* m_trackList = nullptr;
    VirtualListView* m_queueList = nullptr;
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

    // Letra de la canción actual.
    Lyrics m_lyrics;
    std::string m_lyricsItem;
    std::unordered_set<std::string> m_lyricsFetching;
    ScrollView* m_lyricsView = nullptr;
    std::vector<int> m_lyricLabelOf; // índice de etiqueta por línea de la letra (-1: estrofa vacía)
    long m_lyricIndex = -1;
    // Carátulas en camino (para no pedir dos veces) y reconstrucción diferida al llegar varias.
    std::unordered_set<std::string> m_coversFetching;
    Timer m_coverRebuild;
    Label* m_crossfadeLabel = nullptr;

    std::unique_ptr<MprisServer> m_mpris; // último: se va antes que m_player
  };

} // namespace okaru::music
