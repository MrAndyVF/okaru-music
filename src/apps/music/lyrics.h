#pragma once

// OKARU: letras de Valandy. Una letra es texto plano o LRC sincronizado ("[01:23.45] verso"); de dónde sale (la
// incrustada, un .lrc junto a la canción o LRCLIB) y en qué orden lo deciden los ajustes.

#include <filesystem>
#include <string>
#include <vector>

namespace okaru::music {

  struct Track;

  struct LyricLine {
    double time = -1.0; // segundos; -1 en una letra sin sincronizar
    std::string text;
  };

  struct Lyrics {
    std::vector<LyricLine> lines;
    bool synced = false;

    [[nodiscard]] bool empty() const noexcept { return lines.empty(); }
  };

  // LRC (con varias marcas por línea y etiquetas [ar:…] que se ignoran) o texto plano. Las estrofas quedan como
  // líneas vacías.
  [[nodiscard]] Lyrics parseLyrics(const std::string& text);
  // Índice de la línea que suena en `seconds` (la última con time <= seconds); -1 antes de la primera.
  [[nodiscard]] long currentLine(const Lyrics& lyrics, double seconds);
  // El .lrc (o .txt) con el mismo nombre que la canción, junto a ella; vacío si no hay.
  [[nodiscard]] std::string sidecarLyrics(const std::filesystem::path& audio);
  // Dónde se guarda la letra descargada de una canción (y su marca de «no encontrada»).
  [[nodiscard]] std::filesystem::path lyricsCachePath(const Track& track);

} // namespace okaru::music
