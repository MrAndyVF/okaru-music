#pragma once

// OKARU: motor de Valandy sobre libmpv, solo audio. La cola de reproducción ES la lista de mpv: así mpv precarga la
// siguiente canción y las une sin silencio (gapless), y repetir/aleatorio no necesitan lógica propia. La app solo
// le da órdenes y lee su estado (posición, lista, canción actual). Todo se usa desde el hilo principal.

#include "core/timer_manager.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct mpv_handle;

namespace okaru::music {

  enum class Repeat : std::uint8_t { Off, All, One };

  struct PlayerState {
    double position = 0.0;
    double duration = 0.0;
    double volume = 100.0;
    bool paused = true;
    bool muted = false;
    bool idle = true; // sin nada cargado (cola vacía o terminó)
    std::int64_t current = -1;      // índice en `queue`
    std::vector<std::string> queue; // rutas o URL, en orden de reproducción
  };

  // Cambio en el estado: `structural` = algo más que la posición (canción, cola, pausa…).
  using ChangeFn = std::function<void(bool structural)>;

  class AudioPlayer {
  public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_mpv != nullptr; }
    [[nodiscard]] const PlayerState& state() const noexcept { return m_state; }
    [[nodiscard]] bool shuffled() const noexcept { return m_shuffled; }
    [[nodiscard]] Repeat repeat() const noexcept { return m_repeat; }
    [[nodiscard]] const std::string& currentItem() const;

    void setOnChange(ChangeFn callback) { m_onChange = std::move(callback); }
    // Lo que mpv abre para una ruta de la biblioteca (las de servidores son URL de streaming). La cola sigue
    // hablando en rutas: las URL se traducen de vuelta al leer la lista de mpv.
    void setResolver(std::function<std::string(const std::string&)> resolver) { m_resolver = std::move(resolver); }

    // Reemplaza la cola y empieza por `start` (con aleatorio activo, `start` va primero y el resto se mezcla).
    void playQueue(const std::vector<std::string>& items, std::size_t start, bool paused = false);
    void playIndex(std::size_t index);
    void append(const std::vector<std::string>& items);
    void playNext(const std::vector<std::string>& items);
    void removeAt(std::size_t index);
    void clearQueue();
    void next();
    // Con más de 3 s reproducidos vuelve al principio de la canción; si no, a la anterior.
    void previous();

    void setPaused(bool paused);
    void togglePause() { setPaused(!m_state.paused); }
    void seekTo(double seconds);
    void seekBy(double seconds);
    void setVolume(double volume);
    void setMuted(bool muted);
    void setShuffle(bool shuffle);
    void setRepeat(Repeat repeat);
    // Segundos de fundido entre canciones (0 = sin fundido).
    void setCrossfade(int seconds);
    void setGapless(bool gapless);
    // 0 sin normalizar, 1 por pista, 2 por álbum (ReplayGain de las etiquetas).
    void setReplayGain(int mode);

  private:
    // Fundido: otro mpv («cola») sigue tocando el final de la canción que se va, bajando, mientras la principal ya
    // pasó a la siguiente y sube. Así la cola de reproducción sigue siendo una sola (la de la principal).
    void maybeStartCrossfade();
    void stepCrossfade();
    void finishCrossfade();
    void drainEvents();
    void onProperty(const char* name, int format, void* data);
    void command(const std::vector<std::string>& args);
    // Ruta → lo que abre mpv (y lo recuerda para traducir de vuelta).
    [[nodiscard]] std::string playable(const std::string& item);
    // Deja la canción actual y pone `before` delante y `after` detrás (sin cortar la que suena).
    void surround(const std::vector<std::string>& before, const std::vector<std::string>& after);

    mpv_handle* m_mpv = nullptr;
    PlayerState m_state;
    ChangeFn m_onChange;
    bool m_shuffled = false;
    Repeat m_repeat = Repeat::Off;
    std::vector<std::string> m_original; // la cola sin mezclar, para volver a ella
    std::function<std::string(const std::string&)> m_resolver;
    std::unordered_map<std::string, std::string> m_itemOf; // lo que se le dio a mpv → ruta de la biblioteca
    int m_crossfade = 0;
    mpv_handle* m_tail = nullptr;
    Timer m_fadeTimer;
    std::chrono::steady_clock::time_point m_fadeStart;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> m_eventsPending{false};
  };

  // "1:05", "1:02:03".
  [[nodiscard]] std::string formatTime(double seconds);
  // Mezcla `items` dejando `first` al principio.
  [[nodiscard]] std::vector<std::string> shuffledWithFirst(std::vector<std::string> items, std::size_t first);

} // namespace okaru::music
