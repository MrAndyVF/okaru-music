#pragma once

// OKARU: Valandy publica MPRIS (org.mpris.MediaPlayer2) para que la barra de OKARU, playerctl y las teclas
// multimedia lo vean y lo controlen, con carátula, aleatorio y repetir. Corre en el hilo principal.
// Basado en el de okaru-play, copiado a propósito: Valandy no depende de las otras apps.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class SessionBus;

namespace sdbus {
  class IObject;
}

namespace okaru::music {

  class AudioPlayer;
  struct Track;

  class MprisServer {
  public:
    struct Hooks {
      std::function<const Track*()> current; // canción que suena (de la biblioteca); nullptr si no se conoce
      std::function<void()> raise;
      std::function<void()> quit;
    };

    MprisServer(SessionBus& bus, AudioPlayer& player, Hooks hooks);
    ~MprisServer();

    MprisServer(const MprisServer&) = delete;
    MprisServer& operator=(const MprisServer&) = delete;

    // Tras cada cambio del reproductor: avisa de las propiedades que cambiaron y de los saltos de posición.
    void update();

  private:
    struct Snapshot {
      std::string status;
      std::int64_t index = -1;
      std::string item;
      double duration = 0.0;
      double volume = 0.0;
      bool shuffle = false;
      std::string loop;
      bool canPrevious = false;
      bool canNext = false;
      bool loaded = false;

      bool operator==(const Snapshot&) const = default;
    };

    [[nodiscard]] Snapshot snapshot() const;

    SessionBus& m_bus;
    AudioPlayer& m_player;
    Hooks m_hooks;
    std::unique_ptr<sdbus::IObject> m_object;
    Snapshot m_last;
    double m_lastPosition = 0.0;
    std::chrono::steady_clock::time_point m_lastPositionAt;
  };

} // namespace okaru::music
