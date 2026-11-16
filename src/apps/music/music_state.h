#pragma once

// OKARU: lo que Valandy recuerda entre sesiones (~/.local/state/okaru/music.toml): carpetas de música, volumen,
// aleatorio, repetir, el orden de cada vista y la cola con la canción en la que quedó.

#include "apps/music/audio_player.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace okaru::music {

  // De dónde salen la letra y la carátula: del archivo, de la carpeta (.lrc, cover.jpg) o de internet.
  enum class Source : std::uint8_t { Embedded, Local, Online };
  inline constexpr std::size_t kSourceCount = 3;

  // Fuentes en orden de prioridad; `enabled` va por fuente (índice = Source), no por posición.
  struct SourceOrder {
    std::array<Source, kSourceCount> order{Source::Embedded, Source::Local, Source::Online};
    std::array<bool, kSourceCount> enabled{true, true, true};

    [[nodiscard]] bool on(Source source) const { return enabled[static_cast<std::size_t>(source)]; }
    // Sube (delta -1) o baja (+1) la fuente en la posición `index`.
    void move(std::size_t index, int delta);
  };

  enum class ReplayGain : std::uint8_t { Off, Track, Album };

  struct Settings {
    int crossfade = 0; // segundos de fundido entre canciones; 0 = sin fundido
    bool gapless = true;
    ReplayGain replayGain = ReplayGain::Off;
    bool resume = true;      // al abrir, la cola donde quedó
    bool scanOnStart = true; // al abrir, buscar cambios en las carpetas
    SourceOrder lyrics;
    bool preferSynced = true; // seguir buscando una letra sincronizada si la primera no lo es
    SourceOrder covers;
  };

  struct MusicState {
    std::vector<std::filesystem::path> folders;
    double volume = 100.0;
    bool shuffle = false;
    Repeat repeat = Repeat::Off;
    std::vector<std::string> queue;
    std::size_t current = 0;
    // Orden de Álbumes, Artistas, Canciones y Géneros (índice = View), y si va invertido.
    std::array<std::uint8_t, 4> sort{};
    std::array<bool, 4> reverse{};
    Settings settings;
  };

  [[nodiscard]] std::filesystem::path statePath();
  // Sin archivo: la carpeta de música del usuario (XDG_MUSIC_DIR o ~/Music).
  [[nodiscard]] MusicState loadState(const std::filesystem::path& path);
  bool saveState(const MusicState& state, const std::filesystem::path& path);

} // namespace okaru::music
