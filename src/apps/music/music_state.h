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
  };

  [[nodiscard]] std::filesystem::path statePath();
  // Sin archivo: la carpeta de música del usuario (XDG_MUSIC_DIR o ~/Music).
  [[nodiscard]] MusicState loadState(const std::filesystem::path& path);
  bool saveState(const MusicState& state, const std::filesystem::path& path);

} // namespace okaru::music
