#include "apps/music/music_window.h"

#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/log.h"
#include "i18n/i18n.h"
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
        m_state(loadState(statePath())) {
    m_player.setOnChange([this](bool structural) { onPlayerChange(structural); });
    m_app.setBuild([this](float w, float h, float s) { return build(w, h, s); });
    m_app.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    m_app.setAfterBuild([this] {
      if (m_focusSearch && m_search != nullptr) {
        m_focusSearch = false;
        m_app.focus(m_search->inputArea());
      }
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
    setModel(m_library.load());
    if (!m_state.queue.empty()) {
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
    rescan();
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
    m_app.requestRebuild();
  }

  void MusicWindow::rescan() {
    m_progress = {};
    m_library.scan(
        m_state.folders,
        [this](Library::Progress progress) {
          m_progress = progress;
          m_app.requestRebuild();
        },
        [this](Model model) {
          // Sin cambios no se reconstruye (no se pierde el detalle abierto).
          const bool same = model.tracks.size() == m_model.tracks.size() && m_progress.updated == 0;
          if (same) {
            m_app.requestRebuild();
            return;
          }
          setModel(std::move(model));
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
    m_fullPlayer = open;
    m_scrollQueueToCurrent = open;
    m_app.requestRebuild();
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
        && (m_trackAdapter != nullptr ? m_trackAdapter->itemCount() : 0) != m_player.state().queue.size();
    if (structural && m_fullPlayer && (queueChanged || m_player.currentItem() != m_fullPlayerItem)) {
      m_scrollQueueToCurrent = m_player.currentItem() != m_fullPlayerItem;
      m_app.requestRebuild();
      return;
    }
    refreshNowPlaying();
  }

  void MusicWindow::refreshNowPlaying() {
    const auto& state = m_player.state();
    const std::string& item = m_player.currentItem();
    if (item != m_shownItem && (m_nowTitle != nullptr)) {
      m_shownItem = item;
      const Track* track = trackForItem(item);
      m_nowTitle->setText(
          track != nullptr ? track->title : (item.empty() ? tr("apps.music.nothing-playing") : fs::path(item).filename().string())
      );
      m_nowArtist->setText(track != nullptr ? joinDot({track->artist, track->album}) : "");
      m_nowCover->setCover(track != nullptr ? track->cover : "");
      if (m_trackList != nullptr) {
        m_trackList->notifyDataChanged();
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

    const float header = kHeaderHeight * scale;
    const float bottom = kNowPlayingHeight * scale;
    const float sidebar = kSidebarWidth * scale;
    const float bodyHeight = std::max(0.0F, height - header - bottom);

    auto root = ui::node({.width = width, .height = height});
    root->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface), .width = width, .height = height}));
    Node* headerNode = root->addChild(buildHeader(width, scale));
    m_app.setDragNode(headerNode);

    if (m_fullPlayer) {
      auto full = buildFullPlayer(width, bodyHeight, scale);
      full->setPosition(0.0F, header);
      root->addChild(std::move(full));
    } else {
      auto side = buildSidebar(bodyHeight, scale);
      side->setPosition(0.0F, header);
      root->addChild(std::move(side));
      auto content = buildContent(std::max(0.0F, width - sidebar), bodyHeight, scale);
      content->setPosition(sidebar, header);
      root->addChild(std::move(content));
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

    // Carpetas de música.
    side->addChild(
        ui::label({
            .text = tr("apps.music.folders"),
            .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    for (std::size_t i = 0; i < m_state.folders.size(); ++i) {
      const fs::path& folder = m_state.folders[i];
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale, .fillWidth = true});
      row->addChild(ui::glyph({.glyph = "folder", .glyphSize = 14.0F * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
      row->addChild(
          ui::label({
              .text = folder.filename().empty() ? folder.string() : folder.filename().string(),
              .fontSize = Style::fontSizeCaption * scale,
              .maxLines = 1,
              .ellipsize = TextEllipsize::Middle,
              .flexGrow = 1.0F,
          })
      );
      row->addChild(
          ui::button({
              .glyph = "x",
              .glyphSize = 12.0F * scale,
              .controlHeight = 24.0F * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = tr("apps.music.remove-folder", "folder", folder.string()),
              .onClick = [this, i] { removeFolder(i); },
          })
      );
      side->addChild(std::move(row));
    }
    side->addChild(
        ui::button({
            .text = tr("apps.music.add-folder"),
            .glyph = "folder-plus",
            .contentAlign = ButtonContentAlign::Start,
            .variant = ButtonVariant::Ghost,
            .onClick = [this] { addFolder(); },
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

    // Quita las marcas de tiempo de una letra LRC ("[01:23.45]") y las etiquetas ([ar:…]): queda el texto.
    std::string plainLyrics(const std::string& lyrics) {
      std::string out;
      std::size_t start = 0;
      while (start <= lyrics.size()) {
        const std::size_t end = std::min(lyrics.find('\n', start), lyrics.size());
        std::string line = lyrics.substr(start, end - start);
        bool tagOnly = false;
        while (line.starts_with('[')) {
          const std::size_t close = line.find(']');
          if (close == std::string::npos) {
            break;
          }
          const bool timestamp = close > 1 && std::isdigit(static_cast<unsigned char>(line[1])) != 0;
          line.erase(0, close + 1);
          tagOnly = !timestamp && line.empty();
        }
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        if (!tagOnly) {
          out += line;
          out += '\n';
        }
        start = end + 1;
      }
      while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
      }
      return out;
    }

  } // namespace

  std::unique_ptr<Node> MusicWindow::buildFullPlayer(float width, float height, float scale) {
    m_trackAdapter.reset();
    const Track* track = currentTrack();
    const std::string& item = m_player.currentItem();
    m_fullPlayerItem = item;
    const float pad = Style::spaceLg * 2.0F * scale;
    const float panelWidth = std::clamp(width * 0.38F, 320.0F * scale, 520.0F * scale);
    const float leftWidth = std::max(0.0F, width - panelWidth - pad * 3.0F);
    const float textBlock = 64.0F * scale;
    const float side = std::max(0.0F, std::min(leftWidth, height - pad * 2.0F - textBlock));

    auto root = ui::node({.width = width, .height = height, .clipChildren = true});
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
    cover->setPosition(pad + std::round((leftWidth - side) * 0.5F), top);
    root->addChild(std::move(cover));

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
    info->setPosition(pad, top + side + Style::spaceMd * scale);
    root->addChild(std::move(info));

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
                m_app.requestRebuild();
              },
      });
    };
    tabs->addChild(tab(PlayerTab::Queue, "playlist", "apps.music.up-next"));
    tabs->addChild(tab(PlayerTab::Lyrics, "microphone-2", "apps.music.lyrics"));
    panel->addChild(std::move(tabs));
    const float inner = panelWidth - Style::spaceMd * 2.0F * scale;
    panel->addChild(m_playerTab == PlayerTab::Queue ? buildQueuePanel(inner, scale) : buildLyricsPanel(inner, scale));
    panel->setPosition(width - panelWidth - pad, pad);
    root->addChild(std::move(panel));
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
    m_trackAdapter = std::make_unique<TrackListAdapter>(
        std::move(rows), scale,
        [this](std::size_t index) { return static_cast<std::int64_t>(index) == m_player.state().current; },
        [this](std::size_t index) { m_player.playIndex(index); }
    );
    auto list = ui::virtualListView({
        .out = &m_trackList,
        .state = &m_queueScroll,
        .contentScale = scale,
        .adapter = m_trackAdapter.get(),
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
    const Track* track = currentTrack();
    const std::string lyrics = track != nullptr ? plainLyrics(track->lyrics) : std::string();
    if (lyrics.empty()) {
      auto empty = ui::column({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = Style::spaceSm * scale,
          .width = width,
          .flexGrow = 1.0F,
      });
      empty->addChild(
          ui::glyph({.glyph = "microphone-2", .glyphSize = 40.0F * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)})
      );
      empty->addChild(
          ui::label({
              .text = tr("apps.music.no-lyrics"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxWidth = width,
              .textAlign = TextAlign::Center,
          })
      );
      return empty;
    }
    auto scroll = ui::scrollView({
        .state = &m_lyricsScroll,
        .contentScale = scale,
        .width = width,
        .flexGrow = 1.0F,
    });
    scroll->content()->addChild(
        ui::label({
            .text = lyrics,
            .fontSize = Style::fontSizeTitle * scale,
            .fontWeight = FontWeight::Medium,
            .maxWidth = width - Style::spaceMd * scale,
        })
    );
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
      setFullPlayer(true);
      return true;
    default:
      return false;
    }
  }

} // namespace okaru::music
