#include "apps/music/music_window.h"

#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "render/core/image_file_loader.h"
#include "render/core/texture_manager.h"
#include "ui/controls/image.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/text/font_registry.h"
#include "ui/controls/context_menu.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <glib.h>
#include <linux/input-event-codes.h>

namespace okaru::music {

  namespace {

    namespace fs = std::filesystem;
    using i18n::tr;
    using i18n::trp;
    constexpr Logger kLog("music");

    constexpr float kHeaderHeight = 56.0F;
    constexpr float kSidebarWidth = 210.0F;
    constexpr float kRailWidth = 60.0F;          // barra lateral solo con iconos (ventanas angostas, mosaico)
    constexpr float kRailBelow = 920.0F;          // ancho de ventana bajo el que la barra lateral se angosta
    constexpr float kStackedPlayerBelow = 1000.0F; // bajo este ancho (o vertical), el reproductor completo va en columna
    constexpr float kVolumeSliderFrom = 760.0F;   // desde este ancho se ve la barra de volumen
    constexpr float kSongInfoFrom = 620.0F;       // desde este ancho, título y artista junto a la carátula de abajo
    constexpr float kNowPlayingHeight = 88.0F;
    constexpr float kMiniBelowWidth = 560.0F;  // bajo este ancho (o el alto de abajo) la ventana es la miniventana
    constexpr float kMiniBelowHeight = 360.0F;
    constexpr float kRowHeight = 52.0F;
    constexpr float kAlbumMinWidth = 168.0F;
    constexpr float kAlbumTextHeight = 48.0F;
    constexpr float kDetailCover = 148.0F;
    constexpr float kLyricDim = 0.6F;  // letra sincronizada: las líneas que no suenan, atenuadas
    constexpr float kLyricGrow = 1.35F; // y la que suena, más grande
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
        m_heart = static_cast<Glyph*>(addChild(std::make_unique<Glyph>()));
        m_duration = static_cast<Label*>(addChild(std::make_unique<Label>()));
        // Una sola zona para toda la fila: izquierdo reproduce (o da «me gusta» sobre el corazón), derecho abre el
        // menú. El resaltado del puntero lo lleva ella misma.
        m_area = static_cast<InputArea*>(addChild(std::make_unique<InputArea>()));
        m_area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
        m_area->setOnEnter([this](const InputArea::PointerData&) { setHover(true); });
        m_area->setOnLeave([this] { setHover(false); });
      }
      void setHover(bool hover) {
        m_hover = hover;
        m_background->setFill(
            m_current ? colorSpecFromRole(ColorRole::Primary, 0.14F)
                      : (hover ? colorSpecFromRole(ColorRole::OnSurface, 0.08F) : clearColorSpec())
        );
      }
      Box* m_background = nullptr;
      Label* m_number = nullptr;
      Glyph* m_playing = nullptr;
      Label* m_title = nullptr;
      Label* m_subtitle = nullptr;
      Glyph* m_heart = nullptr;
      Label* m_duration = nullptr;
      InputArea* m_area = nullptr;
      bool m_hover = false;
      bool m_current = false;
    };

  } // namespace

  struct TrackListCallbacks {
    std::function<bool(std::size_t)> isCurrent;
    std::function<void(std::size_t)> activate;
    std::function<bool(std::size_t)> isLiked;
    std::function<void(std::size_t)> toggleLike;
    std::function<void(std::size_t, float sceneX, float sceneY)> menu;
  };

  class TrackListAdapter final : public VirtualListAdapter {
  public:
    TrackListAdapter(std::vector<TrackRowData> rows, float scale, TrackListCallbacks callbacks)
        : m_rows(std::move(rows)), m_scale(scale), m_callbacks(std::move(callbacks)) {}

    [[nodiscard]] std::size_t itemCount() const override { return m_rows.size(); }
    [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override {
      return (m_callbacks.isCurrent(index) ? 1U : 0U) | (liked(index) ? 2U : 0U);
    }
    // La fila maneja sus propios clics (ver TrackRow).
    [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return false; }
    [[nodiscard]] float measureItem(Renderer& /*renderer*/, std::size_t /*index*/, float /*width*/) override {
      return kRowHeight * m_scale;
    }
    [[nodiscard]] std::unique_ptr<Node> createItem() override { return std::make_unique<TrackRow>(); }

    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool /*hovered*/) override {
      auto& row = static_cast<TrackRow&>(item);
      const TrackRowData& data = m_rows[index];
      const float s = m_scale;
      const float height = kRowHeight * s;
      const bool current = m_callbacks.isCurrent(index);
      const bool isLiked = liked(index);
      row.setSize(width, height);
      row.m_background->setSize(width, height);
      row.m_current = current;
      row.setHover(row.m_hover);
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
      const float durationWidth = std::max(row.m_duration->width(), 36.0F * s);
      row.m_duration->setPosition(
          std::round(right - row.m_duration->width()), std::round((height - row.m_duration->height()) * 0.5F)
      );

      // Corazón: lleno si tiene «me gusta»; si no, apenas marcado (siempre en el mismo sitio, para darle clic).
      const float heartSize = 16.0F * s;
      const float heartX = right - durationWidth - Style::spaceMd * s - heartSize;
      row.m_heart->setGlyph(isLiked ? "heart-filled" : "heart");
      row.m_heart->setGlyphSize(heartSize);
      row.m_heart->setColor(isLiked ? colorSpecFromRole(ColorRole::Primary) : colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.45F));
      row.m_heart->setVisible(track != nullptr);
      row.m_heart->layout(renderer);
      row.m_heart->setPosition(std::round(heartX), std::round((height - row.m_heart->height()) * 0.5F));

      const float textWidth = std::max(0.0F, heartX - Style::spaceMd * s - lead);
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

      row.m_area->setSize(width, height);
      const float heartLeft = heartX - 8.0F * s;
      const float heartRight = heartX + heartSize + 8.0F * s;
      const bool canLike = track != nullptr;
      row.m_area->setOnClick([this, index, heartLeft, heartRight, canLike](const InputArea::PointerData& pointer) {
        if (pointer.button == BTN_RIGHT) {
          if (m_callbacks.menu) {
            m_callbacks.menu(index, pointer.sceneX, pointer.sceneY);
          }
        } else if (canLike && pointer.localX >= heartLeft && pointer.localX <= heartRight) {
          m_callbacks.toggleLike(index);
        } else {
          m_callbacks.activate(index);
        }
      });
    }

  private:
    [[nodiscard]] bool liked(std::size_t index) const { return m_callbacks.isLiked && m_callbacks.isLiked(index); }

    std::vector<TrackRowData> m_rows;
    float m_scale;
    TrackListCallbacks m_callbacks;
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

  // ── Bloque de letra ──────────────────────────────────────────────────────────────────────────────

  // Renglones centrados. Cada línea tiene dos versiones dibujadas una sola vez: la normal y la resaltada (más
  // grande, otro color); resaltar es un fundido cruzado con escala entre ambas y la altura del renglón pasa de una a
  // otra. Las animaciones solo mueven, escalan y funden con medidas ya tomadas (place(), sin maquetar ni redibujar
  // texto) y piden un cuadro nuevo en cada paso: así el movimiento es continuo. Como en Kopuz, las líneas se
  // atenúan con la distancia a la que suena, y ese foco también se desplaza de forma continua.
  class LyricsBlock final : public Node {
  public:
    LyricsBlock(float width, float margin, float gap, float spacer, float fontSize, float bigFontSize, ColorSpec color, bool depth)
        : m_width(width), m_margin(margin), m_gap(gap), m_spacer(spacer), m_fontSize(fontSize),
          m_bigFontSize(bigFontSize), m_color(color), m_depth(depth) {}

    void addLine(const std::string& text) {
      Line line;
      line.base = static_cast<Label*>(addChild(makeLabel(text, m_fontSize, m_color)));
      if (m_depth) { // solo la letra sincronizada resalta
        line.big = static_cast<Label*>(addChild(makeLabel(text, m_bigFontSize, colorSpecFromRole(ColorRole::Primary))));
      }
      m_lines.push_back(line);
      m_entries.push_back(false);
    }
    void addSpacer() { m_entries.push_back(true); }

    [[nodiscard]] std::size_t lineCount() const noexcept { return m_lines.size(); }
    [[nodiscard]] float centerOf(std::size_t index) const { return m_lines[index].center; }
    [[nodiscard]] bool measured() const noexcept { return m_measured; }
    void setOffset(std::size_t index, float dy) { m_lines[index].offset = dy; }
    void setAlpha(std::size_t index, float alpha) { m_lines[index].alpha = alpha; }
    // 0 = normal, 1 = resaltada (con el rebote pasa un poco de 1 y vuelve).
    void setGrow(std::size_t index, float grow) { m_lines[index].grow = grow; }
    [[nodiscard]] float grow(std::size_t index) const { return m_lines[index].grow; }
    // Línea alrededor de la cual se atenúan las demás (puede estar entre dos: el foco se desliza). <0 = ninguna.
    void setAnchor(float anchor) { m_anchor = anchor; }

    // Aplica posición, escala y opacidad con las medidas del último maquetado.
    void place() {
      if (!m_measured) {
        return;
      }
      const float ratio = m_bigFontSize / m_fontSize;
      float y = m_margin;
      std::size_t index = 0;
      for (const bool spacer : m_entries) {
        if (spacer) {
          y += m_spacer;
          continue;
        }
        Line& line = m_lines[index];
        const float g = line.grow;
        const float shown = std::clamp(g, 0.0F, 1.0F);
        const float height = line.baseHeight + (line.bigHeight - line.baseHeight) * g;
        const float centerY = y + height * 0.5F + line.offset;
        float fade = 1.0F;
        if (m_depth && m_anchor >= 0.0F) {
          fade = 1.0F - std::min(kFadeMax, kFadeStep * std::abs(static_cast<float>(index) - m_anchor));
        }
        line.base->setScale(1.0F + (ratio - 1.0F) * g);
        line.base->setOpacity(line.alpha * fade * (1.0F - shown));
        line.base->setVisible(shown < 1.0F);
        line.base->setPosition(std::round((m_width - line.base->width()) * 0.5F), centerY - line.baseHeight * 0.5F);
        if (line.big != nullptr) {
          line.big->setScale(1.0F / ratio + (1.0F - 1.0F / ratio) * g);
          line.big->setOpacity(line.alpha * shown);
          line.big->setVisible(shown > 0.0F);
          line.big->setPosition(std::round((m_width - line.big->width()) * 0.5F), centerY - line.bigHeight * 0.5F);
        }
        line.center = y + height * 0.5F;
        y += height + m_gap;
        ++index;
      }
      // Solo si cambia de verdad: cambiar el tamaño vuelve a maquetar, y un maquetado por paso traba la animación.
      const float height = y + m_margin;
      if (std::abs(height - m_height) > 0.5F) {
        m_height = height;
        setSize(m_width, m_height);
      }
    }

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& /*constraints*/) override {
      measure(renderer);
      return {m_width, m_height};
    }
    void doLayout(Renderer& renderer) override { measure(renderer); }

  private:
    static constexpr float kFadeStep = 0.09F; // atenuación por línea de distancia
    static constexpr float kFadeMax = 0.55F;

    struct Line {
      Label* base = nullptr;
      Label* big = nullptr;
      float baseHeight = 0.0F;
      float bigHeight = 0.0F;
      float grow = 0.0F;
      float offset = 0.0F;
      float alpha = 1.0F;
      float center = 0.0F;
    };

    std::unique_ptr<Label> makeLabel(const std::string& text, float size, const ColorSpec& color) const {
      return ui::label({
          .text = text,
          .fontSize = size,
          .fontWeight = FontWeight::SemiBold,
          .color = color,
          .maxWidth = m_width,
          .textAlign = TextAlign::Center,
      });
    }

    // Las etiquetas no cambian de texto ni de ancho: se miden una vez.
    void measure(Renderer& renderer) {
      if (m_measured) {
        return;
      }
      for (Line& line : m_lines) {
        line.base->layout(renderer);
        line.baseHeight = line.base->height();
        line.bigHeight = line.baseHeight;
        if (line.big != nullptr) {
          line.big->layout(renderer);
          line.bigHeight = line.big->height();
        }
      }
      m_measured = true;
      place();
    }

    float m_width;
    float m_margin;
    float m_gap;
    float m_spacer;
    float m_fontSize;
    float m_bigFontSize;
    ColorSpec m_color;
    bool m_depth;
    bool m_measured = false;
    float m_anchor = -1.0F;
    float m_height = 0.0F;
    std::vector<bool> m_entries; // true = separación entre estrofas
    std::vector<Line> m_lines;
  };

  // ── Ventana ──────────────────────────────────────────────────────────────────────────────────────

  MusicWindow::MusicWindow(apps::OkaruApp& app)
      : m_app(app), m_library(
                        fs::path(FileUtils::stateDir()) / "music.db",
                        fs::path(g_get_user_cache_dir()) / "okaru" / "music" / "covers"
                    ),
        m_online(app), m_playlists(fs::path(FileUtils::stateDir()) / "music-playlists.db"),
        m_state(loadState(statePath())) {
    // El nombre «Valandy» va con una fuente de firma propia (Great Vibes, OFL), igual en cualquier sistema.
    m_titleFont = text::registerFontFile(paths::appDataDir("music") / "fonts" / "GreatVibes-Regular.ttf");
    m_player.setOnChange([this](bool structural) { onPlayerChange(structural); });
    m_app.setBuild([this](float w, float h, float s) { return build(w, h, s); });
    m_app.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    m_app.setAfterBuild([this] {
      if (m_dialogInput != nullptr) {
        m_app.focus(m_dialogInput->inputArea());
      } else if (m_focusSearch && m_search != nullptr) {
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

    // Servidores: un identificador propio para Jellyfin, sus secretos del llavero, y las rutas remotas como streaming.
    if (m_state.deviceId.empty()) {
      m_state.deviceId = std::format("valandy-{:08x}{:08x}", g_random_int(), g_random_int());
    }
    m_remote = std::make_unique<RemoteSources>(m_app, m_state.deviceId);
    m_remote->setServers(m_state.servers);
    m_player.setResolver([this](const std::string& path) {
      return isRemote(path) ? m_remote->streamUrl(path) : std::string();
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
      syncServers();
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
    m_app.requestFrame(); // va por reloj desde ahora: el primer cuadro, ya
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
    if (m_animateLyrics && m_lyricsBlock != nullptr && m_lyricsBlock->lineCount() > 0) {
      // Renglón por renglón, cada uno sube con rebote un poco después del anterior.
      static constexpr float kLine = 460.0F;
      static constexpr float kStagger = 45.0F;
      static constexpr std::size_t kStaggered = 24; // los de más abajo entran con el último (no se ven todavía)
      const std::size_t count = m_lyricsBlock->lineCount();
      const float total = kLine + kStagger * static_cast<float>(std::min(count, kStaggered));
      const auto apply = [this, s, count](float ms) {
        if (m_lyricsBlock == nullptr) {
          return;
        }
        for (std::size_t i = 0; i < count; ++i) {
          const float start = kStagger * static_cast<float>(std::min(i, kStaggered));
          const float local = std::clamp((ms - start) / kLine, 0.0F, 1.0F);
          const float eased = applyEasing(Easing::EaseOutBack, local);
          m_lyricsBlock->setOffset(i, (1.0F - eased) * 28.0F * s);
          m_lyricsBlock->setAlpha(i, std::clamp(local * 1.8F, 0.0F, 1.0F));
        }
        m_lyricsBlock->place();
        m_app.requestFrame();
      };
      apply(0.0F);
      anim->animate(0.0F, total, total, Easing::Linear, apply, {}, &m_lyricLabelOf);
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
    } else if (m_detail.kind != Detail::Kind::None) {
      m_detail = {};
    } else if (m_view == View::Playlist) {
      // Una lista no es un detalle: se vuelve a la vista desde la que se abrió.
      m_view = m_returnView;
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
      if (m_miniBackground != nullptr) {
        m_miniBackground->setCover(track != nullptr ? track->cover : "");
      }
      applyLavaColors(track != nullptr ? track->cover : "");
      if (m_likeButton != nullptr) {
        m_likeButton->setGlyph(m_playlists.liked(item) ? "heart-filled" : "heart");
        m_likeButton->setEnabled(track != nullptr);
      }
      if (m_addToButton != nullptr) {
        m_addToButton->setEnabled(track != nullptr);
      }
      for (VirtualListView* list : {m_trackList, m_queueList}) {
        if (list != nullptr) {
          list->notifyDataChanged();
        }
      }
    }
    if (m_playButton != nullptr) {
      m_playButton->setGlyph(state.paused || state.idle ? "player-play" : "player-pause");
      m_playButton->setTooltip(state.paused ? tr("apps.music.play") : tr("apps.music.pause"));
    }
    if (m_shuffleButton != nullptr) {
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
    m_likeButton = nullptr;
    m_addToButton = nullptr;
    m_dialogInput = nullptr;
    m_libraryView = nullptr;
    m_playerToggle = nullptr;
    m_search = nullptr;
    m_nowCover = nullptr;
    m_miniBackground = nullptr;
    m_lavaBlobs.clear();
    m_lavaCover.clear();
    m_lavaTick.stop();
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
    m_lyricsBlock = nullptr;
    m_lyricsView = nullptr;
    m_lyricLabelOf.clear();
    m_crossfadeLabel = nullptr;
    m_eqLabels.fill(nullptr);
    m_animScale = scale;

    if (m_app.fullscreen()) {
      m_windowWidth = width;
      auto full = buildFullscreen(width, height, scale);
      if (!m_menu.empty()) {
        full->addChild(buildMenu(width, height, scale));
      }
      refreshNowPlaying();
      return full;
    }
    if (width < kMiniBelowWidth * scale || height < kMiniBelowHeight * scale) {
      m_windowWidth = width;
      auto mini = buildMini(width, height, scale);
      if (!m_menu.empty()) {
        mini->addChild(buildMenu(width, height, scale));
      }
      if (m_dialog != Dialog::None) {
        mini->addChild(buildDialog(width, height, scale));
      }
      refreshNowPlaying();
      return mini;
    }

    const float header = kHeaderHeight * scale;
    const float bottom = kNowPlayingHeight * scale;
    m_windowWidth = width;
    m_rail = width < kRailBelow * scale;
    const float sidebar = (m_rail ? kRailWidth : kSidebarWidth) * scale;
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
    if (!m_menu.empty()) {
      root->addChild(buildMenu(width, height, scale));
    }
    if (m_dialog != Dialog::None) {
      root->addChild(buildDialog(width, height, scale));
    }
    refreshNowPlaying();
    return root;
  }

  std::unique_ptr<Node> MusicWindow::buildHeader(float width, float scale) {
    // «Valandy» empieza donde los íconos de la barra lateral (su margen + el del botón) y la búsqueda donde empieza
    // el contenido; con la barra angosta el nombre no cabe y solo queda su hueco.
    const float edge = (Style::spaceSm + Style::spaceMd) * scale;
    const float gap = Style::spaceSm * scale;
    const float sidebar = (m_rail ? kRailWidth : kSidebarWidth) * scale;
    const float titleWidth = std::max(0.0F, sidebar + Style::spaceLg * scale - edge - gap);
    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = gap,
        .paddingH = edge,
        .width = width,
        .height = kHeaderHeight * scale,
    });
    header->addChild(ui::row(
        {.align = FlexAlign::Center, .width = titleWidth},
        ui::label({
            .text = m_rail ? std::string() : tr("apps.music.title"),
            .fontSize = (m_titleFont.empty() ? Style::fontSizeTitle : 30.0F) * scale,
            .fontWeight = m_titleFont.empty() ? FontWeight::SemiBold : FontWeight::Normal,
            .fontFamily = m_titleFont.empty() ? std::optional<std::string>{} : std::optional<std::string>{m_titleFont},
            .maxLines = 1,
        })
    ));
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
        .width = (m_rail ? kRailWidth : kSidebarWidth) * scale,
        .height = height,
    });
    // Angosta: solo iconos, con el nombre en el tooltip.
    const auto entryButton = [&](const char* glyph, const char* key, bool active, std::function<void()> onClick) {
      return ui::button({
          .text = m_rail ? std::optional<std::string>{} : std::optional<std::string>{tr(key)},
          .glyph = glyph,
          .selected = active,
          .contentAlign = m_rail ? ButtonContentAlign::Center : ButtonContentAlign::Start,
          .variant = active ? ButtonVariant::Secondary : ButtonVariant::Ghost,
          .tooltip = m_rail ? std::optional<std::string>{tr(key)} : std::nullopt,
          .onClick = std::move(onClick),
      });
    };
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
      side->addChild(entryButton(entry.glyph, entry.key, active, [this, view = entry.view] { show(view); }));
    }
    // Listas: Favoritas primero, luego las propias, y crear una nueva.
    side->addChild(ui::node({.height = Style::spaceMd * scale}));
    if (!m_rail) {
      side->addChild(ui::label({
          .text = tr("apps.music.playlists"),
          .fontSize = Style::fontSizeCaption * scale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }));
    }
    auto lists = ui::scrollView({.contentScale = scale, .flexGrow = 1.0F});
    auto column = ui::column({.align = FlexAlign::Stretch, .gap = 2.0F * scale, .width = (m_rail ? kRailWidth : kSidebarWidth) * scale - Style::spaceSm * 2.0F * scale});
    for (const Playlist& list : m_playlists.all()) {
      const bool active = m_view == View::Playlist && m_playlistId == list.id && m_query.empty();
      const std::string name = playlistName(list);
      column->addChild(ui::button({
          .text = m_rail ? std::optional<std::string>{} : std::optional<std::string>{name},
          .glyph = list.favorites ? "heart" : "playlist",
          .selected = active,
          .contentAlign = m_rail ? ButtonContentAlign::Center : ButtonContentAlign::Start,
          .variant = active ? ButtonVariant::Secondary : ButtonVariant::Ghost,
          .tooltip = name,
          .onClick = [this, id = list.id] { openPlaylist(id); },
      }));
    }
    column->addChild(entryButton("playlist-add", "apps.music.playlist.new", false, [this] { openDialog(Dialog::NewPlaylist); }));
    lists->content()->addChild(std::move(column));
    side->addChild(std::move(lists));
    const bool settings = m_view == View::Settings && m_query.empty();
    side->addChild(entryButton("settings", "apps.music.settings.title", settings, [this] { show(View::Settings); }));
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
    const std::int64_t playlist = m_view == View::Playlist && m_query.empty() ? m_playlistId : 0;
    m_trackAdapter = std::make_unique<TrackListAdapter>(
        std::move(rows), scale,
        TrackListCallbacks{
            .isCurrent = [this, items](std::size_t index) { return m_model.tracks[(*items)[index]].path == m_player.currentItem(); },
            .activate = [this, items](std::size_t index) { playTracks(*items, index, false); },
            .isLiked = [this, items](std::size_t index) { return m_playlists.liked(m_model.tracks[(*items)[index]].path); },
            .toggleLike = [this, items](std::size_t index) { toggleLike(m_model.tracks[(*items)[index]].path); },
            .menu =
                [this, items, playlist](std::size_t index, float x, float y) {
                  openSongMenu({m_model.tracks[(*items)[index]].path}, x, y, playlist, index);
                },
        }
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
      float scale, std::unique_ptr<Node> extra
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
      const float coverSide = (width < 560.0F * scale ? 96.0F : kDetailCover) * scale;
      image->setSize(coverSide, coverSide);
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
    if (extra != nullptr) {
      buttons->addChild(std::move(extra));
    }
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
      case View::Playlist:
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
      case View::Playlist:
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
    case View::Playlist: {
      const Playlist* list = m_playlists.find(m_playlistId);
      if (list == nullptr) {
        break;
      }
      // Solo las que siguen en la biblioteca, en el orden de la lista.
      std::vector<std::size_t> tracks;
      double duration = 0.0;
      for (const auto& path : list->items) {
        if (const auto it = m_byPath.find(path); it != m_byPath.end()) {
          tracks.push_back(it->second);
          duration += m_model.tracks[it->second].duration;
        }
      }
      std::unique_ptr<Node> extra;
      if (!list->favorites) {
        extra = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
        const std::int64_t id = list->id;
        extra->addChild(ui::button({
            .glyph = "pencil",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.music.playlist.rename"),
            .onClick = [this] { openDialog(Dialog::RenamePlaylist); },
        }));
        extra->addChild(ui::button({
            .glyph = "trash",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.music.playlist.delete"),
            .onClick =
                [this, id] {
                  m_playlists.remove(id);
                  show(View::Albums);
                },
        }));
      }
      content->addChild(buildDetailHeader(
          tracks.empty() ? "" : m_model.tracks[tracks.front()].cover, playlistName(*list),
          joinDot({trp("apps.music.song-count", static_cast<long>(tracks.size())), tracks.empty() ? "" : totalDuration(duration)}),
          tracks, inner, scale, std::move(extra)
      ));
      if (tracks.empty()) {
        content->addChild(ui::label({
            .text = list->favorites ? tr("apps.music.playlist.empty-favorites") : tr("apps.music.playlist.empty"),
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxWidth = inner,
        }));
      } else {
        content->addChild(buildTracks(std::move(tracks), false, scale));
      }
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
    // Angosta: sin título al lado de la carátula ni barra de volumen; la de tiempo usa lo que queda.
    const bool showInfo = width >= kSongInfoFrom * scale;
    const bool showVolume = width >= kVolumeSliderFrom * scale;
    const float infoWidth = showInfo ? std::clamp(width * 0.22F, 120.0F * scale, 300.0F * scale) : 0.0F;
    auto song = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});
    auto cover = std::make_unique<CoverImage>();
    cover->setSize(side, side);
    m_nowCover = cover.get();
    song->addChild(std::move(cover));
    auto info = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .width = infoWidth});
    info->addChild(ui::label({.out = &m_nowTitle, .text = "", .fontSize = Style::fontSizeBody * scale, .fontWeight = FontWeight::SemiBold, .maxWidth = infoWidth, .maxLines = 1}));
    info->addChild(ui::label({.out = &m_nowArtist, .text = "", .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxWidth = infoWidth, .maxLines = 1}));
    if (showInfo) {
      song->addChild(std::move(info));
    }
    const std::string current = m_player.currentItem();
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
    // Agregar a una lista (a la izquierda, contrapeso del corazón: reproducir queda centrado sobre la barra).
    buttons->addChild(ui::button({
        .out = &m_addToButton,
        .glyph = "playlist-add",
        .enabled = currentTrack() != nullptr,
        .variant = ButtonVariant::Ghost,
        .tooltip = tr("apps.music.menu.add-to-playlist"),
        .onClick = [this] { openAddToPlaylist(); },
    }));
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
    // Me gusta, junto a los controles.
    buttons->addChild(ui::button({
        .out = &m_likeButton,
        .glyph = m_playlists.liked(current) ? "heart-filled" : "heart",
        .enabled = currentTrack() != nullptr,
        .variant = ButtonVariant::Ghost,
        .tooltip = tr("apps.music.like"),
        .onClick =
            [this] {
              if (currentTrack() != nullptr) {
                toggleLike(m_player.currentItem());
              }
            },
    }));
    center->addChild(std::move(buttons));
    // Lo que ocupan los demás: carátula y título, volumen, el botón del reproductor, márgenes y separaciones.
    const float others = side + infoWidth + (showVolume ? 150.0F : 40.0F) * scale + 40.0F * scale
        + Style::spaceMd * 7.0F * scale;
    const float timelineWidth = std::clamp(width - others, 120.0F * scale, 560.0F * scale);
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
            .visible = showVolume,
            .onValueChanged =
                [this](double value) {
                  m_player.setVolume(value);
                  if (m_player.state().muted) {
                    m_player.setMuted(false);
                  }
                },
        })
    );
    bar->addChild(ui::button({
        .glyph = "maximize",
        .variant = ButtonVariant::Ghost,
        .tooltip = tr("apps.music.fullscreen"),
        .onClick = [this] { setFullscreen(true); },
    }));
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

  std::unique_ptr<Node> MusicWindow::buildMini(float width, float height, float scale) {
    const float pad = Style::spaceMd * scale;
    auto root = ui::node({.width = width, .height = height});
    root->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface), .width = width, .height = height}));
    auto background = std::make_unique<CoverImage>();
    background->setBlur(24);
    background->setSize(width, height);
    background->setOpacity(0.28F);
    m_miniBackground = static_cast<CoverImage*>(root->addChild(std::move(background)));

    // Alta y angosta: carátula arriba; si no, a la izquierda.
    const bool vertical = height > width * 1.2F;
    const float controls = 116.0F * scale; // título, artista, botones y barra de tiempo
    const float side = vertical ? std::max(0.0F, std::min(width - pad * 2.0F, height - controls - pad * 3.0F))
                                : std::max(0.0F, std::min(height - pad * 2.0F, width * 0.4F));
    const float textWidth = vertical ? width - pad * 2.0F : std::max(0.0F, width - side - pad * 3.0F);
    auto cover = std::make_unique<CoverImage>();
    cover->setSize(side, side);
    cover->setPosition(vertical ? std::round((width - side) * 0.5F) : pad, pad);
    m_nowCover = static_cast<CoverImage*>(root->addChild(std::move(cover)));
    m_app.setDragNode(m_nowCover); // la carátula mueve la ventana flotante

    auto info = ui::column({.align = FlexAlign::Center, .gap = 2.0F * scale, .width = textWidth});
    info->addChild(ui::label({.out = &m_nowTitle, .text = "", .fontSize = Style::fontSizeBody * scale, .fontWeight = FontWeight::SemiBold, .maxWidth = textWidth, .maxLines = 1}));
    info->addChild(ui::label({.out = &m_nowArtist, .text = "", .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxWidth = textWidth, .maxLines = 1}));
    auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
    buttons->addChild(ui::button({.glyph = "player-track-prev", .variant = ButtonVariant::Ghost, .tooltip = tr("apps.music.previous"), .onClick = [this] { m_player.previous(); }}));
    buttons->addChild(ui::button({.out = &m_playButton, .glyph = "player-play", .variant = ButtonVariant::Primary, .onClick = [this] { m_player.togglePause(); }}));
    buttons->addChild(ui::button({.glyph = "player-track-next", .variant = ButtonVariant::Ghost, .tooltip = tr("apps.music.next"), .onClick = [this] { m_player.next(); }}));
    buttons->addChild(ui::button({
        .out = &m_likeButton,
        .glyph = m_playlists.liked(m_player.currentItem()) ? "heart-filled" : "heart",
        .enabled = currentTrack() != nullptr,
        .variant = ButtonVariant::Ghost,
        .tooltip = tr("apps.music.like"),
        .onClick =
            [this] {
              if (currentTrack() != nullptr) {
                toggleLike(m_player.currentItem());
              }
            },
    }));
    info->addChild(std::move(buttons));
    info->addChild(ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceXs * scale, .width = textWidth},
        ui::label({.out = &m_positionLabel, .text = "0:00", .fontSize = Style::fontSizeCaption * scale, .minWidth = 34.0F * scale, .textAlign = TextAlign::End}),
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
        ui::label({.out = &m_durationLabel, .text = "0:00", .fontSize = Style::fontSizeCaption * scale, .minWidth = 34.0F * scale})
    ));
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      info->layout(*renderer);
    }
    info->setPosition(
        vertical ? pad : side + pad * 2.0F,
        vertical ? side + pad * 2.0F : std::round((height - info->height()) * 0.5F)
    );
    root->addChild(std::move(info));
    return root;
  }

  void MusicWindow::setFullscreen(bool fullscreen) {
    m_menu.clear();
    m_app.setFullscreen(fullscreen);
    m_app.requestRebuild();
  }

  namespace {
    constexpr std::size_t kLavaBlobs = 5;
    // Recorrido de cada mancha: centro (fracción de la ventana), amplitud y velocidad (rad/s) por eje, fase.
    struct LavaPath {
      float cx, cy, ax, ay, sx, sy, phase, size;
    };
    constexpr std::array<LavaPath, kLavaBlobs> kLavaPaths{{
        {0.25F, 0.30F, 0.22F, 0.20F, 0.071F, 0.053F, 0.0F, 0.85F},
        {0.75F, 0.25F, 0.20F, 0.24F, 0.059F, 0.083F, 1.7F, 0.75F},
        {0.50F, 0.70F, 0.28F, 0.18F, 0.047F, 0.067F, 3.1F, 0.95F},
        {0.15F, 0.80F, 0.18F, 0.22F, 0.089F, 0.043F, 4.4F, 0.65F},
        {0.85F, 0.75F, 0.21F, 0.19F, 0.061F, 0.079F, 5.6F, 0.70F},
    }};
    // Mancha difusa: blanco con alfa que cae suave desde el centro (premultiplicado); se tiñe con setTint.
    const std::vector<std::uint8_t>& lavaTexture(int size) {
      static std::vector<std::uint8_t> pixels;
      if (pixels.empty()) {
        pixels.resize(static_cast<std::size_t>(size * size * 4));
        const float half = static_cast<float>(size) * 0.5F;
        for (int y = 0; y < size; ++y) {
          for (int x = 0; x < size; ++x) {
            const float dx = (static_cast<float>(x) + 0.5F - half) / half;
            const float dy = (static_cast<float>(y) + 0.5F - half) / half;
            // Núcleo sólido hasta el 50 % del radio y borde difuso (smoothstep) hasta el final.
            const float r = std::clamp((std::sqrt(dx * dx + dy * dy) - 0.5F) / 0.5F, 0.0F, 1.0F);
            const float falloff = 1.0F - r * r * (3.0F - 2.0F * r);
            const auto a = static_cast<std::uint8_t>(std::lround(falloff * 255.0F));
            std::fill_n(pixels.begin() + (y * size + x) * 4, 4, a);
          }
        }
      }
      return pixels;
    }
    constexpr int kLavaTexture = 128;
  } // namespace

  void MusicWindow::applyLavaColors(const std::string& cover) {
    if (m_lavaBlobs.empty() || cover == m_lavaCover) {
      return;
    }
    m_lavaCover = cover;
    std::vector<Rgb> colors;
    if (!cover.empty()) {
      if (auto image = loadImageFile(cover, 48)) {
        colors = dominantColors(image->rgba, kLavaBlobs);
      }
    }
    for (std::size_t i = 0; i < m_lavaBlobs.size(); ++i) {
      if (colors.empty()) {
        // Sin carátula: los colores del tema.
        constexpr std::array<ColorRole, 3> roles{ColorRole::Primary, ColorRole::Tertiary, ColorRole::Secondary};
        m_lavaBlobs[i]->setTint(resolveColorSpec(colorSpecFromRole(roles[i % roles.size()])));
      } else {
        const Rgb& c = colors[i % colors.size()];
        m_lavaBlobs[i]->setTint(Color{c[0], c[1], c[2], 1.0F});
      }
    }
    m_app.invalidate();
  }

  void MusicWindow::stepLava() {
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_lavaStart).count();
    for (std::size_t i = 0; i < m_lavaBlobs.size(); ++i) {
      const LavaPath& p = kLavaPaths[i];
      Image* blob = m_lavaBlobs[i];
      const float x = m_lavaWidth * (p.cx + p.ax * std::sin(t * p.sx + p.phase));
      const float y = m_lavaHeight * (p.cy + p.ay * std::cos(t * p.sy + p.phase * 0.7F));
      blob->setPosition(std::round(x - blob->width() * 0.5F), std::round(y - blob->height() * 0.5F));
    }
    m_app.requestFrame();
  }

  std::unique_ptr<Node> MusicWindow::buildFullscreen(float width, float height, float scale) {
    const float pad = Style::spaceLg * 3.0F * scale;
    const Track* track = currentTrack();
    const std::string item = m_player.currentItem();
    auto root = ui::node({.width = width, .height = height});
    root->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface), .width = width, .height = height}));

    // Lava: manchas grandes y difusas que se mueven despacio; encima, un velo para que el texto se lea.
    m_lavaWidth = width;
    m_lavaHeight = height;
    const float base = std::max(width, height);
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      const auto& texture = lavaTexture(kLavaTexture);
      for (const LavaPath& path : kLavaPaths) {
        const float size = std::round(base * path.size * 1.1F); // el borde se desvanece: algo más grande que se ve
        auto blob = std::make_unique<Image>();
        blob->setSourceRaw(*renderer, texture.data(), texture.size(), kLavaTexture, kLavaTexture, kLavaTexture * 4, PixmapFormat::RGBA);
        blob->setFit(ImageFit::Stretch);
        blob->setSize(size, size);
        blob->setOpacity(0.85F);
        m_lavaBlobs.push_back(static_cast<Image*>(root->addChild(std::move(blob))));
      }
    }
    root->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface, 0.25F), .width = width, .height = height}));
    applyLavaColors(track != nullptr ? track->cover : "");
    stepLava();
    m_lavaTick.startRepeating(std::chrono::milliseconds(33), [this] { stepLava(); });

    // Abajo: la canción (carátula y título grande) y, debajo, el tiempo y los controles.
    const float controlsHeight = 120.0F * scale;
    const float side = std::clamp(std::min(width, height) * 0.30F, 120.0F * scale, 440.0F * scale);
    const float songTop = height - pad - controlsHeight - side - Style::spaceLg * scale;
    auto cover = std::make_unique<CoverImage>();
    cover->setSize(side, side);
    cover->setPosition(pad, songTop);
    m_nowCover = static_cast<CoverImage*>(root->addChild(std::move(cover)));

    // El texto va puesto antes de medir (si no, mide vacío y queda corrido); una línea cada uno: alto fijo.
    const float textWidth = std::max(0.0F, width - side - pad * 2.0F - Style::spaceLg * 2.0F * scale);
    auto info = ui::column({.align = FlexAlign::Start, .gap = Style::spaceSm * scale, .width = textWidth});
    info->addChild(ui::label({
        .out = &m_nowTitle,
        .text = track != nullptr ? track->title : (item.empty() ? tr("apps.music.nothing-playing") : fs::path(item).filename().string()),
        .fontSize = Style::fontSizeHeader * 3.0F * scale,
        .fontWeight = FontWeight::Bold,
        .maxWidth = textWidth,
        .maxLines = 1,
    }));
    info->addChild(ui::label({
        .out = &m_nowArtist,
        .text = track != nullptr ? joinDot({track->artist, track->album}) : "",
        .fontSize = Style::fontSizeHeader * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxWidth = textWidth,
        .maxLines = 1,
    }));
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      info->layout(*renderer);
    }
    info->setPosition(pad + side + Style::spaceLg * 2.0F * scale, songTop + side - info->height());
    root->addChild(std::move(info));

    const float barWidth = width - pad * 2.0F;
    auto controls = ui::column({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .width = barWidth});
    controls->addChild(ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .width = barWidth},
        ui::label({.out = &m_positionLabel, .text = "0:00", .fontSize = Style::fontSizeBody * scale, .minWidth = 48.0F * scale, .textAlign = TextAlign::End}),
        ui::slider({
            .out = &m_timeline,
            .minValue = 0.0,
            .maxValue = 1.0,
            .value = 0.0,
            .trackHeight = 6.0F * scale,
            .wheelAdjustEnabled = false,
            .flexGrow = 1.0F,
            .onValueChanged =
                [this](double value) {
                  if (!m_syncingTimeline) {
                    m_player.seekTo(value);
                  }
                },
        }),
        ui::label({.out = &m_durationLabel, .text = "0:00", .fontSize = Style::fontSizeBody * scale, .minWidth = 48.0F * scale})
    ));
    // Controles grandes centrados; salir, a la derecha.
    const float glyph = 26.0F * scale;
    const float button = 52.0F * scale;
    auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});
    const auto control = [&](Button** out, const char* name, const std::string& tooltip, std::function<void()> click) {
      buttons->addChild(ui::button({
          .out = out,
          .glyph = name,
          .glyphSize = glyph,
          .controlHeight = button,
          .variant = ButtonVariant::Ghost,
          .tooltip = tooltip,
          .minWidth = button,
          .onClick = std::move(click),
      }));
    };
    control(&m_likeButton, m_playlists.liked(item) ? "heart-filled" : "heart", tr("apps.music.like"), [this] {
      if (currentTrack() != nullptr) {
        toggleLike(m_player.currentItem());
      }
    });
    control(&m_shuffleButton, "arrows-shuffle", tr("apps.music.shuffle"), [this] { m_player.setShuffle(!m_player.shuffled()); });
    control(nullptr, "player-track-prev", tr("apps.music.previous"), [this] { m_player.previous(); });
    buttons->addChild(ui::button({
        .out = &m_playButton,
        .glyph = "player-play",
        .glyphSize = glyph * 1.3F,
        .controlHeight = button * 1.35F,
        .variant = ButtonVariant::Primary,
        .minWidth = button * 1.35F,
        .onClick = [this] { m_player.togglePause(); },
    }));
    control(nullptr, "player-track-next", tr("apps.music.next"), [this] { m_player.next(); });
    control(&m_repeatButton, "repeat", tr("apps.music.repeat"), [this] {
      const Repeat next = m_player.repeat() == Repeat::Off ? Repeat::All
          : m_player.repeat() == Repeat::All               ? Repeat::One
                                                           : Repeat::Off;
      m_player.setRepeat(next);
    });
    control(nullptr, "minimize", tr("apps.music.exit-fullscreen"), [this] { setFullscreen(false); });
    controls->addChild(std::move(buttons));
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      controls->layout(*renderer);
    }
    controls->setPosition(pad, height - pad - controls->height() + Style::spaceLg * scale);
    // Panel translúcido detrás del tiempo y los controles: se leen sobre cualquier color del fondo.
    const float inset = Style::spaceMd * scale;
    auto panel = ui::box({
        .fill = colorSpecFromRole(ColorRole::Surface, 0.8F),
        .radius = Style::spaceLg * scale,
        .width = controls->width() + inset * 2.0F,
        .height = controls->height() + inset * 2.0F,
    });
    panel->setPosition(controls->x() - inset, controls->y() - inset);
    root->addChild(std::move(panel));
    root->addChild(std::move(controls));
    return root;
  }

  // ── Reproductor a pantalla completa ──────────────────────────────────────────────────────────────

  std::unique_ptr<Node> MusicWindow::buildFullPlayer(float width, float height, float scale) {
    m_queueAdapter.reset();
    const Track* track = currentTrack();
    const std::string& item = m_player.currentItem();
    m_fullPlayerItem = item;
    if (item != m_lyricsItem) {
      resolveLyrics();
    }
    // Ancho: carátula a la izquierda y panel a la derecha. Angosto (mosaico): carátula arriba y panel debajo.
    const bool stacked = width < kStackedPlayerBelow * scale || height > width;
    const float pad = Style::spaceLg * (stacked ? 1.0F : 2.0F) * scale;
    const float textBlock = 64.0F * scale;
    const float minPanel = 240.0F * scale;
    const float panelWidth = stacked ? width - pad * 2.0F : std::clamp(width * 0.38F, 320.0F * scale, 520.0F * scale);
    const float leftWidth = stacked ? width - pad * 2.0F : std::max(0.0F, width - panelWidth - pad * 3.0F);
    const float side = stacked
        ? std::clamp(std::min(leftWidth, height * 0.36F), 0.0F, std::max(0.0F, height - pad * 3.0F - textBlock - minPanel))
        : std::max(0.0F, std::min(leftWidth, height - pad * 2.0F - textBlock));

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
    const float top = stacked ? pad : std::round((height - side - textBlock) * 0.5F);
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
    const float panelTop = stacked ? top + side + textBlock + pad * 0.5F : pad;
    const float panelHeight = std::max(0.0F, height - panelTop - pad);
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
    panel->setPosition(m_panelX, panelTop);
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
        TrackListCallbacks{
            .isCurrent = [this](std::size_t index) { return static_cast<std::int64_t>(index) == m_player.state().current; },
            .activate = [this](std::size_t index) { m_player.playIndex(index); },
            .isLiked =
                [this](std::size_t index) {
                  const auto& items = m_player.state().queue;
                  return index < items.size() && m_playlists.liked(items[index]);
                },
            .toggleLike =
                [this](std::size_t index) {
                  if (index < m_player.state().queue.size()) {
                    toggleLike(m_player.state().queue[index]);
                  }
                },
            .menu =
                [this](std::size_t index, float x, float y) {
                  if (index < m_player.state().queue.size()) {
                    openSongMenu({m_player.state().queue[index]}, x, y, 0, 0, static_cast<std::int64_t>(index));
                  }
                },
        }
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
    m_lyricAnchor = -1.0F;
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
    // Las líneas vacías separan estrofas. Sincronizada: margen arriba y abajo para que la primera y la última
    // también puedan quedar al centro, y las que no suenan atenuadas.
    const bool synced = m_lyrics.synced;
    const ColorSpec color =
        synced ? colorSpecFromRole(ColorRole::OnSurfaceVariant, kLyricDim) : colorSpecFromRole(ColorRole::OnSurface);
    auto block = std::make_unique<LyricsBlock>(
        width - Style::spaceMd * scale, synced ? 160.0F * scale : Style::spaceMd * scale,
        Style::spaceXs * scale * (synced ? 3.0F : 1.0F), Style::spaceLg * scale, Style::fontSizeTitle * scale,
        Style::fontSizeTitle * kLyricGrow * scale, color, synced
    );
    for (const LyricLine& lyric : m_lyrics.lines) {
      if (lyric.text.empty()) {
        m_lyricLabelOf.push_back(-1);
        block->addSpacer();
        continue;
      }
      m_lyricLabelOf.push_back(static_cast<int>(block->lineCount()));
      block->addLine(lyric.text);
    }
    m_lyricsBlock = block.get();
    scroll->content()->addChild(std::move(block));
    return scroll;
  }

  // ── Letra ─────────────────────────────────────────────────────────────────────────────────────────

  void MusicWindow::resolveLyrics() {
    m_lyricsItem = m_player.currentItem();
    m_lyrics = {};
    // Letra nueva: desde arriba, sin la transición ni el desplazamiento que quedaban de la anterior.
    if (AnimationManager* anim = m_fullRoot != nullptr ? m_fullRoot->animationManager() : nullptr; anim != nullptr) {
      anim->cancelForOwner(&m_lyricIndex);
      anim->cancelForOwner(&m_lyricsScroll);
    }
    m_lyricsScroll.offset = 0.0F;
    m_lyricAnchor = -1.0F;
    m_lyricIndex = -1;
    m_centerLyric = false;
    m_lastPosition = -1.0;
    // Hasta que mpv avise la posición de la canción nueva, la que tiene es la de la anterior.
    m_stalePosition = m_player.state().position;
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

  double MusicWindow::estimatedPosition() const {
    // mpv avisa la posición cada tanto (en audio, espaciado): entre aviso y aviso se calcula con el reloj.
    const auto& state = m_player.state();
    if (state.paused || state.idle) {
      return state.position;
    }
    const double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_positionAt).count();
    return state.position + std::min(since, 1.0);
  }

  void MusicWindow::updateLyricsHighlight() {
    const auto& state = m_player.state();
    if (state.position != m_lastPosition) {
      m_lastPosition = state.position;
      m_positionAt = std::chrono::steady_clock::now();
    }
    // Mientras se ve una letra sincronizada sonando, un reloj propio la mantiene a tiempo.
    const bool watch = m_lyrics.synced && m_lyricsBlock != nullptr && !state.paused && !state.idle;
    if (watch && !m_lyricsTick.active()) {
      m_lyricsTick.startRepeating(std::chrono::milliseconds(50), [this] { updateLyricsHighlight(); });
    } else if (!watch) {
      m_lyricsTick.stop();
    }
    if (!m_lyrics.synced || m_lyricsBlock == nullptr || m_lyricLabelOf.size() != m_lyrics.lines.size()) {
      return;
    }
    if (m_stalePosition >= 0.0 && state.position == m_stalePosition && state.position > 0.5) {
      return;
    }
    m_stalePosition = -1.0;
    const long line = currentLine(m_lyrics, estimatedPosition());
    if (line == m_lyricIndex) {
      return;
    }
    const auto labelAt = [&](long index) -> long {
      return index < 0 || static_cast<std::size_t>(index) >= m_lyricLabelOf.size()
          ? -1
          : m_lyricLabelOf[static_cast<std::size_t>(index)];
    };
    AnimationManager* anim = m_lyricsBlock->animationManager();
    if (anim != nullptr) {
      anim->cancelForOwner(&m_lyricIndex); // la transición anterior termina donde iba
    }
    // La que se va vuelve a su tamaño; la nueva crece con rebote y la letra se desliza a centrarla, en un mismo
    // movimiento.
    const long to = labelAt(line);
    m_lyricIndex = line;
    // Cada línea parte del tamaño que tenga ahora (una transición interrumpida sigue desde donde iba, sin saltos):
    // las que estaban crecidas vuelven a su tamaño y la nueva crece desde el suyo.
    std::vector<std::pair<std::size_t, float>> shrinking;
    for (std::size_t i = 0; i < m_lyricsBlock->lineCount(); ++i) {
      if (static_cast<long>(i) != to && m_lyricsBlock->grow(i) != 0.0F) {
        shrinking.emplace_back(i, m_lyricsBlock->grow(i));
      }
    }
    const float growFrom = to >= 0 ? std::clamp(m_lyricsBlock->grow(static_cast<std::size_t>(to)), 0.0F, 1.0F) : 0.0F;

    m_scrollFrom = m_lyricsScroll.offset;
    m_lyricT = 0.0F;
    m_centerLyric = true; // sin línea (antes de la primera o entre estrofas) vuelve arriba
    // El foco de atenuación va de donde estaba a la línea nueva; entre estrofas se queda.
    const float anchorFrom = m_lyricAnchor;
    const float anchorTo = to >= 0 ? static_cast<float>(to) : (line < 0 ? -1.0F : anchorFrom);
    const auto apply = [this, shrinking, to, growFrom, anchorFrom, anchorTo](float t) {
      if (m_lyricsBlock == nullptr) {
        return;
      }
      m_lyricT = t;
      const float done = std::clamp(t, 0.0F, 1.0F);
      for (const auto& [index, start] : shrinking) {
        m_lyricsBlock->setGrow(index, start * (1.0F - done));
      }
      if (to >= 0) {
        m_lyricsBlock->setGrow(static_cast<std::size_t>(to), growFrom + (1.0F - growFrom) * t);
      }
      m_lyricAnchor = anchorFrom < 0.0F || anchorTo < 0.0F ? anchorTo : anchorFrom + (anchorTo - anchorFrom) * done;
      m_lyricsBlock->setAnchor(m_lyricAnchor);
      // Todo con medidas ya tomadas y un cuadro nuevo por paso: movimiento continuo.
      m_lyricsBlock->place();
      centerLyricLine();
      m_app.requestFrame();
    };
    if (anim == nullptr) {
      apply(1.0F);
      return;
    }
    anim->animate(
        0.0F, 1.0F, 600.0F, Easing::EaseOutBack, apply, [this] { m_centerLyric = false; }, &m_lyricIndex
    );
    // Las animaciones avanzan por reloj desde ahora: sin un cuadro ya, el primero podía llegar tarde y la transición
    // aparecía casi terminada (la línea grande «de golpe»).
    apply(0.0F);
  }

  void MusicWindow::centerLyricLine() {
    // Tras cada acomodo de la transición: la línea resaltada (que va creciendo) se lleva al centro con el mismo
    // avance de la animación, rebote incluido.
    if (!m_centerLyric || m_lyricsBlock == nullptr || m_lyricsView == nullptr) {
      return;
    }
    const long label = m_lyricIndex >= 0 && static_cast<std::size_t>(m_lyricIndex) < m_lyricLabelOf.size()
        ? m_lyricLabelOf[static_cast<std::size_t>(m_lyricIndex)]
        : -1;
    // Antes de la primera línea: arriba del todo. Entre estrofas (línea vacía): se queda donde está.
    if (label < 0 && m_lyricIndex >= 0) {
      return;
    }
    const float target = label < 0
        ? 0.0F
        : std::max(0.0F, m_lyricsBlock->centerOf(static_cast<std::size_t>(label)) - m_lyricsView->height() * 0.5F);
    m_lyricsView->setScrollOffset(m_scrollFrom + (target - m_scrollFrom) * m_lyricT);
    m_app.requestFrame();
  }

  // ── Ajustes ───────────────────────────────────────────────────────────────────────────────────────

  void MusicWindow::applySettings() {
    const auto& settings = m_state.settings;
    m_player.setCrossfade(settings.crossfade);
    m_player.setGapless(settings.gapless);
    m_player.setReplayGain(static_cast<int>(settings.replayGain));
    m_player.setEqualizer(settings.equalizer ? &settings.eqGains : nullptr);
  }

  void MusicWindow::fetchMissingCovers() {
    // Una petición por álbum sin carátula (o por canción suelta, sin álbum); lo ya buscado sin éxito no se repite.
    // Las de un servidor vienen de él (siempre); las locales, de internet si está activado.
    const bool online = m_state.settings.covers.on(Source::Online);
    for (const Album& album : m_model.albums) {
      if (!album.cover.empty() || album.tracks.empty()) {
        continue;
      }
      const auto groups = album.title.empty() ? album.tracks : std::vector<std::size_t>{album.tracks.front()};
      for (std::size_t index : groups) {
        const Track& track = m_model.tracks[index];
        const bool remote = isRemote(track.path);
        if (!remote && !online) {
          continue;
        }
        const fs::path target = coverPathFor(track, m_library.coversDir());
        if (m_coversFetching.contains(target.string()) || OnlineService::knownMissing(target)) {
          continue;
        }
        // Las canciones que comparten esa carátula: el álbum entero, o solo esa.
        std::vector<std::string> paths;
        for (std::size_t t : album.title.empty() ? std::vector<std::size_t>{index} : album.tracks) {
          paths.push_back(m_model.tracks[t].path);
        }
        m_coversFetching.insert(target.string());
        auto done = [this, target, paths = std::move(paths)](bool ok) {
          m_coversFetching.erase(target.string());
          if (ok) {
            applyCover(paths, target.string());
          }
        };
        std::error_code ec;
        if (fs::exists(target, ec)) {
          done(true); // ya estaba guardada (otro álbum igual, o una sincronización anterior)
        } else if (remote) {
          m_online.downloadCover(m_remote->coverUrl(track.path), target, std::move(done));
        } else {
          m_online.fetchCover(track, target, std::move(done));
        }
      }
    }
  }

  void MusicWindow::applyCover(const std::vector<std::string>& paths, const std::string& cover) {
    m_library.setCover(paths, cover);
    for (const auto& path : paths) {
      if (const auto it = m_byPath.find(path); it != m_byPath.end()) {
        m_model.tracks[it->second].cover = cover;
      }
    }
    for (Album& a : m_model.albums) {
      if (a.cover.empty() && !a.tracks.empty() && m_model.tracks[a.tracks.front()].cover == cover) {
        a.cover = cover;
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
  }

  // ── Servidores ────────────────────────────────────────────────────────────────────────────────────

  void MusicWindow::syncServers() {
    for (const Server& server : m_state.servers) {
      syncServer(server.id);
    }
  }

  void MusicWindow::syncServer(const std::string& id) {
    const auto it = std::ranges::find(m_state.servers, id, &Server::id);
    if (it == m_state.servers.end() || m_serverStatus[id] == ServerStatus::Syncing) {
      return;
    }
    m_serverStatus[id] = ServerStatus::Syncing;
    m_app.requestRebuild();
    const Server server = *it;
    m_remote->sync(server, [this, server](bool ok, std::vector<Track> tracks) {
      if (!ok) {
        // Sin red o error: se queda lo que ya había de ese servidor.
        m_serverStatus[server.id] = ServerStatus::Failed;
        m_app.requestRebuild();
        return;
      }
      m_serverStatus[server.id] = ServerStatus::Ready;
      m_serverCount[server.id] = tracks.size();
      m_library.replaceRemote(remotePrefix(server), tracks);
      setModel(m_library.load());
      fetchMissingCovers();
    });
  }

  void MusicWindow::removeServer(const std::string& id) {
    const auto it = std::ranges::find(m_state.servers, id, &Server::id);
    if (it == m_state.servers.end()) {
      return;
    }
    m_library.replaceRemote(remotePrefix(*it), {});
    m_remote->forget(id);
    m_state.servers.erase(it);
    m_serverStatus.erase(id);
    saveState();
    setModel(m_library.load());
  }

  void MusicWindow::submitServer() {
    ServerForm& form = m_serverForm;
    if (form.busy) {
      return;
    }
    Server server;
    server.id = std::format("{:08x}", g_random_int());
    server.kind = form.kind;
    server.url = normalizeUrl(form.url);
    server.user = form.user;
    server.name = form.name.empty() ? server.url.substr(server.url.find("://") + 3) : form.name;
    if (server.url.empty() || server.user.empty()) {
      form.error = tr("apps.music.servers.missing");
      m_app.requestRebuild();
      return;
    }
    form.busy = true;
    form.error.clear();
    m_app.requestRebuild();
    m_remote->connect(server, form.password, [this](bool ok, std::string error, Server connected) {
      m_serverForm.busy = false;
      if (!ok) {
        m_serverForm.error = error == "unreachable" ? tr("apps.music.servers.unreachable")
                                                    : tr("apps.music.servers.rejected", "error", error);
        m_app.requestRebuild();
        return;
      }
      m_serverForm = {};
      m_dialog = Dialog::None;
      m_state.servers.push_back(connected);
      m_remote->setServers(m_state.servers);
      saveState();
      syncServer(connected.id);
    });
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
              .maxWidth = std::max(120.0F * scale, width - 200.0F * scale),
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
              .maxWidth = std::max(inner * 0.5F, inner - 260.0F * scale),
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

    // Servidores.
    page->addChild(section("server", "apps.music.servers.title"));
    page->addChild(ui::label({
        .text = tr("apps.music.servers.hint"),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxWidth = inner,
    }));
    for (const Server& server : m_state.servers) {
      auto row = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
          .width = inner,
      });
      const auto status = m_serverStatus.contains(server.id) ? m_serverStatus.at(server.id) : ServerStatus::Idle;
      std::string detail = server.kind == ServerKind::Jellyfin ? "Jellyfin" : "Subsonic";
      if (status == ServerStatus::Syncing) {
        detail = joinDot({detail, tr("apps.music.servers.syncing")});
      } else if (status == ServerStatus::Failed) {
        detail = joinDot({detail, tr("apps.music.servers.failed")});
      } else if (status == ServerStatus::Ready) {
        detail = joinDot({detail, trp("apps.music.song-count", static_cast<long>(m_serverCount[server.id]))});
      }
      auto text = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F});
      text->addChild(ui::label({.text = server.name, .fontSize = Style::fontSizeBody * scale, .fontWeight = FontWeight::SemiBold, .maxLines = 1}));
      text->addChild(ui::label({
          .text = joinDot({detail, server.user + " @ " + server.url}),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(status == ServerStatus::Failed ? ColorRole::Error : ColorRole::OnSurfaceVariant),
          .maxWidth = std::max(120.0F * scale, inner - 160.0F * scale),
          .maxLines = 1,
          .ellipsize = TextEllipsize::Middle,
      }));
      row->addChild(ui::glyph({.glyph = "server", .glyphSize = 18.0F * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));
      row->addChild(std::move(text));
      row->addChild(ui::button({
          .glyph = "refresh",
          .enabled = status != ServerStatus::Syncing,
          .variant = ButtonVariant::Ghost,
          .tooltip = tr("apps.music.servers.sync"),
          .onClick = [this, id = server.id] { syncServer(id); },
      }));
      row->addChild(ui::button({
          .glyph = "x",
          .variant = ButtonVariant::Ghost,
          .tooltip = tr("apps.music.servers.remove"),
          .onClick = [this, id = server.id] { removeServer(id); },
      }));
      page->addChild(std::move(row));
    }
    page->addChild(ui::row(
        {.align = FlexAlign::Center},
        ui::button({
            .text = tr("apps.music.servers.add"),
            .glyph = "plus",
            .variant = ButtonVariant::Outline,
            .onClick =
                [this] {
                  m_serverForm = {};
                  openDialog(Dialog::AddServer);
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

    // Ecualizador.
    page->addChild(section("adjustments-horizontal", "apps.music.settings.section.equalizer"));
    page->addChild(setting("apps.music.settings.equalizer", toggle(settings.equalizer, [this] { applySettings(); })));
    {
      std::vector<std::string> presets;
      std::size_t selected = kEqPresets.size(); // «personalizado»
      for (std::size_t i = 0; i < kEqPresets.size(); ++i) {
        presets.push_back(tr(std::string("apps.music.eq.") + kEqPresets[i].name));
        if (kEqPresets[i].gains == settings.eqGains) {
          selected = i;
        }
      }
      presets.push_back(tr("apps.music.eq.custom"));
      page->addChild(setting(
          "apps.music.settings.eq-preset",
          ui::select({
              .options = std::move(presets),
              .selectedIndex = selected,
              .width = 170.0F * scale,
              .onSelectionChanged =
                  [this](std::size_t index, std::string_view) {
                    if (index < kEqPresets.size()) {
                      m_state.settings.eqGains = kEqPresets[index].gains;
                      applySettings();
                      saveState();
                      m_app.requestRebuild();
                    }
                  },
          })
      ));
      const auto gainText = [](double gain) { return std::format("{:+.0f} dB", gain); };
      for (std::size_t band = 0; band < kEqBands.size(); ++band) {
        const int hz = kEqBands[band];
        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .width = inner});
        row->addChild(ui::label({
            .text = hz >= 1000 ? std::format("{} kHz", hz / 1000) : std::format("{} Hz", hz),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .minWidth = 64.0F * scale,
        }));
        row->addChild(ui::slider({
            .minValue = -kEqMaxGain,
            .maxValue = kEqMaxGain,
            .step = 1.0,
            .value = settings.eqGains[band],
            .flexGrow = 1.0F,
            .onValueChanged =
                [this, band, gainText](double value) {
                  m_state.settings.eqGains[band] = value;
                  applySettings();
                  if (m_eqLabels[band] != nullptr) {
                    m_eqLabels[band]->setText(gainText(value));
                    m_app.invalidate();
                  }
                },
            .onDragEnd =
                [this] {
                  saveState();
                  m_app.requestRebuild(); // el preajuste pasa a «personalizado»
                },
        }));
        row->addChild(ui::label({
            .out = &m_eqLabels[band],
            .text = gainText(settings.eqGains[band]),
            .fontSize = Style::fontSizeCaption * scale,
            .minWidth = 56.0F * scale,
        }));
        page->addChild(std::move(row));
      }
    }

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

  // ── Listas, «me gusta» y menú ─────────────────────────────────────────────────────────────────────

  std::string MusicWindow::playlistName(const Playlist& list) const {
    return list.favorites ? tr("apps.music.favorites") : list.name;
  }

  void MusicWindow::openPlaylist(std::int64_t id) {
    m_fullPlayer = false;
    m_query.clear();
    m_detail = {};
    if (m_view != View::Playlist) {
      m_returnView = m_view;
    }
    m_view = View::Playlist;
    m_playlistId = id;
    m_app.requestRebuild();
  }

  void MusicWindow::toggleLike(const std::string& path) {
    const bool first = std::ranges::none_of(m_playlists.all(), &Playlist::favorites);
    m_playlists.toggleLike(path);
    if (m_likeButton != nullptr && path == m_player.currentItem()) {
      m_likeButton->setGlyph(m_playlists.liked(path) ? "heart-filled" : "heart");
    }
    // Las filas cambian de corazón; la barra lateral, si recién nació «Favoritas»; la lista, si es la abierta.
    if (first || (m_view == View::Playlist && m_query.empty())) {
      m_app.requestRebuild();
      return;
    }
    for (VirtualListView* list : {m_trackList, m_queueList}) {
      if (list != nullptr) {
        list->notifyDataChanged();
      }
    }
    m_app.invalidate();
  }

  void MusicWindow::openSongMenu(
      std::vector<std::string> paths, float x, float y, std::int64_t playlist, std::size_t index, std::int64_t queueIndex
  ) {
    if (paths.empty()) {
      return;
    }
    std::vector<MenuEntry> menu;
    menu.push_back({tr("apps.music.menu.play-next"), [this, paths] { m_player.playNext(paths); }});
    menu.push_back({tr("apps.music.menu.add-to-queue"), [this, paths] { m_player.append(paths); }});
    const bool liked = m_playlists.liked(paths.front());
    menu.push_back({liked ? tr("apps.music.menu.unlike") : tr("apps.music.like"), [this, path = paths.front()] { toggleLike(path); }});
    if (const auto it = m_byPath.find(paths.front()); it != m_byPath.end()) {
      for (std::size_t a = 0; a < m_model.albums.size(); ++a) {
        if (std::ranges::contains(m_model.albums[a].tracks, it->second)) {
          menu.push_back({tr("apps.music.menu.go-to-album"), [this, a] {
                            m_fullPlayer = false;
                            m_view = View::Albums;
                            open({.kind = Detail::Kind::Album, .index = a});
                          }});
          break;
        }
      }
    }
    menu.push_back({.separator = true});
    for (const Playlist& list : m_playlists.all()) {
      if (!list.favorites && list.id != playlist) {
        menu.push_back({tr("apps.music.menu.add-to", "name", list.name), [this, id = list.id, paths] { m_playlists.add(id, paths); }});
      }
    }
    menu.push_back({tr("apps.music.menu.new-playlist"), [this, paths] { openDialog(Dialog::NewPlaylist, paths); }});
    if (queueIndex >= 0) {
      menu.push_back({.separator = true});
      menu.push_back({tr("apps.music.menu.remove-from-queue"), [this, queueIndex] {
                        m_player.removeAt(static_cast<std::size_t>(queueIndex));
                      }});
    }
    if (playlist != 0) {
      menu.push_back({.separator = true});
      menu.push_back({tr("apps.music.menu.remove-from-playlist"), [this, playlist, index] {
                        m_playlists.removeItem(playlist, index);
                        m_app.requestRebuild();
                      }});
    }
    m_menu = std::move(menu);
    m_menuX = x;
    m_menuY = y;
    m_menuAbove = false;
    m_app.requestRebuild();
  }

  void MusicWindow::openAddToPlaylist() {
    const std::string item = m_player.currentItem();
    if (currentTrack() == nullptr || m_addToButton == nullptr) {
      return;
    }
    std::vector<MenuEntry> menu;
    for (const Playlist& list : m_playlists.all()) {
      if (!list.favorites) {
        menu.push_back({tr("apps.music.menu.add-to", "name", list.name), [this, id = list.id, item] { m_playlists.add(id, {item}); }});
      }
    }
    menu.push_back({tr("apps.music.menu.new-playlist"), [this, item] { openDialog(Dialog::NewPlaylist, {item}); }});
    // Sobre el botón, hacia arriba (está en la barra de abajo).
    float x = 0.0F;
    float y = 0.0F;
    Node::absolutePosition(m_addToButton, x, y);
    m_menu = std::move(menu);
    m_menuX = x;
    m_menuY = y;
    m_menuAbove = true;
    m_app.requestRebuild();
  }

  std::unique_ptr<Node> MusicWindow::buildMenu(float width, float height, float scale) {
    auto layer = ui::node({.width = width, .height = height, .zIndex = 20});
    // Una pulsación fuera lo cierra (solo una nueva: la suelta del clic que lo abrió no cuenta).
    layer->addChild(ui::inputArea({
        .acceptedButtons = InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}),
        .width = width,
        .height = height,
        .onPress =
            [this](const InputArea::PointerData& data) {
              if (data.pressed) {
                DeferredCall::callLater([this] {
                  m_menu.clear();
                  m_app.requestRebuild();
                });
              }
            },
    }));
    std::vector<ContextMenuControlEntry> entries;
    for (std::size_t i = 0; i < m_menu.size(); ++i) {
      entries.push_back({.id = static_cast<std::int32_t>(i), .label = m_menu[i].label, .separator = m_menu[i].separator});
    }
    Renderer* renderer = m_app.renderer();
    const float menuWidth = std::clamp(
        renderer != nullptr ? ContextMenuControl::preferredWidth(*renderer, entries, scale, false) : 240.0F * scale,
        200.0F * scale, 360.0F * scale
    );
    const float menuHeight = ContextMenuControl::preferredHeight(entries, entries.size(), scale);
    const float x = std::clamp(m_menuX, 4.0F, std::max(4.0F, width - menuWidth - 4.0F));
    const float wantedY = m_menuAbove ? m_menuY - menuHeight - 4.0F : m_menuY;
    const float y = std::clamp(wantedY, 4.0F, std::max(4.0F, height - menuHeight - 4.0F));
    layer->addChild(ui::box({
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
        .border = colorSpecFromRole(ColorRole::OutlineVariant),
        .borderWidth = 1.0F,
        .width = menuWidth,
        .height = menuHeight,
        .configure = [x, y](Box& box) { box.setPosition(x, y); },
    }));
    auto menu = std::make_unique<ContextMenuControl>();
    menu->setContentScale(scale);
    menu->setMenuWidth(menuWidth);
    menu->setMaxVisible(entries.size());
    menu->setEntries(std::move(entries));
    menu->setRedrawCallback([this] { m_app.invalidate(); });
    menu->setOnActivate([this](const ContextMenuControlEntry& entry) {
      const auto index = static_cast<std::size_t>(entry.id);
      DeferredCall::callLater([this, index] {
        if (index >= m_menu.size()) {
          return;
        }
        auto action = m_menu[index].action;
        m_menu.clear();
        m_app.requestRebuild();
        if (action) {
          action();
        }
      });
    });
    menu->setPosition(x, y);
    menu->setSize(menuWidth, menuHeight);
    layer->addChild(std::move(menu));
    return layer;
  }

  void MusicWindow::openDialog(Dialog dialog, std::vector<std::string> paths) {
    m_dialog = dialog;
    m_dialogPaths = std::move(paths);
    const Playlist* list = m_playlists.find(m_playlistId);
    m_dialogText = dialog == Dialog::RenamePlaylist && list != nullptr ? list->name : "";
    m_focusSearch = false;
    m_app.requestRebuild();
  }

  void MusicWindow::submitDialog() {
    std::string name = m_dialogInput != nullptr ? m_dialogInput->value() : m_dialogText;
    name.erase(0, name.find_first_not_of(' '));
    name.erase(name.find_last_not_of(' ') + 1);
    if (name.empty()) {
      return;
    }
    if (m_dialog == Dialog::NewPlaylist) {
      const std::int64_t id = m_playlists.create(name);
      if (!m_dialogPaths.empty()) {
        m_playlists.add(id, m_dialogPaths);
      }
      m_dialog = Dialog::None;
      openPlaylist(id);
      return;
    }
    m_playlists.rename(m_playlistId, name);
    m_dialog = Dialog::None;
    m_app.requestRebuild();
  }

  std::unique_ptr<Node> MusicWindow::buildDialog(float width, float height, float scale) {
    auto layer = ui::node({.width = width, .height = height, .zIndex = 30});
    layer->addChild(ui::inputArea({.width = width, .height = height}));
    layer->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Scrim, 0.45F), .width = width, .height = height}));
    const float cardWidth = std::min(420.0F * scale, width - 32.0F * scale);
    auto card = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * scale,
        .padding = Style::spaceLg * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
        .width = cardWidth,
    });
    if (m_dialog == Dialog::AddServer) {
      card->addChild(buildServerForm(*card, cardWidth - Style::spaceLg * 2.0F * scale, scale));
      if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
        card->layout(*renderer);
      }
      card->setPosition(std::round((width - cardWidth) * 0.5F), std::round((height - card->height()) * 0.4F));
      layer->addChild(std::move(card));
      return layer;
    }
    card->addChild(ui::label({
        .text = m_dialog == Dialog::NewPlaylist ? tr("apps.music.playlist.new") : tr("apps.music.playlist.rename"),
        .fontSize = Style::fontSizeTitle * scale,
        .fontWeight = FontWeight::SemiBold,
    }));
    card->addChild(ui::input({
        .out = &m_dialogInput,
        .value = m_dialogText,
        .placeholder = tr("apps.music.playlist.name"),
        .onChange = [this](const std::string& value) { m_dialogText = value; },
        .onSubmit = [this](const std::string&) { DeferredCall::callLater([this] { submitDialog(); }); },
    }));
    auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale, .fillWidth = true});
    buttons->addChild(ui::button({
        .text = tr("apps.music.cancel"),
        .variant = ButtonVariant::Ghost,
        .onClick =
            [this] {
              m_dialog = Dialog::None;
              m_app.requestRebuild();
            },
    }));
    buttons->addChild(ui::button({
        .text = m_dialog == Dialog::NewPlaylist ? tr("apps.music.playlist.create") : tr("apps.music.playlist.save"),
        .variant = ButtonVariant::Primary,
        .onClick = [this] { submitDialog(); },
    }));
    card->addChild(std::move(buttons));
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      card->layout(*renderer);
    }
    card->setPosition(std::round((width - cardWidth) * 0.5F), std::round((height - card->height()) * 0.4F));
    layer->addChild(std::move(card));
    return layer;
  }

  std::unique_ptr<Node> MusicWindow::buildServerForm(Flex& card, float width, float scale) {
    ServerForm& form = m_serverForm;
    card.addChild(ui::label({.text = tr("apps.music.servers.add"), .fontSize = Style::fontSizeTitle * scale, .fontWeight = FontWeight::SemiBold}));
    card.addChild(ui::select({
        .options = std::vector<std::string>{"Jellyfin", tr("apps.music.servers.subsonic")},
        .selectedIndex = form.kind == ServerKind::Jellyfin ? 0U : 1U,
        .width = width,
        .onSelectionChanged =
            [this](std::size_t index, std::string_view) {
              m_serverForm.kind = index == 0 ? ServerKind::Jellyfin : ServerKind::Subsonic;
            },
    }));
    const auto field = [&](std::string& value, const char* placeholder, bool password, Input** out) {
      card.addChild(ui::input({
          .out = out,
          .value = value,
          .placeholder = tr(placeholder),
          .passwordMode = password,
          .width = width,
          .onChange = [&value](const std::string& text) { value = text; },
          .onSubmit = [this](const std::string&) { DeferredCall::callLater([this] { submitServer(); }); },
      }));
    };
    field(form.url, "apps.music.servers.url", false, &m_dialogInput); // el primero recibe el foco
    field(form.name, "apps.music.servers.name", false, nullptr);
    field(form.user, "apps.music.servers.user", false, nullptr);
    field(form.password, "apps.music.servers.password", true, nullptr);
    if (!form.error.empty()) {
      card.addChild(ui::label({
          .text = form.error,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
          .maxWidth = width,
      }));
    }
    auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale, .fillWidth = true});
    if (form.busy) {
      buttons->addChild(ui::spinner({}));
    }
    buttons->addChild(ui::button({
        .text = tr("apps.music.cancel"),
        .variant = ButtonVariant::Ghost,
        .onClick =
            [this] {
              m_dialog = Dialog::None;
              m_serverForm = {};
              m_app.requestRebuild();
            },
    }));
    buttons->addChild(ui::button({
        .text = form.busy ? tr("apps.music.servers.connecting") : tr("apps.music.servers.connect"),
        .enabled = !form.busy,
        .variant = ButtonVariant::Primary,
        .onClick = [this] { submitServer(); },
    }));
    return buttons;
  }

  bool MusicWindow::onKey(const KeyboardEvent& event) {
    if (!event.pressed || event.preedit) {
      return false;
    }
    const bool ctrl = (event.modifiers & KeyMod::Ctrl) != 0;
    // Escribiendo en la búsqueda, las teclas sueltas son texto.
    const InputArea* focused = m_app.input().focusedArea();
    const bool typing = (m_search != nullptr && focused == m_search->inputArea()) || m_dialog != Dialog::None;
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
      break; // sin Ctrl: «me gusta» (más abajo, si no se está escribiendo)
    case XKB_KEY_Escape:
      if (!m_menu.empty() || m_dialog != Dialog::None) {
        m_menu.clear();
        m_dialog = Dialog::None;
        m_app.requestRebuild();
        return true;
      }
      if (m_app.fullscreen()) {
        setFullscreen(false);
        return true;
      }
      if (m_fullPlayer) {
        setFullPlayer(false);
        return true;
      }
      if (!m_query.empty() || m_detail.kind != Detail::Kind::None || m_view == View::Playlist) {
        back();
        return true;
      }
      return false;
    case XKB_KEY_F11:
      setFullscreen(!m_app.fullscreen());
      return true;
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
    case XKB_KEY_f:
      if (currentTrack() != nullptr) {
        toggleLike(m_player.currentItem());
      }
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
