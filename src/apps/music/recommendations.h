#pragma once

// OKARU: recomendaciones de Valandy («Para ti»). Las de YouTube Music vienen de su portada (servers.h); estas
// salen de la biblioteca propia y no necesitan internet.

#include "apps/music/library.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace okaru::music {

  // Álbumes con archivos de este equipo, del agregado (o cambiado) más reciente al más viejo.
  [[nodiscard]] std::vector<std::size_t> recentAlbums(const Model& model, std::size_t count);
  // Álbumes al azar para volver a escuchar; con la misma `seed` (el día) salen los mismos.
  [[nodiscard]] std::vector<std::size_t> rediscoverAlbums(const Model& model, std::size_t count, std::uint32_t seed);

} // namespace okaru::music
