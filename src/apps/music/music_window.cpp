#include "apps/music/music_window.h"

#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <glib.h>

namespace okaru::music {

  namespace {

    namespace fs = std::filesystem;
    using i18n::tr;
    using i18n::trp;
    constexpr Logger kLog("music");

    constexpr float kHeaderHeight = 56.0F;
    constexpr float kSidebarWidth = 210.0F;
    constexpr float kNowPlayingHeight = 88.0F;
    constexpr float kRowHeight = 52.0F;
    constexpr float kAlbumMinWidth = 168.0F;
    constexpr float kAlbumTextHeight = 48.0F;
    constexpr float kDetailCover = 148.0F;
    constexpr float kLyricDim = 0.6F; // letra sincronizada: las líneas que no suenan, atenuadas
    constexpr float kLibraryBackScale = 0.96F; // la biblioteca, un poco más atrás, mientras se abre el reproductor

    std::string joinDot(const std::vector<std::string>& parts) {
      std::string out;
      for (const auto& part : parts) {
        if (part.empty()) {
          continue;
        }
        if (!out.empty()) {
          out += " · ";
        }
        out += part;
      }
      return out;
    }

    std::string totalDuration(double seconds) {
      const auto minutes = static_cast<long>(std::lround(seconds / 60.0));
      return minutes >= 60 ? tr("apps.music.hours", "hours", minutes / 60, "minutes", minutes % 60)
                           : tr("apps.music.minutes", "minutes", minutes);
    }

    std::uint64_t scrollKey(View view, int kind, std::size_t index, bool searching) {
      return searching ? 1ULL << 62
                       : (static_cast<std::uint64_t>(view) << 40) | (static_cast<std::uint64_t>(kind) << 32) | index;
    }

    const char* repeatGlyph(Repeat repeat) { return repeat == Repeat::One ? "repeat-once" : "repeat"; }

  } // namespace

  // ── Carátula que se carga al maquetar (ahí hay renderer y contexto GL) ─────────────────────────────

  class CoverImage final : public Node {
  public:
    CoverImage() {
      m_placeholder = static_cast<Box*>(addChild(std::make_unique<Box>()));
      m_placeholder->setFill(colorSpecFromRole(ColorRole::SurfaceContainerHigh));
      m_glyph = static_cast<Glyph*>(addChild(std::make_unique<Glyph>()));
      m_glyph->setGlyph("disc");
      m_glyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_image = static_cast<Image*>(addChild(std::make_unique<Image>()));
      m_image->setFit(ImageFit::Cover);
    }

    void setCover(const std::string& path) {
      if (path != m_wanted) {
        m_wanted = path;
        markLayoutDirty();
      }
    }

    // Fondo: carátula desenfocada, sin el disco de relleno cuando no hay.
    void setBlur(int radius) {
      m_blur = radius;
      m_placeholder->setVisible(radius == 0);
    }

  protected:
    void doLayout(Renderer& renderer) override {
      const float w = width();
      const float h = height();
      m_placeholder->setSize(w, h);
      m_image->setSize(w, h);
      m_glyph->setGlyphSize(std::min(w, h) * 0.4F);
      m_glyph->layout(renderer);
      m_glyph->setPosition(std::round((w - m_glyph->width()) * 0.5F), std::round((h - m_glyph->height()) * 0.5F));
      const int size = static_cast<int>(std::ceil(std::max(w, h)));
      if (m_wanted != m_loaded || size != m_loadedSize) {
        m_loaded = m_wanted;
        m_loadedSize = size;
        // shortcut: decodifica al maquetar (carátulas ya reducidas a 512 px, ~2 ms c/u); si una cuadrícula grande
        // se traba al desplazar, pasar a AsyncTextureCache con un contexto GL compartido.
        const bool ok = !m_loaded.empty()
            && (m_blur > 0 ? m_image->setSourceFileBlurred(renderer, m_loaded, size / 4, m_blur)
                           : m_image->setSourceFile(renderer, m_loaded, size, true, true));
        if (!ok) {
          m_image->clear(renderer);
        }
      }
      m_glyph->setVisible(m_blur == 0 && !m_image->hasImage());
      Node::doLayout(renderer);
    }

  private:
    Box* m_placeholder = nullptr;
    Glyph* m_glyph = nullptr;
    Image* m_image = nullptr;
    std::string m_wanted;
    std::string m_loaded;
    int m_loadedSize = 0;
    int m_blur = 0;
  };

  // ── Filas de canciones ───────────────────────────────────────────────────────────────────────────

  namespace {

    struct TrackRowData {
      const Track* track = nullptr; // nullptr: algo de la cola que no está en la biblioteca
      std::string item;             // ruta o URL
      int number = 0;               // número a mostrar (0 = sin número)
    };

    class TrackRow final : public Node {
    public:
      TrackRow() {
        m_background = static_cast<Box*>(addChild(std::make_unique<Box>()));
        m_number = static_cast<Label*>(addChild(std::make_unique<Label>()));
        m_playing = static_cast<Glyph*>(addChild(std::make_unique<Glyph>()));
        m_playing->setGlyph("player-play");
        m_title = static_cast<Label*>(addChild(std::make_unique<Label>()));
        m_subtitle = static_cast<Label*>(addChild(std::make_unique<Label>()));
        m_duration = static_cast<Label*>(addChild(std::make_unique<Label>()));
      }
      Box* m_background = nullptr;
      Label* m_number = nullptr;
      Glyph* m_playing = nullptr;
      Label* m_title = nullptr;
      Label* m_subtitle = nullptr;
      Label* m_duration = nullptr;
    };

  } // namespace

  class TrackListAdapter final : public VirtualListAdapter {
  public:
    TrackListAdapter(
        std::vector<TrackRowData> rows, float scale, std::function<bool(std::size_t)> isCurrent,
        std::function<void(std::size_t)> activate
    )
        : m_rows(std::move(rows)), m_scale(scale), m_isCurrent(std::move(isCurrent)), m_activate(std::move(activate)) {}

    [[nodiscard]] std::size_t itemCount() const override { return m_rows.size(); }
    [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override { return m_isCurrent(index) ? 1 : 0; }
    [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return true; }
    [[nodiscard]] float measureItem(Renderer& /*renderer*/, std::size_t /*index*/, float /*width*/) override {
      return kRowHeight * m_scale;
    }
    [[nodiscard]] std::unique_ptr<Node> createItem() override { return std::make_unique<TrackRow>(); }
    void onActivate(std::size_t index) override { m_activate(index); }

    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override {
      auto& row = static_cast<TrackRow&>(item);
      const TrackRowData& data = m_rows[index];
      const float s = m_scale;
      const float height = kRowHeight * s;
      const bool current = m_isCurrent(index);
      row.setSize(width, height);
      row.m_background->setSize(width, height);
      row.m_background->setFill(
          current ? colorSpecFromRole(ColorRole::Primary, 0.14F)
                  : (hovered ? colorSpecFromRole(ColorRole::OnSurface, 0.08F) : clearColorSpec())
      );
      const ColorSpec accent = colorSpecFromRole(current ? ColorRole::Primary : ColorRole::OnSurface);
      const ColorSpec muted = colorSpecFromRole(current ? ColorRole::Primary : ColorRole::OnSurfaceVariant);

      // Número de pista (o ▶ si es la que suena).
      const float lead = 40.0F * s;
      row.m_playing->setVisible(current);
      row.m_number->setVisible(!current && data.number > 0);
      if (current) {
        row.m_playing->setGlyphSize(16.0F * s);
        row.m_playing->setColor(accent);
        row.m_playing->layout(renderer);
        row.m_playing->setPosition(
            std::round((lead - row.m_playing->width()) * 0.5F), std::round((height - row.m_playing->height()) * 0.5F)
        );
      } else if (data.number > 0) {
        row.m_number->setText(std::to_string(data.number));
        row.m_number->setFontSize(Style::fontSizeCaption * s);
        row.m_number->setColor(muted);
        row.m_number->layout(renderer);
        row.m_number->setPosition(
            std::round((lead - row.m_number->width()) * 0.5F), std::round((height - row.m_number->height()) * 0.5F)
        );
      }

      const Track* track = data.track;
      row.m_duration->setText(track != nullptr && track->duration > 0.0 ? formatTime(track->duration) : "");
      row.m_duration->setFontSize(Style::fontSizeCaption * s);
      row.m_duration->setColor(muted);
      row.m_duration->layout(renderer);
      const float right = width - Style::spaceMd * s;
      row.m_duration->setPosition(
          std::round(right - row.m_duration->width()), std::round((height - row.m_duration->height()) * 0.5F)
      );

      const float textWidth = std::max(0.0F, right - row.m_duration->width() - Style::spaceMd * s - lead);
      row.m_title->setText(track != nullptr ? track->title : fs::path(data.item).filename().string());
      row.m_title->setFontSize(Style::fontSizeBody * s);
      row.m_title->setFontWeight(FontWeight::SemiBold);
      row.m_title->setColor(accent);
      row.m_title->setMaxWidth(textWidth);
      row.m_title->setMaxLines(1);
      row.m_title->layout(renderer);
      row.m_subtitle->setText(track != nullptr ? joinDot({track->artist, track->album}) : data.item);
      row.m_subtitle->setFontSize(Style::fontSizeCaption * s);
      row.m_subtitle->setColor(muted);
      row.m_subtitle->setMaxWidth(textWidth);
      row.m_subtitle->setMaxLines(1);
      row.m_subtitle->layout(renderer);
      const float block = row.m_title->height() + 2.0F * s + row.m_subtitle->height();
      const float top = std::round((height - block) * 0.5F);
      row.m_title->setPosition(lead, top);
      row.m_subtitle->setPosition(lead, top + row.m_title->height() + 2.0F * s);
    }

  private:
    std::vector<TrackRowData> m_rows;
    float m_scale;
    std::function<bool(std::size_t)> m_isCurrent;
    std::function<void(std::size_t)> m_activate;
  };

  // ── Filas de artistas y géneros ──────────────────────────────────────────────────────────────────

  namespace {

    struct NameRowData {
      std::string name;
      std::string caption;
      std::string cover;
      bool showCover = false;
    };

    class NameRow final : public Node {
    public:
      NameRow() {
        m_background = static_cast<Box*>(addChild(std::make_unique<Box>()));
        m_cover = static_cast<CoverImage*>(addChild(std::make_unique<CoverImage>()));
        m_name = static_cast<Label*>(addChild(std::make_unique<Label>()));
        m_caption = static_cast<Label*>(addChild(std::make_unique<Label>()));
      }
      Box* m_background = nullptr;
      CoverImage* m_cover = nullptr;
      Label* m_name = nullptr;
      Label* m_caption = nullptr;
    };

  } // namespace

  class NameListAdapter final : public VirtualListAdapter {
  public:
    NameListAdapter(std::vector<NameRowData> rows, float scale, std::function<void(std::size_t)> activate)
        : m_rows(std::move(rows)), m_scale(scale), m_activate(std::move(activate)) {}

    [[nodiscard]] std::size_t itemCount() const override { return m_rows.size(); }
    [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return true; }
    [[nodiscard]] float measureItem(Renderer& /*renderer*/, std::size_t /*index*/, float /*width*/) override {
      return (kRowHeight + 8.0F) * m_scale;
    }
    [[nodiscard]] std::unique_ptr<Node> createItem() override { return std::make_unique<NameRow>(); }
    void onActivate(std::size_t index) override { m_activate(index); }

    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override {
      auto& row = static_cast<NameRow&>(item);
      const NameRowData& data = m_rows[index];
      const float s = m_scale;
      const float height = (kRowHeight + 8.0F) * s;
      row.setSize(width, height);
      row.m_background->setSize(width, height);
      row.m_background->setFill(hovered ? colorSpecFromRole(ColorRole::OnSurface, 0.08F) : clearColorSpec());
      float x = Style::spaceSm * s;
      row.m_cover->setVisible(data.showCover);
      if (data.showCover) {
        const float side = height - 12.0F * s;
        row.m_cover->setSize(side, side);
        row.m_cover->setPosition(x, 6.0F * s);
        row.m_cover->setCover(data.cover);
        x += side + Style::spaceMd * s;
      }
      const float textWidth = std::max(0.0F, width - x - Style::spaceSm * s);
      row.m_name->setText(data.name);
      row.m_name->setFontSize(Style::fontSizeBody * s);
      row.m_name->setFontWeight(FontWeight::SemiBold);
      row.m_name->setColor(colorSpecFromRole(ColorRole::OnSurface));
      row.m_name->setMaxWidth(textWidth);
      row.m_name->setMaxLines(1);
      row.m_name->layout(renderer);
      row.m_caption->setText(data.caption);
      row.m_caption->setFontSize(Style::fontSizeCaption * s);
      row.m_caption->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      row.m_caption->setMaxWidth(textWidth);
      row.m_caption->setMaxLines(1);
      row.m_caption->layout(renderer);
      const float block = row.m_name->height() + 2.0F * s + row.m_caption->height();
      const float top = std::round((height - block) * 0.5F);
      row.m_name->setPosition(x, top);
      row.m_caption->setPosition(x, top + row.m_name->height() + 2.0F * s);
    }

  private:
    std::vector<NameRowData> m_rows;
    float m_scale;
    std::function<void(std::size_t)> m_activate;
  };

  // ── Cuadrícula de álbumes ────────────────────────────────────────────────────────────────────────

  namespace {

    class AlbumTile final : public Node {
    public:
      AlbumTile() {
        m_background = static_cast<Box*>(addChild(std::make_unique<Box>()));
        m_cover = static_cast<CoverImage*>(addChild(std::make_unique<CoverImage>()));
        m_title = static_cast<Label*>(addChild(std::make_unique<Label>()));
        m_artist = static_cast<Label*>(addChild(std::make_unique<Label>()));
      }
      Box* m_background = nullptr;
      CoverImage* m_cover = nullptr;
      Label* m_title = nullptr;
      Label* m_artist = nullptr;
    };

  } // namespace

  class AlbumGridAdapter final : public VirtualGridAdapter {
  public:
    AlbumGridAdapter(
        const Model& model, std::vector<std::size_t> albums, float scale,
        std::function<std::string(const Album&)> artistName, std::function<void(std::size_t)> open
    )
        : m_model(model), m_albums(std::move(albums)), m_scale(scale), m_artistName(std::move(artistName)),
          m_open(std::move(open)) {}

    [[nodiscard]] std::size_t itemCount() const override { return m_albums.size(); }
    [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<AlbumTile>(); }
    void onActivate(std::size_t index) override { m_open(m_albums[index]); }
    [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
      return m_model.albums[m_albums[index]].title;
    }

    void bindTile(Node& tile, std::size_t index, bool /*selected*/, bool hovered) override {
      auto& cell = static_cast<AlbumTile&>(tile);
      const Album& album = m_model.albums[m_albums[index]];
      const float s = m_scale;
      const float w = tile.width();
      const float h = tile.height();
      const float pad = Style::spaceSm * s;
      const float side = std::max(0.0F, std::min(w - pad * 2.0F, h - kAlbumTextHeight * s - pad));
      cell.m_background->setSize(w, h);
      cell.m_background->setFill(hovered ? colorSpecFromRole(ColorRole::OnSurface, 0.08F) : clearColorSpec());
      cell.m_cover->setSize(side, side);
      cell.m_cover->setPosition(std::round((w - side) * 0.5F), pad);
      cell.m_cover->setCover(album.cover);
      const float textX = std::round((w - side) * 0.5F);
      cell.m_title->setText(album.title.empty() ? tr("apps.music.unknown-album") : album.title);
      cell.m_title->setFontSize(Style::fontSizeBody * s);
      cell.m_title->setFontWeight(FontWeight::SemiBold);
      cell.m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      cell.m_title->setMaxWidth(side);
      cell.m_title->setMaxLines(1);
      cell.m_title->setPosition(textX, pad + side + 6.0F * s);
      cell.m_artist->setText(joinDot({m_artistName(album), album.year > 0 ? std::to_string(album.year) : ""}));
      cell.m_artist->setFontSize(Style::fontSizeCaption * s);
      cell.m_artist->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      cell.m_artist->setMaxWidth(side);
      cell.m_artist->setMaxLines(1);
      cell.m_artist->setPosition(textX, pad + side + 26.0F * s);
    }

  private:
    const Model& m_model;
    std::vector<std::size_t> m_albums;
    float m_scale;
    std::function<std::string(const Album&)> m_artistName;
    std::function<void(std::size_t)> m_open;
  };

  // ── Ventana ──────────────────────────────────────────────────────────────────────────────────────

  MusicWindow::MusicWindow(apps::OkaruApp& app)
      : m_app(app), m_library(
                        fs::path(FileUtils::stateDir()) / "music.db",
                        fs::path(g_get_user_cache_dir()) / "okaru" / "music" / "covers"
                    ),
        m_online(app), m_state(loadState(statePath())) {
    m_player.setOnChange([this](bool structural) { onPlayerChange(structural); });
    m_app.setBuild([this](float w, float h, float s) { return build(w, h, s); });
    m_app.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    m_app.setAfterBuild([this] {
      if (m_focusSearch && m_search != nullptr) {
        m_focusSearch = false;
        m_app.focus(m_search->inputArea());
      }
      startAnimations();
    });
    m_app.setOnClose([this] { saveState(); });
    m_app.setDropCallback([this](std::vector<std::string> paths) {
      std::vector<std::string> audio;
      for (auto& path : paths) {
        if (isAudioFile(path)) {
          audio.push_back(std::move(path));
        }
      }
      if (!audio.empty()) {
        m_player.playQueue(audio, 0);
      }
    });

    m_player.setVolume(m_state.volume);
    m_player.setShuffle(m_state.shuffle);
    m_player.setRepeat(m_state.repeat);
    applySettings();
    setModel(m_library.load());
    if (!m_state.queue.empty() && m_state.settings.resume) {
      m_player.playQueue(m_state.queue, m_state.current, true); // queda en pausa donde estaba
    }
    if (auto* bus = m_app.sessionBus(); bus != nullptr) {
      try {
        m_mpris = std::make_unique<MprisServer>(
            *bus, m_player,
            MprisServer::Hooks{
                .current = [this] { return currentTrack(); },
                .raise = [] {},
                .quit = [this] { m_app.quit(); },
            }
        );
      } catch (const std::exception& e) {
        kLog.warn("MPRIS unavailable: {}", e.what());
      }
    }
    // Biblioteca vacía: siempre se lee; si no, solo si está activado buscar cambios al abrir.
    if (m_model.tracks.empty() || m_state.settings.scanOnStart) {
      rescan();
    } else {
      fetchMissingCovers();
    }
  }

  MusicWindow::~MusicWindow() = default;

  // ── Biblioteca ────────────────────────────────────────────────────────────────────────────────────

  void MusicWindow::setModel(Model model) {
    m_model = std::move(model);
    m_byPath.clear();
    m_byPath.reserve(m_model.tracks.size());
    for (std::size_t i = 0; i < m_model.tracks.size(); ++i) {
      m_byPath.emplace(m_model.tracks[i].path, i);
    }
    // Los índices de un detalle abierto y los órdenes calculados ya no valen.
    m_detail = {};
    m_sorted.clear();
    m_shownItem.clear();
    m_lyricsItem.clear(); // las canciones se rehicieron: la letra se vuelve a resolver
    m_app.requestRebuild();
  }

  void MusicWindow::rescan() {
    m_progress = {};
    m_library.scan(
        m_state.folders, m_state.settings.covers,
        [this](Library::Progress progress) {
          m_progress = progress;
          m_app.requestRebuild();
        },
        [this](Model model) {
          // Sin cambios no se reconstruye (no se pierde el detalle abierto).
          const bool same = model.tracks.size() == m_model.tracks.size() && m_progress.updated == 0;
          if (!same) {
            setModel(std::move(model));
          }
          m_app.requestRebuild();
          fetchMissingCovers();
        }
    );
    m_app.requestRebuild();
  }

  void MusicWindow::addFolder() {
    SessionBus* bus = m_app.sessionBus();
    if (bus == nullptr) {
      return;
    }
    if (m_chooser == nullptr) {
      m_chooser = std::make_unique<apps::PortalFileChooser>(*bus);
    }
    m_chooser->openFolder(tr("apps.music.add-folder"), [this](std::vector<std::string> paths) {
      for (const auto& path : paths) {
        if (std::ranges::find(m_state.folders, fs::path(path)) == m_state.folders.end()) {
          m_state.folders.emplace_back(path);
        }
      }
      if (!paths.empty()) {
        saveState();
        rescan();
      }
    });
  }

  void MusicWindow::removeFolder(std::size_t index) {
    if (index < m_state.folders.size()) {
      m_state.folders.erase(m_state.folders.begin() + static_cast<std::ptrdiff_t>(index));
      saveState();
      rescan();
    }
  }

  void MusicWindow::saveState() {
    const auto& state = m_player.state();
    m_state.volume = state.volume;
    m_state.shuffle = m_player.shuffled();
    m_state.repeat = m_player.repeat();
    m_state.queue = state.queue;
    m_state.current = state.current > 0 ? static_cast<std::size_t>(state.current) : 0;
    if (!music::saveState(m_state, statePath())) {
      kLog.warn("cannot save {}", statePath().string());
    }
  }

  // ── Navegación y reproducción ─────────────────────────────────────────────────────────────────────

  void MusicWindow::setFullPlayer(bool open) {
    AnimationManager* anim = m_fullRoot != nullptr ? m_fullRoot->animationManager() : nullptr;
    // Clic a mitad del cierre: vuelve desde donde está, sin esperar a que termine.
    if (open && m_closing && anim != nullptr) {
      m_closing = false;
      m_fullPlayer = true;
      settleFullPlayer(*anim, true, [this] { hideLibraryUnderPlayer(); });
      return;
    }
    if (open == m_fullPlayer) {
      return;
    }
    if (!open && anim != nullptr && !m_closing) {
      // Se va deslizándose hacia abajo desde donde esté (también a mitad de la apertura) y vuelve la biblioteca.
      // La biblioteca ya está construida debajo: se muestra y el reproductor se funde sobre ella.
      m_closing = true;
      if (m_libraryView != nullptr) {
        m_libraryView->setVisible(true);
      }
      settleFullPlayer(*anim, false, [this] {
        m_closing = false;
        m_fullPlayer = false;
        if (m_fullRoot != nullptr) {
          m_fullRoot->setVisible(false); // sin reconstruir: la escena ya es la de la biblioteca
        }
        if (m_playerToggle != nullptr) {
          m_playerToggle->setGlyph("chevron-up");
          m_playerToggle->setSelected(false);
          m_playerToggle->setTooltip(tr("apps.music.show-player"));
        }
        m_app.invalidate();
      });
      return;
    }
    m_fullPlayer = open;
    m_scrollQueueToCurrent = open;
    m_animateOpen = open;
    m_libraryScale = 1.0F;
    m_animateLyrics = open && m_playerTab == PlayerTab::Lyrics;
    m_app.requestRebuild();
  }

  void MusicWindow::settleFullPlayer(AnimationManager& anim, bool visible, std::function<void()> done) {
    // Parte del estado actual de cada nodo (puede estar a mitad de otra animación) y llega a visible u oculto.
    anim.cancelForOwner(this);
    const float s = m_animScale;
    struct From {
      float rootY, rootAlpha, panelX, coverX, coverAlpha, infoX, infoAlpha;
    };
    const From from{
        m_fullRoot->y(),
        m_fullRoot->opacity(),
        m_fullPanel != nullptr ? m_fullPanel->x() : 0.0F,
        m_fullCover != nullptr ? m_fullCover->x() : 0.0F,
        m_fullCover != nullptr ? m_fullCover->opacity() : 1.0F,
        m_fullInfo != nullptr ? m_fullInfo->x() : 0.0F,
        m_fullInfo != nullptr ? m_fullInfo->opacity() : 1.0F,
    };
    const float toY = visible ? m_fullY : m_fullY + 56.0F * s;
    const float toAlpha = visible ? 1.0F : 0.0F;
    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    // Lo que falta recorrer marca la duración: un clic temprano no espera la animación entera.
    const float remaining = std::abs(toAlpha - from.rootAlpha);
    const float duration = visible ? 160.0F + 280.0F * remaining : 120.0F + 260.0F * std::max(remaining, 0.3F);
    // La biblioteca de debajo se acerca (96 % → 100 %) mientras el reproductor se va, y se aleja si vuelve.
    const float fromLibrary = m_libraryScale;
    const float toLibrary = visible ? kLibraryBackScale : 1.0F;
    anim.animate(
        0.0F, 1.0F, duration, visible ? Easing::EaseOutBack : Easing::EaseInOutCubic,
        [this, from, toY, toAlpha, visible, lerp, fromLibrary, toLibrary](float t) {
          if (m_fullRoot == nullptr) {
            return;
          }
          const float a = std::clamp(t, 0.0F, 1.0F);
          if (m_libraryView != nullptr) {
            m_libraryScale = lerp(fromLibrary, toLibrary, a);
            m_libraryView->setScale(m_libraryScale);
          }
          m_fullRoot->setPosition(0.0F, lerp(from.rootY, toY, t));
          m_fullRoot->setOpacity(std::clamp(lerp(from.rootAlpha, toAlpha, a), 0.0F, 1.0F));
          if (visible) { // al volver, lo que quedó corrido regresa a su sitio
            if (m_fullPanel != nullptr) {
              m_fullPanel->setPosition(lerp(from.panelX, m_panelX, t), m_fullPanel->y());
            }
            if (m_fullCover != nullptr) {
              m_fullCover->setPosition(lerp(from.coverX, m_coverX, t), m_fullCover->y());
              m_fullCover->setOpacity(lerp(from.coverAlpha, 1.0F, a));
            }
            if (m_fullInfo != nullptr) {
              m_fullInfo->setPosition(lerp(from.infoX, m_infoX, t), m_fullInfo->y());
              m_fullInfo->setOpacity(lerp(from.infoAlpha, 1.0F, a));
            }
          }
          m_app.requestFrame();
        },
        std::move(done), this
    );
  }

  void MusicWindow::hideLibraryUnderPlayer() {
    // Abierto del todo: la biblioteca de debajo no se pinta ni recibe clics.
    if (m_fullPlayer && !m_closing && m_libraryView != nullptr) {
      m_libraryView->setVisible(false);
    }
  }

  void MusicWindow::startAnimations() {
    AnimationManager* anim = m_fullRoot != nullptr ? m_fullRoot->animationManager() : nullptr;
    if (anim == nullptr) {
      m_animateOpen = m_animateTrack = m_animateLyrics = false;
      return;
    }
    const float s = m_animScale;
    if (m_animateOpen || m_animateTrack) {
      const bool open = m_animateOpen;
      anim->cancelForOwner(this);
      const auto apply = [this, open, s](float t) {
        const float back = 1.0F - t; // con EaseOutBack pasa de 1 y vuelve: el rebote
        const float alpha = std::clamp(t * 1.6F, 0.0F, 1.0F);
        if (open && m_fullRoot != nullptr) {
          m_fullRoot->setPosition(0.0F, m_fullY + back * 70.0F * s);
          m_fullRoot->setOpacity(alpha);
        }
        if (m_fullPanel != nullptr && open) {
          m_fullPanel->setPosition(m_panelX + back * 90.0F * s, m_fullPanel->y());
        }
        if (m_fullCover != nullptr) {
          m_fullCover->setPosition(m_coverX + back * (open ? -40.0F : 56.0F) * s, m_fullCover->y());
          m_fullCover->setOpacity(alpha);
        }
        if (m_fullInfo != nullptr) {
          m_fullInfo->setPosition(m_infoX + back * (open ? -40.0F : 72.0F) * s, m_fullInfo->y());
          m_fullInfo->setOpacity(alpha);
        }
        m_app.requestFrame();
      };
      apply(0.0F);
      anim->animate(
          0.0F, 1.0F, open ? 380.0F : 360.0F, Easing::EaseOutBack,
          [this, apply, open](float t) {
            apply(t);
            if (open && m_libraryView != nullptr) {
              m_libraryScale = 1.0F + (kLibraryBackScale - 1.0F) * std::clamp(t, 0.0F, 1.0F);
              m_libraryView->setScale(m_libraryScale);
            }
          },
          [this, open] {
            if (open) {
              hideLibraryUnderPlayer();
            }
          },
          this
      );
    }
    if (m_animateLyrics && !m_lyricLines.empty()) {
      // Renglón por renglón, cada uno sube con rebote un poco después del anterior.
      static constexpr float kLine = 460.0F;
      static constexpr float kStagger = 45.0F;
      static constexpr std::size_t kStaggered = 24; // los de más abajo entran con el último (no se ven todavía)
      const float total = kLine + kStagger * static_cast<float>(std::min(m_lyricLines.size(), kStaggered));
      const auto apply = [this, s](float ms) {
        for (std::size_t i = 0; i < m_lyricLines.size(); ++i) {
          const float start = kStagger * static_cast<float>(std::min(i, kStaggered));
          const float local = std::clamp((ms - start) / kLine, 0.0F, 1.0F);
          const float eased = applyEasing(Easing::EaseOutBack, local);
          m_lyricLines[i]->setPosition(m_lyricLines[i]->x(), m_lyricTops[i] + (1.0F - eased) * 28.0F * s);
          m_lyricLines[i]->setOpacity(std::clamp(local * 1.8F, 0.0F, 1.0F));
        }
        m_app.requestFrame();
      };
      apply(0.0F);
      anim->animate(0.0F, total, total, Easing::Linear, apply, {}, &m_lyricLines);
    }
    m_animateOpen = m_animateTrack = m_animateLyrics = false;
  }

  void MusicWindow::show(View view) {
    m_fullPlayer = false;
    m_view = view;
    m_detail = {};
    m_query.clear();
    m_app.requestRebuild();
  }

  void MusicWindow::open(Detail detail) {
    m_detail = detail;
    m_query.clear();
    m_app.requestRebuild();
  }

  void MusicWindow::back() {
    if (!m_query.empty()) {
      m_query.clear();
    } else {
      m_detail = {};
    }
    m_app.requestRebuild();
  }

  void MusicWindow::playTracks(const std::vector<std::size_t>& tracks, std::size_t start, bool shuffle) {
    if (tracks.empty()) {
      return;
    }
    std::vector<std::string> items;
    items.reserve(tracks.size());
    for (std::size_t i : tracks) {
      items.push_back(m_model.tracks[i].path);
    }
    if (shuffle) {
      m_player.setShuffle(true);
      start = static_cast<std::size_t>(std::rand()) % items.size();
    }
    m_player.playQueue(items, start);
  }

  void MusicWindow::queueTracks(const std::vector<std::size_t>& tracks) {
    std::vector<std::string> items;
    items.reserve(tracks.size());
    for (std::size_t i : tracks) {
      items.push_back(m_model.tracks[i].path);
    }
    m_player.append(items);
  }

  const Track* MusicWindow::trackForItem(const std::string& item) const {
    const auto it = m_byPath.find(item);
    return it == m_byPath.end() ? nullptr : &m_model.tracks[it->second];
  }

  const Track* MusicWindow::currentTrack() const { return trackForItem(m_player.currentItem()); }

  std::string MusicWindow::albumArtistName(const Album& album) const {
    return album.artist.empty() ? tr("apps.music.various-artists") : album.artist;
  }

  void MusicWindow::onPlayerChange(bool structural) {
    if (m_mpris != nullptr) {
      m_mpris->update();
    }
    // El reproductor completo se rehace si cambió la cola o la canción (carátula, letra); el resto solo actualiza
    // la barra de abajo.
    const bool queueChanged = m_playerTab == PlayerTab::Queue
        && (m_queueAdapter != nullptr ? m_queueAdapter->itemCount() : 0) != m_player.state().queue.size();
    if (structural && m_fullPlayer && (queueChanged || m_player.currentItem() != m_fullPlayerItem)) {
      const bool newTrack = m_player.currentItem() != m_fullPlayerItem;
      m_scrollQueueToCurrent = newTrack;
      m_animateTrack = newTrack;
      m_animateLyrics = newTrack && m_playerTab == PlayerTab::Lyrics;
      m_app.requestRebuild();
      return;
    }
    refreshNowPlaying();
  }

  void MusicWindow::refreshNowPlaying() {
    const auto& state = m_player.state();
    const std::string& item = m_player.currentItem();
    if (item != m_lyricsItem) {
      resolveLyrics();
    }
    updateLyricsHighlight();
    if (item != m_shownItem && (m_nowTitle != nullptr)) {
      m_shownItem = item;
      const Track* track = trackForItem(item);
      m_nowTitle->setText(
          track != nullptr ? track->title : (item.empty() ? tr("apps.music.nothing-playing") : fs::path(item).filename().string())
      );
      m_nowArtist->setText(track != nullptr ? joinDot({track->artist, track->album}) : "");
      m_nowCover->setCover(track != nullptr ? track->cover : "");
      for (VirtualListView* list : {m_trackList, m_queueList}) {
        if (list != nullptr) {
          list->notifyDataChanged();
        }
      }
    }
    if (m_playButton != nullptr) {
      m_playButton->setGlyph(state.paused || state.idle ? "player-play" : "player-pause");
      m_playButton->setTooltip(state.paused ? tr("apps.music.play") : tr("apps.music.pause"));
      m_shuffleButton->setSelected(m_player.shuffled());
      m_repeatButton->setGlyph(repeatGlyph(m_player.repeat()));
      m_repeatButton->setSelected(m_player.repeat() != Repeat::Off);
    }
    if (m_timeline != nullptr && !m_timeline->dragging()) {
      m_syncingTimeline = true;
      m_timeline->setRange(0.0, std::max(state.duration, 0.001));
      m_timeline->setValue(state.position);
      m_timeline->setEnabled(state.duration > 0.0);
      m_syncingTimeline = false;
    }
    if (m_positionLabel != nullptr) {
      m_positionLabel->setText(formatTime(state.position));
      m_durationLabel->setText(formatTime(state.duration));
    }
    m_app.invalidate();
  }

  // ── Escena ───────────────────────────────────────────────────────────────────────────────────────

  std::unique_ptr<Node> MusicWindow::build(float width, float height, float scale) {
    m_trackList = nullptr;
    m_queueList = nullptr;
    m_libraryView = nullptr;
    m_playerToggle = nullptr;
    m_search = nullptr;
    m_nowCover = nullptr;
    m_nowTitle = nullptr;
    m_nowArtist = nullptr;
    m_playButton = nullptr;
    m_shuffleButton = nullptr;
    m_repeatButton = nullptr;
    m_timeline = nullptr;
    m_positionLabel = nullptr;
    m_durationLabel = nullptr;
    m_shownItem.clear();
    m_fullRoot = m_fullCover = m_fullInfo = m_fullPanel = nullptr;
    m_lyricLines.clear();
    m_lyricTops.clear();
    m_lyricsView = nullptr;
    m_lyricLabelOf.clear();
    m_crossfadeLabel = nullptr;
    m_animScale = scale;

    const float header = kHeaderHeight * scale;
    const float bottom = kNowPlayingHeight * scale;
    const float sidebar = kSidebarWidth * scale;
    const float bodyHeight = std::max(0.0F, height - header - bottom);

    auto root = ui::node({.width = width, .height = height});
    root->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface), .width = width, .height = height}));
    Node* headerNode = root->addChild(buildHeader(width, scale));
    m_app.setDragNode(headerNode);

    // La biblioteca siempre existe; con el reproductor completo queda debajo, oculta salvo mientras él se anima:
    // al cerrarlo aparece sin reconstruir nada (sin destello del fondo vacío).
    auto library = ui::node({.width = width, .height = bodyHeight});
    library->setPosition(0.0F, header);
    library->addChild(buildSidebar(bodyHeight, scale));
    auto content = buildContent(std::max(0.0F, width - sidebar), bodyHeight, scale);
    content->setPosition(sidebar, 0.0F);
    library->addChild(std::move(content));
    m_libraryView = root->addChild(std::move(library));
    if (m_fullPlayer) {
      m_libraryView->setVisible(m_animateOpen);
      auto full = buildFullPlayer(width, bodyHeight, scale);
      full->setPosition(0.0F, header);
      m_fullY = header;
      m_fullRoot = root->addChild(std::move(full));
    }
    auto now = buildNowPlaying(width, scale);
    now->setPosition(0.0F, height - bottom);
    root->addChild(std::move(now));
    refreshNowPlaying();
    return root;
  }

  std::unique_ptr<Node> MusicWindow::buildHeader(float width, float scale) {
    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .paddingH = Style::spaceMd * scale,
        .width = width,
        .height = kHeaderHeight * scale,
    });
    header->addChild(ui::glyph({.glyph = "vinyl", .glyphSize = 22.0F * scale, .color = colorSpecFromRole(ColorRole::Primary)}));
    header->addChild(
        ui::label({
            .text = tr("apps.music.title"),
            .fontSize = Style::fontSizeTitle * scale,
            .fontWeight = FontWeight::SemiBold,
            .minWidth = (kSidebarWidth - 60.0F) * scale,
        })
    );
    header->addChild(
        ui::input({
            .out = &m_search,
            .value = m_query,
            .placeholder = tr("apps.music.search"),
            .clearButtonEnabled = true,
            .width = std::min(420.0F * scale, width * 0.45F),
            .onChange =
                [this](const std::string& value) {
                  if (value == m_query) {
                    return;
                  }
                  m_query = value;
                  m_fullPlayer = false;
                  m_focusSearch = true;
                  m_app.requestRebuild();
                },
        })
    );
    header->addChild(ui::node({.flexGrow = 1.0F}));
    if (m_library.scanning()) {
      header->addChild(ui::spinner({}));
      header->addChild(
          ui::label({
              .text = trp("apps.music.scanning", static_cast<long>(m_progress.scanned)),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    } else {
      header->addChild(
          ui::button({
              .glyph = "refresh",
              .variant = ButtonVariant::Ghost,
              .tooltip = tr("apps.music.rescan"),
              .onClick = [this] { rescan(); },
          })
      );
    }
    header->addChild(
        ui::button({
            .glyph = "x",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.music.close"),
            .onClick = [this] { m_app.quit(); },
        })
    );
    return header;
  }

  std::unique_ptr<Node> MusicWindow::buildSidebar(float height, float scale) {
    auto side = ui::column({
        .align = FlexAlign::Stretch,
        .gap = 2.0F * scale,
        .padding = Style::spaceSm * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
        .width = kSidebarWidth * scale,
        .height = height,
    });
    struct Entry {
      View view;
      const char* glyph;
      const char* key;
    };
    constexpr std::array<Entry, 4> kEntries{{
        {View::Albums, "disc", "apps.music.albums"},
        {View::Artists, "users", "apps.music.artists"},
        {View::Songs, "music", "apps.music.songs"},
        {View::Genres, "category", "apps.music.genres"},
    }};
    for (const auto& entry : kEntries) {
      const bool active = m_view == entry.view && m_query.empty();
      side->addChild(
          ui::button({
              .text = tr(entry.key),
              .glyph = entry.glyph,
              .selected = active,
              .contentAlign = ButtonContentAlign::Start,
              .variant = active ? ButtonVariant::Secondary : ButtonVariant::Ghost,
              .onClick = [this, view = entry.view] { show(view); },
          })
      );
    }
    side->addChild(ui::node({.flexGrow = 1.0F}));
    const bool settings = m_view == View::Settings && m_query.empty();
    side->addChild(
        ui::button({
            .text = tr("apps.music.settings.title"),
            .glyph = "settings",
            .selected = settings,
            .contentAlign = ButtonContentAlign::Start,
            .variant = settings ? ButtonVariant::Secondary : ButtonVariant::Ghost,
            .onClick = [this] { show(View::Settings); },
        })
    );
    return side;
  }

  std::unique_ptr<Node> MusicWindow::buildEmpty(float width, float height, float scale) {
    auto empty = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceSm * scale,
        .width = width,
        .height = height,
    });
    empty->addChild(ui::glyph({.glyph = "vinyl", .glyphSize = 72.0F * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
    const bool scanning = m_library.scanning();
    empty->addChild(
        ui::label({
            .text = scanning ? tr("apps.music.empty.scanning") : tr("apps.music.empty.title"),
            .fontSize = Style::fontSizeTitle * scale,
            .fontWeight = FontWeight::SemiBold,
        })
    );
    if (!scanning) {
      empty->addChild(
          ui::label({
              .text = tr("apps.music.empty.hint"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      empty->addChild(
          ui::button({
              .text = tr("apps.music.add-folder"),
              .glyph = "folder-plus",
              .variant = ButtonVariant::Primary,
              .onClick = [this] { addFolder(); },
          })
      );
    }
    return empty;
  }

  std::unique_ptr<Node> MusicWindow::buildTracks(std::vector<std::size_t> tracks, bool numbers, float scale) {
    std::vector<TrackRowData> rows;
    rows.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      const Track& track = m_model.tracks[tracks[i]];
      rows.push_back({.track = &track, .item = track.path, .number = numbers ? track.number : static_cast<int>(i + 1)});
    }
    auto items = std::make_shared<std::vector<std::size_t>>(std::move(tracks));
    m_trackAdapter = std::make_unique<TrackListAdapter>(
        std::move(rows), scale,
        [this, items](std::size_t index) {
          return m_model.tracks[(*items)[index]].path == m_player.currentItem();
        },
        [this, items](std::size_t index) { playTracks(*items, index, false); }
    );
    return ui::virtualListView({
        .out = &m_trackList,
        .state = &m_scroll[scrollKey(m_view, static_cast<int>(m_detail.kind), m_detail.index, !m_query.empty())],
        .contentScale = scale,
        .adapter = m_trackAdapter.get(),
        .flexGrow = 1.0F,
    });
  }

  std::unique_ptr<Node> MusicWindow::buildAlbums(std::vector<std::size_t> albums, float scale) {
    m_albumAdapter = std::make_unique<AlbumGridAdapter>(
        m_model, std::move(albums), scale, [this](const Album& album) { return albumArtistName(album); },
        [this](std::size_t album) { open({.kind = Detail::Kind::Album, .index = album}); }
    );
    return ui::virtualGridView({
        .state = &m_scroll[scrollKey(m_view, static_cast<int>(m_detail.kind), m_detail.index, false)],
        .contentScale = scale,
        .minCellWidth = kAlbumMinWidth * scale,
        .cellHeight = (kAlbumMinWidth + kAlbumTextHeight) * scale,
        .columnGap = Style::spaceXs * scale,
        .rowGap = Style::spaceXs * scale,
        .adapter = m_albumAdapter.get(),
        .flexGrow = 1.0F,
    });
  }

  std::unique_ptr<Node> MusicWindow::buildDetailHeader(
      const std::string& cover, std::string title, std::string subtitle, std::vector<std::size_t> tracks, float width,
      float scale
  ) {
    auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceLg * scale, .width = width});
    header->addChild(
        ui::button({
            .glyph = "arrow-left",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.music.back"),
            .onClick = [this] { back(); },
        })
    );
    if (!cover.empty()) {
      auto image = std::make_unique<CoverImage>();
      image->setSize(kDetailCover * scale, kDetailCover * scale);
      image->setCover(cover);
      header->addChild(std::move(image));
    }
    auto info = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
    info->addChild(
        ui::label({
            .text = std::move(title),
            .fontSize = Style::fontSizeHeader * scale,
            .fontWeight = FontWeight::SemiBold,
            .maxLines = 2,
        })
    );
    info->addChild(
        ui::label({
            .text = std::move(subtitle),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
        })
    );
    auto shared = std::make_shared<std::vector<std::size_t>>(std::move(tracks));
    auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
    buttons->addChild(
        ui::button({
            .text = tr("apps.music.play"),
            .glyph = "player-play",
            .variant = ButtonVariant::Primary,
            .onClick = [this, shared] {
              m_player.setShuffle(false);
              playTracks(*shared, 0, false);
            },
        })
    );
    buttons->addChild(
        ui::button({
            .text = tr("apps.music.shuffle"),
            .glyph = "arrows-shuffle",
            .variant = ButtonVariant::Outline,
            .onClick = [this, shared] { playTracks(*shared, 0, true); },
        })
    );
    buttons->addChild(
        ui::button({
            .glyph = "playlist-add",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.music.add-to-queue"),
            .onClick = [this, shared] { queueTracks(*shared); },
        })
    );
    info->addChild(std::move(buttons));
    header->addChild(std::move(info));
    return header;
  }

  namespace {

    // Claves i18n de los órdenes de cada vista (mismo orden que SongSort/AlbumSort/NameSort).
    std::vector<const char*> sortOptions(View view) {
      switch (view) {
      case View::Albums:
        return {"apps.music.sort.artist", "apps.music.sort.title", "apps.music.sort.year", "apps.music.sort.added"};
      case View::Songs:
        return {"apps.music.sort.artist", "apps.music.sort.title", "apps.music.sort.album", "apps.music.sort.duration",
                "apps.music.sort.added"};
      case View::Artists:
      case View::Genres:
        return {"apps.music.sort.name", "apps.music.sort.songs"};
      case View::Settings:
        break;
      }
      return {};
    }

  } // namespace

  const std::vector<std::size_t>& MusicWindow::sorted() {
    const auto view = static_cast<std::size_t>(m_view);
    const std::uint8_t sort = m_state.sort[view];
    const bool reverse = m_state.reverse[view];
    const auto key = static_cast<std::uint32_t>(view << 16 | static_cast<std::size_t>(sort) << 1 | (reverse ? 1U : 0U));
    auto it = m_sorted.find(key);
    if (it == m_sorted.end()) {
      std::vector<std::size_t> order;
      switch (m_view) {
      case View::Albums:
        order = sortedAlbums(m_model, static_cast<AlbumSort>(sort), reverse);
        break;
      case View::Songs:
        order = sortedTracks(m_model, static_cast<SongSort>(sort), reverse);
        break;
      case View::Artists:
        order = sortedArtists(m_model, static_cast<NameSort>(sort), reverse);
        break;
      case View::Genres:
        order = sortedGenres(m_model, static_cast<NameSort>(sort), reverse);
        break;
      case View::Settings:
        break;
      }
      it = m_sorted.emplace(key, std::move(order)).first;
    }
    return it->second;
  }

  std::unique_ptr<Node> MusicWindow::buildSortControls(float scale) {
    const auto view = static_cast<std::size_t>(m_view);
    std::vector<std::string> options;
    for (const char* key : sortOptions(m_view)) {
      options.push_back(tr(key));
    }
    auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
    row->addChild(
        ui::select({
            .options = std::move(options),
            .selectedIndex = m_state.sort[view],
            .width = 170.0F * scale,
            .onSelectionChanged =
                [this, view](std::size_t index, std::string_view) {
                  m_state.sort[view] = static_cast<std::uint8_t>(index);
                  m_app.requestRebuild();
                },
        })
    );
    row->addChild(
        ui::button({
            .glyph = m_state.reverse[view] ? "sort-descending" : "sort-ascending",
            .selected = m_state.reverse[view],
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.music.sort.reverse"),
            .onClick =
                [this, view] {
                  m_state.reverse[view] = !m_state.reverse[view];
                  m_app.requestRebuild();
                },
        })
    );
    return row;
  }

  std::unique_ptr<Node> MusicWindow::buildContent(float width, float height, float scale) {
    m_trackAdapter.reset();
    m_albumAdapter.reset();
    m_nameAdapter.reset();
    const float pad = Style::spaceLg * scale;
    const float inner = std::max(0.0F, width - pad * 2.0F);
    auto content = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * scale,
        .padding = pad,
        .width = width,
        .height = height,
    });
    const auto title = [&](std::string text, std::string caption) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .width = inner});
      row->addChild(ui::label({.text = std::move(text), .fontSize = Style::fontSizeHeader * scale, .fontWeight = FontWeight::SemiBold}));
      row->addChild(
          ui::label({
              .text = std::move(caption),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      return row;
    };

    if (!m_query.empty()) {
      auto results = search(m_model, m_query);
      content->addChild(title(tr("apps.music.results"), trp("apps.music.song-count", static_cast<long>(results.size()))));
      if (results.empty()) {
        content->addChild(ui::label({.text = tr("apps.music.no-results"), .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
      } else {
        content->addChild(buildTracks(std::move(results), false, scale));
      }
      return content;
    }
    if (m_view == View::Settings) {
      return buildSettings(width, height, scale);
    }
    if (m_model.tracks.empty()) {
      return buildEmpty(width, height, scale);
    }

    switch (m_detail.kind) {
    case Detail::Kind::Album: {
      const Album& album = m_model.albums[m_detail.index];
      content->addChild(buildDetailHeader(
          album.cover, album.title.empty() ? tr("apps.music.unknown-album") : album.title,
          joinDot(
              {albumArtistName(album), album.year > 0 ? std::to_string(album.year) : "",
               trp("apps.music.song-count", static_cast<long>(album.tracks.size())), totalDuration(album.duration)}
          ),
          album.tracks, inner, scale
      ));
      content->addChild(buildTracks(album.tracks, true, scale));
      return content;
    }
    case Detail::Kind::Artist: {
      const Artist& artist = m_model.artists[m_detail.index];
      // Sus canciones (en un recopilatorio, solo las suyas), en el orden de los álbumes.
      std::vector<std::size_t> tracks;
      const std::string key = sortKey(artist.name);
      for (std::size_t a : artist.albums) {
        for (std::size_t t : m_model.albums[a].tracks) {
          if (sortKey(groupingArtist(m_model.tracks[t])) == key) {
            tracks.push_back(t);
          }
        }
      }
      content->addChild(buildDetailHeader(
          artist.cover, artist.name.empty() ? tr("apps.music.unknown-artist") : artist.name,
          joinDot(
              {trp("apps.music.album-count", static_cast<long>(artist.albums.size())),
               trp("apps.music.song-count", static_cast<long>(artist.trackCount))}
          ),
          std::move(tracks), inner, scale
      ));
      content->addChild(buildAlbums(artist.albums, scale));
      return content;
    }
    case Detail::Kind::Genre: {
      const Genre& genre = m_model.genres[m_detail.index];
      content->addChild(buildDetailHeader(
          "", genre.name, trp("apps.music.song-count", static_cast<long>(genre.tracks.size())), genre.tracks, inner, scale
      ));
      content->addChild(buildTracks(genre.tracks, false, scale));
      return content;
    }
    case Detail::Kind::None:
      break;
    }

    switch (m_view) {
    case View::Albums: {
      auto heading = title(tr("apps.music.albums"), trp("apps.music.album-count", static_cast<long>(m_model.albums.size())));
      heading->addChild(ui::node({.flexGrow = 1.0F}));
      heading->addChild(buildSortControls(scale));
      content->addChild(std::move(heading));
      content->addChild(buildAlbums(sorted(), scale));
      break;
    }
    case View::Artists: {
      auto heading = title(tr("apps.music.artists"), trp("apps.music.artist-count", static_cast<long>(m_model.artists.size())));
      heading->addChild(ui::node({.flexGrow = 1.0F}));
      heading->addChild(buildSortControls(scale));
      content->addChild(std::move(heading));
      const auto& order = sorted();
      std::vector<NameRowData> rows;
      rows.reserve(order.size());
      for (std::size_t index : order) {
        const Artist& artist = m_model.artists[index];
        rows.push_back({
            .name = artist.name.empty() ? tr("apps.music.unknown-artist") : artist.name,
            .caption = joinDot(
                {trp("apps.music.album-count", static_cast<long>(artist.albums.size())),
                 trp("apps.music.song-count", static_cast<long>(artist.trackCount))}
            ),
            .cover = artist.cover,
            .showCover = true,
        });
      }
      m_nameAdapter = std::make_unique<NameListAdapter>(std::move(rows), scale, [this, order](std::size_t index) {
        open({.kind = Detail::Kind::Artist, .index = order[index]});
      });
      content->addChild(ui::virtualListView({
          .state = &m_scroll[scrollKey(m_view, 0, 0, false)],
          .contentScale = scale,
          .adapter = m_nameAdapter.get(),
          .flexGrow = 1.0F,
      }));
      break;
    }
    case View::Songs: {
      auto heading = title(tr("apps.music.songs"), trp("apps.music.song-count", static_cast<long>(m_model.tracks.size())));
      heading->addChild(ui::node({.flexGrow = 1.0F}));
      heading->addChild(buildSortControls(scale));
      content->addChild(std::move(heading));
      content->addChild(buildTracks(sorted(), false, scale));
      break;
    }
    case View::Genres: {
      auto heading = title(tr("apps.music.genres"), trp("apps.music.genre-count", static_cast<long>(m_model.genres.size())));
      heading->addChild(ui::node({.flexGrow = 1.0F}));
      heading->addChild(buildSortControls(scale));
      content->addChild(std::move(heading));
      const auto& order = sorted();
      std::vector<NameRowData> rows;
      rows.reserve(order.size());
      for (std::size_t index : order) {
        const Genre& genre = m_model.genres[index];
        rows.push_back({.name = genre.name, .caption = trp("apps.music.song-count", static_cast<long>(genre.tracks.size()))});
      }
      m_nameAdapter = std::make_unique<NameListAdapter>(std::move(rows), scale, [this, order](std::size_t index) {
        open({.kind = Detail::Kind::Genre, .index = order[index]});
      });
      content->addChild(ui::virtualListView({
          .state = &m_scroll[scrollKey(m_view, 0, 0, false)],
          .contentScale = scale,
          .adapter = m_nameAdapter.get(),
          .flexGrow = 1.0F,
      }));
      break;
    }
    case View::Settings:
      break;
    }
    return content;
  }

  std::unique_ptr<Node> MusicWindow::buildNowPlaying(float width, float scale) {
    const auto& state = m_player.state();
    const float height = kNowPlayingHeight * scale;
    auto bar = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceMd * scale,
        .paddingH = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
        .width = width,
        .height = height,
    });

    // Carátula y título: un clic abre (o cierra) el reproductor a pantalla completa.
    const float side = height - Style::spaceMd * 2.0F * scale;
    const float infoWidth = std::clamp(width * 0.24F, 140.0F * scale, 300.0F * scale);
    auto song = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});
    auto cover = std::make_unique<CoverImage>();
    cover->setSize(side, side);
    m_nowCover = cover.get();
    song->addChild(std::move(cover));
    auto info = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .width = infoWidth});
    info->addChild(ui::label({.out = &m_nowTitle, .text = "", .fontSize = Style::fontSizeBody * scale, .fontWeight = FontWeight::SemiBold, .maxWidth = infoWidth, .maxLines = 1}));
    info->addChild(ui::label({.out = &m_nowArtist, .text = "", .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxWidth = infoWidth, .maxLines = 1}));
    song->addChild(std::move(info));
    auto songArea = ui::inputArea({
        .tooltip = m_fullPlayer ? tr("apps.music.hide-player") : tr("apps.music.show-player"),
        .onClick = [this](const InputArea::PointerData&) { setFullPlayer(!m_fullPlayer); },
    });
    Node* songNode = songArea->addChild(std::move(song));
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      songNode->layout(*renderer);
      songArea->setSize(songNode->width(), songNode->height());
    }
    bar->addChild(std::move(songArea));

    // Controles y barra de tiempo.
    auto center = ui::column({.align = FlexAlign::Center, .gap = 2.0F * scale, .flexGrow = 1.0F});
    auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
    buttons->addChild(
        ui::button({
            .out = &m_shuffleButton,
            .glyph = "arrows-shuffle",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.music.shuffle"),
            .onClick = [this] { m_player.setShuffle(!m_player.shuffled()); },
        })
    );
    buttons->addChild(
        ui::button({
            .glyph = "player-track-prev",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.music.previous"),
            .onClick = [this] { m_player.previous(); },
        })
    );
    buttons->addChild(
        ui::button({
            .out = &m_playButton,
            .glyph = "player-play",
            .variant = ButtonVariant::Primary,
            .onClick = [this] { m_player.togglePause(); },
        })
    );
    buttons->addChild(
        ui::button({
            .glyph = "player-track-next",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.music.next"),
            .onClick = [this] { m_player.next(); },
        })
    );
    buttons->addChild(
        ui::button({
            .out = &m_repeatButton,
            .glyph = "repeat",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.music.repeat"),
            .onClick =
                [this] {
                  const Repeat next = m_player.repeat() == Repeat::Off ? Repeat::All
                      : m_player.repeat() == Repeat::All               ? Repeat::One
                                                                       : Repeat::Off;
                  m_player.setRepeat(next);
                },
        })
    );
    center->addChild(std::move(buttons));
    const float timelineWidth = std::min(560.0F * scale, std::max(160.0F * scale, width - infoWidth - 420.0F * scale));
    center->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .width = timelineWidth},
            ui::label({.out = &m_positionLabel, .text = "0:00", .fontSize = Style::fontSizeCaption * scale, .minWidth = 40.0F * scale, .textAlign = TextAlign::End}),
            ui::slider({
                .out = &m_timeline,
                .minValue = 0.0,
                .maxValue = 1.0,
                .value = 0.0,
                .wheelAdjustEnabled = false,
                .flexGrow = 1.0F,
                .onValueChanged =
                    [this](double value) {
                      if (!m_syncingTimeline) {
                        m_player.seekTo(value);
                      }
                    },
            }),
            ui::label({.out = &m_durationLabel, .text = "0:00", .fontSize = Style::fontSizeCaption * scale, .minWidth = 40.0F * scale})
        )
    );
    bar->addChild(std::move(center));

    // Volumen.
    bar->addChild(
        ui::button({
            .glyph = state.muted || state.volume <= 0.0 ? "volume-off" : state.volume < 50.0 ? "volume-2" : "volume",
            .variant = ButtonVariant::Ghost,
            .tooltip = state.muted ? tr("apps.music.unmute") : tr("apps.music.mute"),
            .onClick =
                [this] {
                  m_player.setMuted(!m_player.state().muted);
                  m_app.requestRebuild();
                },
        })
    );
    bar->addChild(
        ui::slider({
            .minValue = 0.0,
            .maxValue = 100.0,
            .value = state.muted ? 0.0 : state.volume,
            .width = 110.0F * scale,
            .onValueChanged =
                [this](double value) {
                  m_player.setVolume(value);
                  if (m_player.state().muted) {
                    m_player.setMuted(false);
                  }
                },
        })
    );
    bar->addChild(
        ui::button({
            .out = &m_playerToggle,
            .glyph = m_fullPlayer ? "chevron-down" : "chevron-up",
            .selected = m_fullPlayer,
            .variant = ButtonVariant::Ghost,
            .tooltip = m_fullPlayer ? tr("apps.music.hide-player") : tr("apps.music.show-player"),
            .onClick = [this] { setFullPlayer(!m_fullPlayer); },
        })
    );
    return bar;
  }

  // ── Reproductor a pantalla completa ──────────────────────────────────────────────────────────────

  namespace {

  } // namespace

  std::unique_ptr<Node> MusicWindow::buildFullPlayer(float width, float height, float scale) {
    m_queueAdapter.reset();
    const Track* track = currentTrack();
    const std::string& item = m_player.currentItem();
    m_fullPlayerItem = item;
    if (item != m_lyricsItem) {
      resolveLyrics();
    }
    const float pad = Style::spaceLg * 2.0F * scale;
    const float panelWidth = std::clamp(width * 0.38F, 320.0F * scale, 520.0F * scale);
    const float leftWidth = std::max(0.0F, width - panelWidth - pad * 3.0F);
    const float textBlock = 64.0F * scale;
    const float side = std::max(0.0F, std::min(leftWidth, height - pad * 2.0F - textBlock));

    auto root = ui::node({.width = width, .height = height, .clipChildren = true});
    // Opaco: al desvanecerse deja ver la biblioteca de debajo, no un fondo vacío.
    root->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface), .width = width, .height = height}));
    // Fondo: la carátula desenfocada y tenue, como en YouTube Music.
    auto background = std::make_unique<CoverImage>();
    background->setBlur(24);
    background->setSize(width, height);
    background->setCover(track != nullptr ? track->cover : "");
    background->setOpacity(0.28F);
    root->addChild(std::move(background));

    auto cover = std::make_unique<CoverImage>();
    cover->setSize(side, side);
    cover->setCover(track != nullptr ? track->cover : "");
    const float top = std::round((height - side - textBlock) * 0.5F);
    m_coverX = pad + std::round((leftWidth - side) * 0.5F);
    cover->setPosition(m_coverX, top);
    m_fullCover = root->addChild(std::move(cover));

    auto info = ui::column({.align = FlexAlign::Center, .gap = 2.0F * scale, .width = leftWidth});
    info->addChild(
        ui::label({
            .text = track != nullptr ? track->title
                : item.empty()       ? tr("apps.music.nothing-playing")
                                     : fs::path(item).filename().string(),
            .fontSize = Style::fontSizeHeader * scale,
            .fontWeight = FontWeight::SemiBold,
            .maxWidth = leftWidth,
            .maxLines = 1,
            .textAlign = TextAlign::Center,
        })
    );
    if (track != nullptr) {
      info->addChild(
          ui::label({
              .text = joinDot({track->artist, track->album, track->year > 0 ? std::to_string(track->year) : ""}),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxWidth = leftWidth,
              .maxLines = 1,
              .textAlign = TextAlign::Center,
          })
      );
    }
    m_infoX = pad;
    info->setPosition(pad, top + side + Style::spaceMd * scale);
    m_fullInfo = root->addChild(std::move(info));

    // Panel: pestañas «A continuación» y «Letra».
    const float panelHeight = std::max(0.0F, height - pad * 2.0F);
    auto panel = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .padding = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainer, 0.85F),
        .width = panelWidth,
        .height = panelHeight,
    });
    auto tabs = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
    const auto tab = [&](PlayerTab which, const char* glyph, const char* key) {
      const bool active = m_playerTab == which;
      return ui::button({
          .text = tr(key),
          .glyph = glyph,
          .selected = active,
          .variant = active ? ButtonVariant::TabActive : ButtonVariant::Tab,
          .flexGrow = 1.0F,
          .onClick =
              [this, which] {
                m_playerTab = which;
                m_scrollQueueToCurrent = which == PlayerTab::Queue;
                m_animateLyrics = which == PlayerTab::Lyrics;
                m_app.requestRebuild();
              },
      });
    };
    tabs->addChild(tab(PlayerTab::Queue, "playlist", "apps.music.up-next"));
    tabs->addChild(tab(PlayerTab::Lyrics, "microphone-2", "apps.music.lyrics"));
    panel->addChild(std::move(tabs));
    const float inner = panelWidth - Style::spaceMd * 2.0F * scale;
    panel->addChild(m_playerTab == PlayerTab::Queue ? buildQueuePanel(inner, scale) : buildLyricsPanel(inner, scale));
    m_panelX = width - panelWidth - pad;
    panel->setPosition(m_panelX, pad);
    m_fullPanel = root->addChild(std::move(panel));
    return root;
  }

  std::unique_ptr<Node> MusicWindow::buildQueuePanel(float width, float scale) {
    const auto& queue = m_player.state().queue;
    auto column = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .width = width, .flexGrow = 1.0F});
    auto heading = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .width = width});
    heading->addChild(
        ui::label({
            .text = trp("apps.music.song-count", static_cast<long>(queue.size())),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .flexGrow = 1.0F,
        })
    );
    heading->addChild(
        ui::button({
            .text = tr("apps.music.clear-queue"),
            .glyph = "trash",
            .enabled = !queue.empty(),
            .variant = ButtonVariant::Ghost,
            .onClick = [this] { m_player.clearQueue(); },
        })
    );
    column->addChild(std::move(heading));
    if (queue.empty()) {
      column->addChild(
          ui::label({
              .text = tr("apps.music.queue-empty"),
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxWidth = width,
              .maxLines = 3,
          })
      );
      return column;
    }
    std::vector<TrackRowData> rows;
    rows.reserve(queue.size());
    for (std::size_t i = 0; i < queue.size(); ++i) {
      rows.push_back({.track = trackForItem(queue[i]), .item = queue[i], .number = static_cast<int>(i + 1)});
    }
    m_queueAdapter = std::make_unique<TrackListAdapter>(
        std::move(rows), scale,
        [this](std::size_t index) { return static_cast<std::int64_t>(index) == m_player.state().current; },
        [this](std::size_t index) { m_player.playIndex(index); }
    );
    auto list = ui::virtualListView({
        .out = &m_queueList,
        .state = &m_queueScroll,
        .contentScale = scale,
        .adapter = m_queueAdapter.get(),
        .width = width,
        .flexGrow = 1.0F,
    });
    // Al abrir o cambiar de canción, la actual queda a la vista.
    if (m_scrollQueueToCurrent && m_player.state().current >= 0) {
      m_scrollQueueToCurrent = false;
      list->scrollToIndex(static_cast<std::size_t>(m_player.state().current));
    }
    column->addChild(std::move(list));
    return column;
  }

  std::unique_ptr<Node> MusicWindow::buildLyricsPanel(float width, float scale) {
    m_lyricsView = nullptr;
    m_lyricLabelOf.clear();
    m_lyricIndex = -1;
    if (m_lyrics.empty()) {
      const Track* track = currentTrack();
      const bool searching = track != nullptr && m_lyricsFetching.contains(lyricsCachePath(*track).string());
      auto empty = ui::column({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = Style::spaceSm * scale,
          .width = width,
          .flexGrow = 1.0F,
      });
      if (searching) {
        empty->addChild(ui::spinner({}));
      } else {
        empty->addChild(ui::glyph(
            {.glyph = "microphone-2", .glyphSize = 40.0F * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
        ));
      }
      empty->addChild(
          ui::label({
              .text = searching ? tr("apps.music.searching-lyrics") : tr("apps.music.no-lyrics"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxWidth = width,
              .textAlign = TextAlign::Center,
          })
      );
      return empty;
    }
    auto scroll = ui::scrollView({
        .out = &m_lyricsView,
        .state = &m_lyricsScroll,
        .contentScale = scale,
        .width = width,
        .flexGrow = 1.0F,
    });
    // Renglones centrados, cada uno su etiqueta y colocados a mano: así la animación los mueve sin que el layout
    // los devuelva a su sitio. Las líneas vacías separan estrofas. Sincronizada: la que suena resalta.
    const float lineWidth = width - Style::spaceMd * scale;
    const float gap = Style::spaceXs * scale;
    auto block = ui::node({.width = lineWidth});
    // Margen arriba y abajo: la primera y la última línea también pueden quedar en el centro.
    float y = m_lyrics.synced ? 160.0F * scale : Style::spaceMd * scale;
    Renderer* renderer = m_app.renderer();
    for (const LyricLine& lyric : m_lyrics.lines) {
      if (lyric.text.empty()) {
        m_lyricLabelOf.push_back(-1);
        y += Style::spaceLg * scale;
        continue;
      }
      auto line = ui::label({
          .text = lyric.text,
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::SemiBold,
          .color = m_lyrics.synced ? colorSpecFromRole(ColorRole::OnSurfaceVariant, kLyricDim)
                                   : colorSpecFromRole(ColorRole::OnSurface),
          .maxWidth = lineWidth,
          .textAlign = TextAlign::Center,
      });
      if (renderer != nullptr) {
        line->layout(*renderer);
      }
      line->setPosition(std::round((lineWidth - line->width()) * 0.5F), y);
      m_lyricTops.push_back(y);
      y += line->height() + gap * (m_lyrics.synced ? 3.0F : 1.0F);
      m_lyricLabelOf.push_back(static_cast<int>(m_lyricLines.size()));
      m_lyricLines.push_back(block->addChild(std::move(line)));
    }
    block->setSize(lineWidth, y + (m_lyrics.synced ? 160.0F * scale : Style::spaceLg * scale));
    scroll->content()->addChild(std::move(block));
    return scroll;
  }

  // ── Letra ─────────────────────────────────────────────────────────────────────────────────────────

  void MusicWindow::resolveLyrics() {
    m_lyricsItem = m_player.currentItem();
    m_lyrics = {};
    const Track* track = currentTrack();
    if (track == nullptr) {
      return;
    }
    const auto& settings = m_state.settings;
    const fs::path cache = lyricsCachePath(*track);
    bool needOnline = false;
    for (Source source : settings.lyrics.order) {
      if (!settings.lyrics.on(source)) {
        continue;
      }
      Lyrics found;
      if (source == Source::Embedded) {
        found = parseLyrics(track->lyrics);
      } else if (source == Source::Local) {
        found = parseLyrics(sidecarLyrics(track->path));
      } else if (std::ifstream in(cache, std::ios::binary); in) {
        found = parseLyrics(std::string(std::istreambuf_iterator<char>(in), {}));
      } else if (!OnlineService::knownMissing(cache) && (m_lyrics.empty() || (settings.preferSynced && !m_lyrics.synced))) {
        needOnline = true;
      }
      if (found.empty()) {
        continue;
      }
      if (m_lyrics.empty() || (settings.preferSynced && found.synced && !m_lyrics.synced)) {
        m_lyrics = std::move(found);
      }
      if (m_lyrics.synced || !settings.preferSynced) {
        break;
      }
    }
    if (needOnline && !m_lyricsFetching.contains(cache.string())) {
      m_lyricsFetching.insert(cache.string());
      const std::string item = m_lyricsItem;
      m_online.fetchLyrics(*track, cache, [this, item, key = cache.string()](std::string /*text*/) {
        m_lyricsFetching.erase(key);
        // Si sigue sonando esa canción, se vuelve a elegir con la descargada ya en caché.
        if (m_player.currentItem() == item) {
          resolveLyrics();
          if (m_fullPlayer && m_playerTab == PlayerTab::Lyrics) {
            m_animateLyrics = true;
            m_app.requestRebuild();
          }
        }
      });
    }
  }

  void MusicWindow::updateLyricsHighlight() {
    if (!m_lyrics.synced || m_lyricLines.empty() || m_lyricLabelOf.size() != m_lyrics.lines.size()) {
      return;
    }
    const long line = currentLine(m_lyrics, m_player.state().position);
    if (line == m_lyricIndex) {
      return;
    }
    const auto labelAt = [&](long index) -> Label* {
      if (index < 0 || static_cast<std::size_t>(index) >= m_lyricLabelOf.size()) {
        return nullptr;
      }
      const int label = m_lyricLabelOf[static_cast<std::size_t>(index)];
      return label < 0 ? nullptr : static_cast<Label*>(m_lyricLines[static_cast<std::size_t>(label)]);
    };
    if (Label* old = labelAt(m_lyricIndex); old != nullptr) {
      old->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant, kLyricDim));
    }
    m_lyricIndex = line;
    Label* now = labelAt(line);
    if (now == nullptr) {
      m_app.invalidate();
      return;
    }
    // Solo color (sin escala: escalar el texto ya rasterizado lo deja borroso).
    now->setColor(colorSpecFromRole(ColorRole::Primary));
    // Desliza la letra para que la línea que suena quede al centro.
    if (m_lyricsView != nullptr) {
      const float target = std::max(0.0F, now->y() + now->height() * 0.5F - m_lyricsView->height() * 0.5F);
      const float from = m_lyricsScroll.offset;
      if (AnimationManager* anim = m_lyricsView->animationManager(); anim != nullptr) {
        anim->cancelForOwner(&m_lyricsScroll);
        anim->animate(
            0.0F, 1.0F, 420.0F, Easing::EaseOutCubic,
            [this, from, target](float t) {
              if (m_lyricsView != nullptr) {
                m_lyricsView->setScrollOffset(from + (target - from) * t);
                m_app.requestFrame();
              }
            },
            {}, &m_lyricsScroll
        );
      } else {
        m_lyricsView->setScrollOffset(target);
      }
    }
    m_app.invalidate();
  }

  // ── Ajustes ───────────────────────────────────────────────────────────────────────────────────────

  void MusicWindow::applySettings() {
    const auto& settings = m_state.settings;
    m_player.setCrossfade(settings.crossfade);
    m_player.setGapless(settings.gapless);
    m_player.setReplayGain(static_cast<int>(settings.replayGain));
  }

  void MusicWindow::fetchMissingCovers() {
    if (!m_state.settings.covers.on(Source::Online)) {
      return;
    }
    // Una petición por álbum sin carátula (o por canción suelta, sin álbum); lo ya buscado sin éxito no se repite.
    for (const Album& album : m_model.albums) {
      if (!album.cover.empty() || album.tracks.empty()) {
        continue;
      }
      const auto groups = album.title.empty() ? album.tracks : std::vector<std::size_t>{album.tracks.front()};
      for (std::size_t index : groups) {
        const Track& track = m_model.tracks[index];
        const fs::path target = coverPathFor(track, m_library.coversDir());
        std::error_code ec;
        if (m_coversFetching.contains(target.string()) || OnlineService::knownMissing(target) || fs::exists(target, ec)) {
          continue;
        }
        m_coversFetching.insert(target.string());
        // Las canciones que comparten esa carátula: el álbum entero, o solo esa.
        std::vector<std::string> paths;
        for (std::size_t t : album.title.empty() ? std::vector<std::size_t>{index} : album.tracks) {
          paths.push_back(m_model.tracks[t].path);
        }
        m_online.fetchCover(track, target, [this, target, paths = std::move(paths)](bool ok) {
          m_coversFetching.erase(target.string());
          if (!ok) {
            return;
          }
          m_library.setCover(paths, target.string());
          for (const auto& path : paths) {
            if (const auto it = m_byPath.find(path); it != m_byPath.end()) {
              m_model.tracks[it->second].cover = target.string();
            }
          }
          for (Album& a : m_model.albums) {
            if (a.cover.empty() && !a.tracks.empty() && m_model.tracks[a.tracks.front()].cover == target.string()) {
              a.cover = target.string();
            }
          }
          for (Artist& artist : m_model.artists) {
            for (std::size_t a : artist.albums) {
              if (artist.cover.empty() && !m_model.albums[a].cover.empty()) {
                artist.cover = m_model.albums[a].cover;
              }
            }
          }
          // Varias pueden llegar seguidas: una sola reconstrucción. Con el reproductor abierto no se toca la vista.
          m_coverRebuild.start(std::chrono::milliseconds(1500), [this] {
            if (!m_fullPlayer) {
              m_app.requestRebuild();
            }
          });
        });
      }
    }
  }

  std::unique_ptr<Node> MusicWindow::buildSourceList(SourceOrder& sources, bool lyrics, float width, float scale) {
    auto list = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .width = width});
    // Las carátulas de internet no se ordenan: siempre son el último recurso (solo para las que faltan).
    std::vector<std::size_t> positions;
    for (std::size_t i = 0; i < kSourceCount; ++i) {
      if (lyrics || sources.order[i] != Source::Online) {
        positions.push_back(i);
      }
    }
    for (std::size_t p = 0; p < positions.size(); ++p) {
      const std::size_t position = positions[p];
      const Source source = sources.order[position];
      const auto index = static_cast<std::size_t>(source);
      const char* names[] = {"embedded", "local", "online"};
      const std::string base = std::format("apps.music.settings.{}.{}", lyrics ? "lyrics" : "covers", names[index]);
      auto row = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
          .width = width,
      });
      row->addChild(
          ui::label({
              .text = std::to_string(p + 1),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .minWidth = 18.0F * scale,
          })
      );
      auto text = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F});
      text->addChild(ui::label({.text = tr(base + ".name"), .fontSize = Style::fontSizeBody * scale, .fontWeight = FontWeight::SemiBold}));
      text->addChild(
          ui::label({
              .text = tr(base + ".hint"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxWidth = width - 200.0F * scale,
          })
      );
      row->addChild(std::move(text));
      const auto move = [this, &sources, position, positions, p](int delta) {
        const auto target = static_cast<long>(p) + delta;
        if (target >= 0 && target < static_cast<long>(positions.size())) {
          std::swap(sources.order[position], sources.order[positions[static_cast<std::size_t>(target)]]);
          saveState();
          m_lyricsItem.clear();
          m_app.requestRebuild();
        }
      };
      row->addChild(ui::button({
          .glyph = "arrow-up",
          .enabled = p > 0,
          .variant = ButtonVariant::Ghost,
          .tooltip = tr("apps.music.settings.move-up"),
          .onClick = [move] { move(-1); },
      }));
      row->addChild(ui::button({
          .glyph = "arrow-down",
          .enabled = p + 1 < positions.size(),
          .variant = ButtonVariant::Ghost,
          .tooltip = tr("apps.music.settings.move-down"),
          .onClick = [move] { move(1); },
      }));
      row->addChild(ui::toggle({
          .checked = sources.enabled[index],
          .onChange =
              [this, &sources, index](bool on) {
                sources.enabled[index] = on;
                saveState();
                m_lyricsItem.clear();
                m_app.requestRebuild();
              },
      }));
      list->addChild(std::move(row));
    }
    return list;
  }

  std::unique_ptr<Node> MusicWindow::buildSettings(float width, float height, float scale) {
    auto& settings = m_state.settings;
    const float pad = Style::spaceLg * scale;
    const float inner = std::min(760.0F * scale, std::max(0.0F, width - pad * 2.0F - 16.0F * scale));
    auto page = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd * scale, .padding = pad, .width = inner + pad * 2.0F});

    const auto section = [&](const char* glyph, const char* key) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .paddingV = Style::spaceSm * scale});
      row->addChild(ui::glyph({.glyph = glyph, .glyphSize = 18.0F * scale, .color = colorSpecFromRole(ColorRole::Primary)}));
      row->addChild(ui::label({.text = tr(key), .fontSize = Style::fontSizeTitle * scale, .fontWeight = FontWeight::SemiBold}));
      return row;
    };
    // Fila de ajuste: título y explicación a la izquierda, el control a la derecha.
    const auto setting = [&](const std::string& key, std::unique_ptr<Node> control) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .width = inner});
      auto text = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F});
      text->addChild(ui::label({.text = tr(key + ".name"), .fontSize = Style::fontSizeBody * scale}));
      text->addChild(
          ui::label({
              .text = tr(key + ".hint"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxWidth = inner - 260.0F * scale,
          })
      );
      row->addChild(std::move(text));
      row->addChild(std::move(control));
      return row;
    };
    const auto toggle = [&](bool& value, std::function<void()> after = {}) {
      return ui::toggle({
          .checked = value,
          .onChange =
              [this, &value, after](bool on) {
                value = on;
                if (after) {
                  after();
                }
                saveState();
              },
      });
    };

    page->addChild(ui::label({.text = tr("apps.music.settings.title"), .fontSize = Style::fontSizeHeader * scale, .fontWeight = FontWeight::SemiBold}));

    // Biblioteca.
    page->addChild(section("folder", "apps.music.settings.section.library"));
    for (std::size_t i = 0; i < m_state.folders.size(); ++i) {
      const fs::path& folder = m_state.folders[i];
      auto row = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
          .width = inner,
      });
      row->addChild(ui::glyph({.glyph = "folder", .glyphSize = 16.0F * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
      row->addChild(ui::label({.text = folder.string(), .fontSize = Style::fontSizeBody * scale, .maxLines = 1, .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
      row->addChild(ui::button({
          .glyph = "x",
          .variant = ButtonVariant::Ghost,
          .tooltip = tr("apps.music.remove-folder", "folder", folder.string()),
          .onClick = [this, i] { removeFolder(i); },
      }));
      page->addChild(std::move(row));
    }
    auto folderButtons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    folderButtons->addChild(ui::button({.text = tr("apps.music.add-folder"), .glyph = "folder-plus", .variant = ButtonVariant::Outline, .onClick = [this] { addFolder(); }}));
    folderButtons->addChild(ui::button({
        .text = tr("apps.music.rescan"),
        .glyph = "refresh",
        .enabled = !m_library.scanning(),
        .variant = ButtonVariant::Ghost,
        .onClick = [this] { rescan(); },
    }));
    page->addChild(std::move(folderButtons));
    page->addChild(setting("apps.music.settings.scan-on-start", toggle(settings.scanOnStart)));
    page->addChild(setting(
        "apps.music.settings.rebuild",
        ui::button({
            .text = tr("apps.music.settings.rebuild.action"),
            .glyph = "database-off",
            .enabled = !m_library.scanning(),
            .variant = ButtonVariant::Destructive,
            .onClick =
                [this] {
                  m_library.reset();
                  setModel({});
                  rescan();
                },
        })
    ));

    // Reproducción.
    page->addChild(section("player-play", "apps.music.settings.section.playback"));
    auto fade = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    fade->addChild(ui::slider({
        .minValue = 0.0,
        .maxValue = 12.0,
        .step = 1.0,
        .value = static_cast<double>(settings.crossfade),
        .width = 160.0F * scale,
        .onValueChanged =
            [this](double value) {
              m_state.settings.crossfade = static_cast<int>(std::lround(value));
              m_player.setCrossfade(m_state.settings.crossfade);
              if (m_crossfadeLabel != nullptr) {
                m_crossfadeLabel->setText(
                    m_state.settings.crossfade == 0 ? tr("apps.music.settings.crossfade.off")
                                                    : tr("apps.music.settings.crossfade.seconds", "seconds", m_state.settings.crossfade)
                );
                m_app.invalidate();
              }
            },
        .onDragEnd = [this] { saveState(); },
    }));
    fade->addChild(ui::label({
        .out = &m_crossfadeLabel,
        .text = settings.crossfade == 0 ? tr("apps.music.settings.crossfade.off")
                                        : tr("apps.music.settings.crossfade.seconds", "seconds", settings.crossfade),
        .fontSize = Style::fontSizeCaption * scale,
        .minWidth = 80.0F * scale,
    }));
    page->addChild(setting("apps.music.settings.crossfade", std::move(fade)));
    page->addChild(setting("apps.music.settings.gapless", toggle(settings.gapless, [this] { applySettings(); })));
    page->addChild(setting(
        "apps.music.settings.replaygain",
        ui::select({
            .options = std::vector<std::string>{tr("apps.music.settings.replaygain.off"), tr("apps.music.settings.replaygain.track"),
                                                tr("apps.music.settings.replaygain.album")},
            .selectedIndex = static_cast<std::size_t>(settings.replayGain),
            .width = 170.0F * scale,
            .onSelectionChanged =
                [this](std::size_t index, std::string_view) {
                  m_state.settings.replayGain = static_cast<ReplayGain>(index);
                  applySettings();
                  saveState();
                },
        })
    ));
    page->addChild(setting("apps.music.settings.resume", toggle(settings.resume)));

    // Letras.
    page->addChild(section("microphone-2", "apps.music.settings.section.lyrics"));
    page->addChild(ui::label({
        .text = tr("apps.music.settings.lyrics.hint"),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxWidth = inner,
    }));
    page->addChild(buildSourceList(settings.lyrics, true, inner, scale));
    page->addChild(setting("apps.music.settings.prefer-synced", toggle(settings.preferSynced, [this] { m_lyricsItem.clear(); })));

    // Carátulas.
    page->addChild(section("photo", "apps.music.settings.section.covers"));
    page->addChild(ui::label({
        .text = tr("apps.music.settings.covers.hint"),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxWidth = inner,
    }));
    page->addChild(buildSourceList(settings.covers, false, inner, scale));
    page->addChild(setting(
        "apps.music.settings.covers.download",
        toggle(settings.covers.enabled[static_cast<std::size_t>(Source::Online)], [this] { fetchMissingCovers(); })
    ));
    page->addChild(ui::label({
        .text = tr("apps.music.settings.privacy"),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxWidth = inner,
    }));

    auto scroll = ui::scrollView({
        .state = &m_scroll[scrollKey(View::Settings, 0, 0, false)],
        .contentScale = scale,
        .width = width,
        .height = height,
    });
    scroll->content()->addChild(std::move(page));
    return scroll;
  }

  bool MusicWindow::onKey(const KeyboardEvent& event) {
    if (!event.pressed || event.preedit) {
      return false;
    }
    const bool ctrl = (event.modifiers & KeyMod::Ctrl) != 0;
    // Escribiendo en la búsqueda, las teclas sueltas son texto.
    const bool typing = m_search != nullptr && m_app.input().focusedArea() == m_search->inputArea();
    switch (event.sym) {
    case XKB_KEY_q:
    case XKB_KEY_w:
      if (ctrl) {
        m_app.quit();
        return true;
      }
      return false;
    case XKB_KEY_comma:
      if (ctrl) {
        show(View::Settings);
        return true;
      }
      return false;
    case XKB_KEY_f:
      if (ctrl && m_search != nullptr) {
        m_app.focus(m_search->inputArea());
        return true;
      }
      return false;
    case XKB_KEY_Escape:
      if (m_fullPlayer) {
        setFullPlayer(false);
        return true;
      }
      if (!m_query.empty() || m_detail.kind != Detail::Kind::None) {
        back();
        return true;
      }
      return false;
    case XKB_KEY_XF86AudioPlay:
      m_player.togglePause();
      return true;
    case XKB_KEY_XF86AudioNext:
      m_player.next();
      return true;
    case XKB_KEY_XF86AudioPrev:
      m_player.previous();
      return true;
    default:
      break;
    }
    if (typing) {
      return false;
    }
    switch (event.sym) {
    case XKB_KEY_space:
      m_player.togglePause();
      return true;
    case XKB_KEY_Left:
      m_player.seekBy(-5.0);
      return true;
    case XKB_KEY_Right:
      m_player.seekBy(5.0);
      return true;
    case XKB_KEY_n:
      m_player.next();
      return true;
    case XKB_KEY_p:
      m_player.previous();
      return true;
    case XKB_KEY_s:
      m_player.setShuffle(!m_player.shuffled());
      return true;
    case XKB_KEY_m:
      m_player.setMuted(!m_player.state().muted);
      return true;
    case XKB_KEY_v:
      setFullPlayer(!m_fullPlayer);
      return true;
    case XKB_KEY_u:
    case XKB_KEY_l:
      m_playerTab = event.sym == XKB_KEY_l ? PlayerTab::Lyrics : PlayerTab::Queue;
      m_scrollQueueToCurrent = m_playerTab == PlayerTab::Queue;
      m_animateLyrics = m_playerTab == PlayerTab::Lyrics;
      if (m_fullPlayer) {
        m_app.requestRebuild();
      } else {
        setFullPlayer(true);
      }
      return true;
    default:
      return false;
    }
  }

} // namespace okaru::music
