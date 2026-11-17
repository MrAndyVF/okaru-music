#pragma once

// OKARU: YouTube Music en Valandy. Sin API oficial: se habla con InnerTube (la API interna de la web de YouTube
// Music) con las cookies de una sesión iniciada, y el audio lo resuelve yt-dlp dentro de mpv.
//
// Inicio de sesión: se abre el navegador que elija el usuario con un perfil NUEVO y aislado (carpeta temporal en
// XDG_RUNTIME_DIR, borrada al terminar), apuntando a la página de Google. La contraseña se escribe solo ahí;
// Valandy nunca la ve. Cuando aparecen las cookies de sesión de youtube.com en ese perfil, se leen, se cierra el
// navegador, se borra el perfil y las cookies van al llavero del sistema (como los secretos de los servidores).

#include "core/timer_manager.h"

#include <nlohmann/json_fwd.hpp>
#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace okaru::music {

  struct Browser {
    std::string id;     // "zen", "firefox", "chrome"…
    std::string name;   // para mostrar
    std::string binary; // ruta encontrada en PATH
    bool chromium = false; // familia Chromium (cookies cifradas) o Firefox (cookies en claro)
  };

  // Los navegadores conocidos que están instalados, en orden de preferencia.
  [[nodiscard]] std::vector<Browser> installedBrowsers();

  // Cookies de youtube.com del perfil como cabecera "a=b; c=d" (vacía si no hay o no se pudo leer).
  [[nodiscard]] std::string readFirefoxCookies(const std::filesystem::path& profile);
  [[nodiscard]] std::string readChromiumCookies(const std::filesystem::path& profile);
  // Valor de una cookie en la cabecera ("" si no está).
  [[nodiscard]] std::string cookieValue(const std::string& header, const std::string& name);
  // Hay sesión iniciada: están las cookies que InnerTube necesita.
  [[nodiscard]] bool hasYouTubeSession(const std::string& header);
  // Cabecera Authorization de InnerTube (SAPISIDHASH y sus variantes 1P/3P), con `timestamp` en segundos.
  [[nodiscard]] std::string sapisidAuthorization(const std::string& cookies, long long timestamp);
  // Cookies en formato Netscape (el que lee yt-dlp con --cookies).
  [[nodiscard]] std::string netscapeCookies(const std::string& header);

  // Una canción tal como la describe InnerTube.
  struct YtTrack {
    std::string videoId;
    std::string title;
    std::string artist;
    bool artistLinked = false; // el artista viene enlazado (si no, puede ser otro texto suelto de la fila)
    std::string album;
    std::string albumId; // "MPREb_…" o vacío
    std::string thumbnail;
    double duration = 0.0;
    bool video = false; // videoclip o video de usuario (no la canción de YouTube Music)
  };
  // Encabezado de un álbum o una lista (para completar las canciones que no traen artista, álbum o carátula).
  struct YtHeader {
    std::string title;
    std::string artist;
    std::string thumbnail;
  };
  // Todas las filas de canciones de una respuesta (búsqueda, «me gusta», lista, álbum), sin repetir.
  [[nodiscard]] std::vector<YtTrack> parseYtTracks(const nlohmann::json& response);
  [[nodiscard]] YtHeader parseYtHeader(const nlohmann::json& response);
  // Listas o álbumes guardados de la biblioteca: browseId ("VL…", "MPREb_…").
  [[nodiscard]] std::vector<std::string> parseYtShelfIds(const nlohmann::json& response, const std::string& prefix);
  // Siguiente página: el token y si va en el cuerpo (continuationCommand) o en la URL (nextContinuationData).
  struct YtContinuation {
    std::string token;
    bool inBody = true;
  };
  [[nodiscard]] YtContinuation parseYtContinuation(const nlohmann::json& response);
  // Canciones de la radio de una canción (endpoint "next": playlistPanelVideoRenderer).
  [[nodiscard]] std::vector<YtTrack> parseYtPanel(const nlohmann::json& response);

  // Portada de YouTube Music: estantes de tarjetas (canciones, álbumes o listas).
  struct YtCard {
    enum class Kind : std::uint8_t { Song, Album, Playlist };
    Kind kind = Kind::Song;
    std::string title;
    std::string subtitle;
    std::string artist; // de la canción (para reproducirla con sus datos)
    std::string album;
    std::string thumbnail;
    std::string videoId;  // canción
    std::string browseId; // álbum ("MPREb_…") o lista ("VL…")
  };
  struct YtShelf {
    std::string title;
    std::vector<YtCard> cards;
  };
  [[nodiscard]] std::vector<YtShelf> parseYtHome(const nlohmann::json& response);

  // "3:54" → 234; "1:02:03" → 3723; otra cosa → 0.
  [[nodiscard]] double parseYtDuration(const std::string& value);
  // Miniatura en tamaño de carátula (las de googleusercontent llevan el tamaño en la URL).
  [[nodiscard]] std::string ytCoverSize(const std::string& url, int size);

  // URL directas de audio, resueltas por adelantado con yt-dlp (unos 3,5 s por canción) en un hilo aparte: así
  // la siguiente canción de la cola ya está lista cuando toca. Una URL recién resuelta tarda ~2 s en valer
  // (YouTube responde 403 antes), y vence a las horas (parámetro "expire").
  class StreamResolver {
  public:
    // Se llama en el hilo principal cuando una canción queda lista.
    using ReadyFn = std::function<void(const std::string& videoId)>;

    explicit StreamResolver(ReadyFn ready);
    ~StreamResolver();
    StreamResolver(const StreamResolver&) = delete;
    StreamResolver& operator=(const StreamResolver&) = delete;

    void setCookies(std::string file);
    // La URL lista y vigente, o vacía.
    [[nodiscard]] std::string cached(const std::string& videoId) const;
    // Agrega a la fila las que falten (en orden); las ya listas o en camino se ignoran.
    void prefetch(const std::vector<std::string>& videoIds);
    // Una URL que mpv no pudo abrir (venció, cambió la red): se olvida.
    void forget(const std::string& url);

  private:
    void run();

    struct Entry {
      std::string url;
      std::chrono::system_clock::time_point expires;
    };
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::string> m_queue;
    std::unordered_set<std::string> m_pending;
    std::unordered_map<std::string, Entry> m_cache;
    std::string m_cookies;
    bool m_stop = false;
    ReadyFn m_ready;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<pid_t> m_child{-1}; // yt-dlp en curso (se termina al cerrar)
    std::thread m_thread; // al final: arranca con todo lo demás ya construido
  };

  // Un videoId válido (11 caracteres de [A-Za-z0-9_-]).
  [[nodiscard]] bool isVideoId(const std::string& id);

  // Inicio de sesión con un perfil aislado del navegador elegido.
  class YouTubeLogin {
  public:
    // ok + cookies, o !ok + motivo ("closed" si se cerró el navegador, "timeout", "launch").
    using DoneFn = std::function<void(bool ok, std::string cookies, std::string error)>;

    YouTubeLogin() = default;
    ~YouTubeLogin();
    YouTubeLogin(const YouTubeLogin&) = delete;
    YouTubeLogin& operator=(const YouTubeLogin&) = delete;

    bool start(const Browser& browser, DoneFn done);
    void cancel();
    [[nodiscard]] bool active() const noexcept { return m_pid > 0; }
    // Restos de un inicio de sesión interrumpido (la app se cerró a mitad).
    static void cleanLeftovers();

  private:
    void poll();
    void finish(bool ok, std::string cookies, std::string error);

    Browser m_browser;
    std::filesystem::path m_profile;
    pid_t m_pid = -1;
    Timer m_poll;
    int m_ticks = 0;
    DoneFn m_done;
  };

} // namespace okaru::music
