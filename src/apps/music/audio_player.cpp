#include "apps/music/audio_player.h"

#include "core/deferred_call.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <mpv/client.h>
#include <random>
#include <strings.h>

namespace okaru::music {

  namespace {

    constexpr Logger kLog("music");
    constexpr double kRestartThreshold = 3.0;

    const mpv_node* mapValue(const mpv_node& map, const char* key) {
      if (map.format != MPV_FORMAT_NODE_MAP) {
        return nullptr;
      }
      for (int i = 0; i < map.u.list->num; ++i) {
        if (strcasecmp(map.u.list->keys[i], key) == 0) {
          return &map.u.list->values[i];
        }
      }
      return nullptr;
    }

  } // namespace

  AudioPlayer::AudioPlayer() {
    m_mpv = mpv_create();
    if (m_mpv == nullptr) {
      kLog.error("mpv_create failed");
      return;
    }
    const std::array<std::pair<const char*, const char*>, 12> options{{
        {"vo", "null"},
        {"video", "no"},         // la portada incrustada no es un video
        {"audio-display", "no"}, // ni se decodifica como imagen
        {"idle", "yes"},
        {"keep-open", "no"},
        {"terminal", "no"},
        {"input-default-bindings", "no"},
        {"prefetch-playlist", "yes"}, // abre la siguiente antes de que termine la actual
        {"gapless-audio", "weak"},
        {"volume-max", "100"},
        // yt-dlp solo para las rutas "ytdl://" (YouTube Music): las URL de los servidores se abren directo (sin
        // try_ytdl_first, el hook solo toma las "ytdl://" o las que mpv no pudo abrir).
        {"ytdl", "yes"},
        {"ytdl-format", "bestaudio/best"},
    }};
    for (const auto& [name, value] : options) {
      mpv_set_option_string(m_mpv, name, value);
    }
    if (mpv_initialize(m_mpv) < 0) {
      kLog.error("mpv_initialize failed");
      mpv_terminate_destroy(m_mpv);
      m_mpv = nullptr;
      return;
    }
    const std::array<std::pair<const char*, mpv_format>, 8> properties{{
        {"time-pos", MPV_FORMAT_DOUBLE},
        {"duration", MPV_FORMAT_DOUBLE},
        {"pause", MPV_FORMAT_FLAG},
        {"volume", MPV_FORMAT_DOUBLE},
        {"mute", MPV_FORMAT_FLAG},
        {"idle-active", MPV_FORMAT_FLAG},
        {"playlist-pos", MPV_FORMAT_INT64},
        {"playlist", MPV_FORMAT_NODE},
    }};
    for (const auto& [name, format] : properties) {
      mpv_observe_property(m_mpv, 0, name, format);
    }
    mpv_request_log_messages(m_mpv, "warn");
    mpv_set_wakeup_callback(
        m_mpv,
        [](void* ctx) {
          auto* self = static_cast<AudioPlayer*>(ctx);
          if (self->m_eventsPending.exchange(true)) {
            return;
          }
          DeferredCall::callLater([self, alive = self->m_alive] {
            if (*alive) {
              self->m_eventsPending = false;
              self->drainEvents();
            }
          });
        },
        this
    );
  }

  AudioPlayer::~AudioPlayer() {
    *m_alive = false;
    finishCrossfade();
    if (m_mpv != nullptr) {
      mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
      mpv_terminate_destroy(m_mpv);
    }
  }

  const std::string& AudioPlayer::currentItem() const {
    static const std::string kNone;
    const auto index = m_state.current;
    return index >= 0 && static_cast<std::size_t>(index) < m_state.queue.size()
        ? m_state.queue[static_cast<std::size_t>(index)]
        : kNone;
  }

  void AudioPlayer::command(const std::vector<std::string>& args) {
    if (m_mpv == nullptr) {
      return;
    }
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    // En orden: mpv procesa los comandos asíncronos uno tras otro.
    mpv_command_async(m_mpv, 0, argv.data());
  }

  void AudioPlayer::surround(const std::vector<std::string>& before, const std::vector<std::string>& after) {
    command({"playlist-clear"}); // deja solo la que suena
    for (std::size_t i = 0; i < before.size(); ++i) {
      command({"loadfile", playable(before[i]), "insert-at", std::to_string(i)});
    }
    for (const auto& item : after) {
      command({"loadfile", playable(item), "append"});
    }
  }

  void AudioPlayer::playQueue(const std::vector<std::string>& items, std::size_t start, bool paused) {
    if (m_mpv == nullptr || items.empty() || start >= items.size()) {
      return;
    }
    m_original = items;
    const std::vector<std::string> order = m_shuffled ? shuffledWithFirst(items, start) : items;
    const std::size_t first = m_shuffled ? 0 : start;
    int flag = paused ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
    command({"loadfile", playable(order[first]), "replace"});
    surround(
        {order.begin(), order.begin() + static_cast<std::ptrdiff_t>(first)},
        {order.begin() + static_cast<std::ptrdiff_t>(first) + 1, order.end()}
    );
  }

  void AudioPlayer::playIndex(std::size_t index) {
    finishCrossfade();
    if (index < m_state.queue.size()) {
      command({"playlist-play-index", std::to_string(index)});
      setPaused(false);
    }
  }

  void AudioPlayer::append(const std::vector<std::string>& items) {
    if (m_state.idle && m_state.queue.empty()) {
      playQueue(items, 0);
      return;
    }
    for (const auto& item : items) {
      command({"loadfile", playable(item), "append"});
      m_original.push_back(item);
    }
  }

  void AudioPlayer::playNext(const std::vector<std::string>& items) {
    if (m_state.idle && m_state.queue.empty()) {
      playQueue(items, 0);
      return;
    }
    // insert-next las pondría al revés si se insertan de a una: se insertan desde la última.
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
      command({"loadfile", playable(*it), "insert-next"});
    }
    m_original.insert(m_original.end(), items.begin(), items.end());
  }

  void AudioPlayer::removeAt(std::size_t index) {
    if (index < m_state.queue.size()) {
      // Solo una aparición: la misma canción puede estar dos veces en la cola.
      if (const auto it = std::ranges::find(m_original, m_state.queue[index]); it != m_original.end()) {
        m_original.erase(it);
      }
      command({"playlist-remove", std::to_string(index)});
    }
  }

  void AudioPlayer::retryFailed(std::int64_t entryId) {
    // YouTube: una URL directa que venció o se rechazó, o "ytdl://" pedido antes de que el script de yt-dlp de
    // mpv terminara de cargar (al abrir la app). Se vuelve a cargar en su lugar, una vez, y sigue desde ahí.
    std::int64_t count = 0;
    mpv_get_property(m_mpv, "playlist-count", MPV_FORMAT_INT64, &count);
    for (std::int64_t i = 0; i < count; ++i) {
      std::int64_t id = -1;
      mpv_get_property(m_mpv, std::format("playlist/{}/id", i).c_str(), MPV_FORMAT_INT64, &id);
      if (id != entryId) {
        continue;
      }
      char* name = mpv_get_property_string(m_mpv, std::format("playlist/{}/filename", i).c_str());
      const std::string url = name != nullptr ? name : "";
      mpv_free(name);
      const auto item = m_itemOf.find(url);
      const bool youtube = url.starts_with("ytdl://") || url.contains(".googlevideo.com/");
      if (!youtube || item == m_itemOf.end() || !m_retried.insert(url).second) {
        return;
      }
      if (m_onStreamFailed) {
        m_onStreamFailed(url); // olvida la URL directa: el reintento va por yt-dlp
      }
      m_retryTimer.start(std::chrono::milliseconds(800), [this, index = static_cast<std::size_t>(i), path = item->second] {
        if (index >= m_state.queue.size() || m_state.queue[index] != path) {
          return; // la cola cambió mientras tanto
        }
        command({"loadfile", playable(path), "insert-at", std::to_string(index)});
        command({"playlist-remove", std::to_string(index + 1)});
        command({"playlist-play-index", std::to_string(index)}); // la pausa se respeta
      });
      return;
    }
  }

  void AudioPlayer::refreshItem(std::size_t index) {
    if (index >= m_state.queue.size() || static_cast<std::int64_t>(index) == m_state.current) {
      return;
    }
    const std::string item = m_state.queue[index];
    command({"loadfile", playable(item), "insert-at", std::to_string(index)});
    command({"playlist-remove", std::to_string(index + 1)});
  }

  void AudioPlayer::clearQueue() {
    m_original.clear();
    command({"stop"});
  }

  void AudioPlayer::next() {
    finishCrossfade();
    if (m_repeat == Repeat::One) {
      // Con «repetir una», el botón igual pasa a la siguiente (loop-file solo afecta al final natural).
      command({"playlist-next", "force"});
      return;
    }
    command({"playlist-next"});
  }

  void AudioPlayer::previous() {
    finishCrossfade();
    if (m_state.position > kRestartThreshold || m_state.current <= 0) {
      seekTo(0.0);
      return;
    }
    command({"playlist-prev"});
  }

  void AudioPlayer::setPaused(bool paused) {
    if (m_mpv == nullptr) {
      return;
    }
    finishCrossfade(); // pausar a mitad del fundido corta la canción que se va
    // Terminada la cola, «reproducir» la empieza de nuevo.
    if (!paused && m_state.idle && !m_state.queue.empty()) {
      command({"playlist-play-index", "0"});
    }
    int flag = paused ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
  }

  void AudioPlayer::seekTo(double seconds) {
    finishCrossfade();
    command({"seek", std::format("{:.3f}", std::max(0.0, seconds)), "absolute+exact"});
  }

  void AudioPlayer::seekBy(double seconds) {
    finishCrossfade();
    command({"seek", std::format("{:.3f}", seconds), "relative+exact"});
  }

  void AudioPlayer::setVolume(double volume) {
    if (m_mpv != nullptr) {
      double value = std::clamp(volume, 0.0, 100.0);
      mpv_set_property_async(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE, &value);
    }
  }

  void AudioPlayer::setMuted(bool muted) {
    if (m_mpv != nullptr) {
      int flag = muted ? 1 : 0;
      mpv_set_property_async(m_mpv, 0, "mute", MPV_FORMAT_FLAG, &flag);
    }
  }

  void AudioPlayer::setShuffle(bool shuffle) {
    if (shuffle == m_shuffled) {
      return;
    }
    m_shuffled = shuffle;
    const std::string current = currentItem();
    if (m_state.queue.empty() || current.empty()) {
      return;
    }
    if (shuffle) {
      // La que suena queda primera; el resto, mezclado detrás.
      m_original = m_state.queue;
      auto rest = m_state.queue;
      rest.erase(rest.begin() + m_state.current);
      std::ranges::shuffle(rest, std::mt19937{std::random_device{}()});
      surround({}, rest);
    } else {
      // Vuelve al orden original alrededor de la que suena.
      const auto it = std::ranges::find(m_original, current);
      if (it == m_original.end()) {
        return;
      }
      surround({m_original.begin(), it}, {it + 1, m_original.end()});
    }
    if (m_onChange) {
      m_onChange(true);
    }
  }

  void AudioPlayer::setRepeat(Repeat repeat) {
    m_repeat = repeat;
    if (m_mpv != nullptr) {
      mpv_set_property_string(m_mpv, "loop-file", repeat == Repeat::One ? "inf" : "no");
      mpv_set_property_string(m_mpv, "loop-playlist", repeat == Repeat::All ? "inf" : "no");
    }
    if (m_onChange) {
      m_onChange(true);
    }
  }

  void AudioPlayer::drainEvents() {
    bool structural = false;
    bool changed = false;
    while (m_mpv != nullptr) {
      mpv_event* event = mpv_wait_event(m_mpv, 0);
      if (event->event_id == MPV_EVENT_NONE) {
        break;
      }
      switch (event->event_id) {
      case MPV_EVENT_PROPERTY_CHANGE: {
        const auto* prop = static_cast<mpv_event_property*>(event->data);
        onProperty(prop->name, prop->format, prop->data);
        changed = true;
        structural = structural || std::string_view(prop->name) != "time-pos";
        break;
      }
      case MPV_EVENT_LOG_MESSAGE: {
        const auto* message = static_cast<mpv_event_log_message*>(event->data);
        std::string_view text = message->text;
        if (text.ends_with('\n')) {
          text.remove_suffix(1);
        }
        kLog.warn("mpv/{}: {}", message->prefix, text);
        break;
      }
      case MPV_EVENT_END_FILE: {
        const auto* end = static_cast<mpv_event_end_file*>(event->data);
        if (end->reason == MPV_END_FILE_REASON_ERROR) {
          kLog.warn("mpv: {}", mpv_error_string(end->error));
          retryFailed(end->playlist_entry_id);
        }
        break;
      }
      default:
        break;
      }
    }
    if (changed) {
      maybeStartCrossfade();
    }
    if (changed && m_onChange) {
      m_onChange(structural);
    }
  }

  void AudioPlayer::setCrossfade(int seconds) {
    m_crossfade = std::clamp(seconds, 0, 12);
    if (m_crossfade == 0) {
      finishCrossfade();
    }
  }

  void AudioPlayer::setGapless(bool gapless) {
    if (m_mpv != nullptr) {
      mpv_set_property_string(m_mpv, "gapless-audio", gapless ? "weak" : "no");
    }
  }

  void AudioPlayer::setReplayGain(int mode) {
    m_replayGain = mode == 1 ? "track" : mode == 2 ? "album" : "no";
    if (m_mpv != nullptr) {
      mpv_set_property_string(m_mpv, "replaygain", m_replayGain.c_str());
    }
  }

  void AudioPlayer::setYtdlCookies(const std::string& file) {
    m_ytdlRaw = file.empty() ? std::string() : "cookies=" + file;
    if (m_mpv != nullptr) {
      mpv_set_property_string(m_mpv, "ytdl-raw-options", m_ytdlRaw.c_str());
    }
  }

  void AudioPlayer::setEqualizer(const EqGains* gains) {
    std::string filter = gains != nullptr ? equalizerFilter(*gains) : std::string();
    if (filter == m_filter) {
      return;
    }
    m_filter = std::move(filter);
    for (mpv_handle* handle : {m_mpv, m_tail}) {
      if (handle != nullptr) {
        mpv_set_property_string(handle, "af", m_filter.c_str());
      }
    }
  }

  void AudioPlayer::maybeStartCrossfade() {
    const auto& s = m_state;
    const auto fade = static_cast<double>(m_crossfade);
    const bool hasNext = s.current >= 0
        && (static_cast<std::size_t>(s.current) + 1 < s.queue.size() || (m_repeat == Repeat::All && s.queue.size() > 1));
    // Solo al final natural de una canción que dura bastante más que el fundido.
    if (m_crossfade == 0 || m_tail != nullptr || s.paused || s.idle || m_repeat == Repeat::One || !hasNext
        || s.duration < fade * 3.0 || s.duration - s.position > fade || s.position <= 0.0) {
      return;
    }
    m_tail = mpv_create();
    if (m_tail == nullptr) {
      return;
    }
    const std::string volume = std::format("{:.1f}", s.volume);
    const std::string start = std::format("{:.3f}", s.position);
    for (const auto& [name, value] : std::array<std::pair<const char*, std::string>, 12>{{
             {"vo", "null"},
             {"video", "no"},
             {"audio-display", "no"},
             {"terminal", "no"},
             {"ytdl", "yes"},
             {"ytdl-format", "bestaudio/best"},
             {"ytdl-raw-options", m_ytdlRaw},
             {"volume", volume},
             {"mute", s.muted ? "yes" : "no"},
             {"start", start},
             {"replaygain", m_replayGain},
             {"af", m_filter},
         }}) {
      mpv_set_option_string(m_tail, name, value.c_str());
    }
    if (mpv_initialize(m_tail) < 0) {
      mpv_terminate_destroy(m_tail);
      m_tail = nullptr;
      return;
    }
    const std::string item = playable(currentItem());
    const char* load[] = {"loadfile", item.c_str(), nullptr};
    mpv_command_async(m_tail, 0, load);
    // La principal pasa ya a la siguiente, en silencio, y sube con el fundido.
    double silent = -60.0;
    mpv_set_property(m_mpv, "volume-gain", MPV_FORMAT_DOUBLE, &silent);
    command({"playlist-next"});
    m_fadeStart = std::chrono::steady_clock::now();
    m_fadeTimer.startRepeating(std::chrono::milliseconds(30), [this] { stepCrossfade(); });
  }

  void AudioPlayer::stepCrossfade() {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_fadeStart).count();
    const double p = std::clamp(elapsed / std::max(1, m_crossfade), 0.0, 1.0);
    if (p >= 1.0) {
      finishCrossfade();
      return;
    }
    // Igual potencia (seno/coseno): el volumen total no se hunde a mitad del fundido.
    const auto toDb = [](double amplitude) { return std::max(-60.0, 20.0 * std::log10(std::max(amplitude, 1e-3))); };
    double in = toDb(std::sin(p * M_PI / 2.0));
    double out = toDb(std::cos(p * M_PI / 2.0));
    mpv_set_property_async(m_mpv, 0, "volume-gain", MPV_FORMAT_DOUBLE, &in);
    if (m_tail != nullptr) {
      mpv_set_property_async(m_tail, 0, "volume-gain", MPV_FORMAT_DOUBLE, &out);
    }
  }

  void AudioPlayer::finishCrossfade() {
    m_fadeTimer.stop();
    if (m_tail != nullptr) {
      mpv_terminate_destroy(m_tail);
      m_tail = nullptr;
    }
    if (m_mpv != nullptr) {
      double gain = 0.0;
      mpv_set_property_async(m_mpv, 0, "volume-gain", MPV_FORMAT_DOUBLE, &gain);
    }
  }

  void AudioPlayer::onProperty(const char* name, int format, void* data) {
    const std::string_view key = name;
    const bool present = format != MPV_FORMAT_NONE && data != nullptr;
    if (key == "time-pos") {
      m_state.position = present ? *static_cast<double*>(data) : 0.0;
    } else if (key == "duration") {
      m_state.duration = present ? *static_cast<double*>(data) : 0.0;
    } else if (key == "volume") {
      m_state.volume = present ? *static_cast<double*>(data) : m_state.volume;
    } else if (key == "pause") {
      m_state.paused = present && *static_cast<int*>(data) != 0;
    } else if (key == "mute") {
      m_state.muted = present && *static_cast<int*>(data) != 0;
    } else if (key == "idle-active") {
      m_state.idle = present && *static_cast<int*>(data) != 0;
    } else if (key == "playlist-pos") {
      m_state.current = present ? *static_cast<std::int64_t*>(data) : -1;
    } else if (key == "playlist") {
      m_state.queue.clear();
      if (present && static_cast<mpv_node*>(data)->format == MPV_FORMAT_NODE_ARRAY) {
        const auto* list = static_cast<mpv_node*>(data)->u.list;
        m_state.queue.reserve(static_cast<std::size_t>(list->num));
        for (int i = 0; i < list->num; ++i) {
          const auto* file = mapValue(list->values[i], "filename");
          std::string entry = file != nullptr && file->format == MPV_FORMAT_STRING ? file->u.string : "";
          if (const auto it = m_itemOf.find(entry); it != m_itemOf.end()) {
            entry = it->second;
          }
          m_state.queue.push_back(std::move(entry));
        }
      }
    }
  }

  std::string AudioPlayer::playable(const std::string& item) {
    std::string url = m_resolver ? m_resolver(item) : std::string();
    if (url.empty()) {
      return item;
    }
    m_itemOf[url] = item;
    return url;
  }

  std::string equalizerFilter(const EqGains& gains) {
    std::string graph;
    double boost = 0.0;
    for (std::size_t i = 0; i < gains.size(); ++i) {
      const double gain = std::clamp(gains[i], -kEqMaxGain, kEqMaxGain);
      if (std::abs(gain) < 0.05) {
        continue;
      }
      boost = std::max(boost, gain);
      graph += std::format(",equalizer=f={}:t=o:w=1:g={:.1f}", kEqBands[i], gain);
    }
    if (graph.empty()) {
      return {};
    }
    return std::format("lavfi=[volume={:.1f}dB{}]", -boost, graph);
  }

  std::string formatTime(double seconds) {
    const auto total = static_cast<long>(std::max(0.0, std::floor(seconds)));
    const long hours = total / 3600;
    const long minutes = (total / 60) % 60;
    const long secs = total % 60;
    return hours > 0 ? std::format("{}:{:02}:{:02}", hours, minutes, secs) : std::format("{}:{:02}", minutes, secs);
  }

  std::vector<std::string> shuffledWithFirst(std::vector<std::string> items, std::size_t first) {
    if (first >= items.size()) {
      return items;
    }
    std::swap(items[0], items[first]);
    std::shuffle(items.begin() + 1, items.end(), std::mt19937{std::random_device{}()});
    return items;
  }

} // namespace okaru::music
