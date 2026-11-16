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
    const std::array<std::pair<const char*, const char*>, 11> options{{
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
        {"ytdl", "no"}, // las URL de los servidores son flujos directos
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
      command({"loadfile", before[i], "insert-at", std::to_string(i)});
    }
    for (const auto& item : after) {
      command({"loadfile", item, "append"});
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
    command({"loadfile", order[first], "replace"});
    surround(
        {order.begin(), order.begin() + static_cast<std::ptrdiff_t>(first)},
        {order.begin() + static_cast<std::ptrdiff_t>(first) + 1, order.end()}
    );
  }

  void AudioPlayer::playIndex(std::size_t index) {
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
      command({"loadfile", item, "append"});
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
      command({"loadfile", *it, "insert-next"});
    }
    m_original.insert(m_original.end(), items.begin(), items.end());
  }

  void AudioPlayer::removeAt(std::size_t index) {
    if (index < m_state.queue.size()) {
      std::erase(m_original, m_state.queue[index]);
      command({"playlist-remove", std::to_string(index)});
    }
  }

  void AudioPlayer::clearQueue() {
    m_original.clear();
    command({"stop"});
  }

  void AudioPlayer::next() {
    if (m_repeat == Repeat::One) {
      // Con «repetir una», el botón igual pasa a la siguiente (loop-file solo afecta al final natural).
      command({"playlist-next", "force"});
      return;
    }
    command({"playlist-next"});
  }

  void AudioPlayer::previous() {
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
    // Terminada la cola, «reproducir» la empieza de nuevo.
    if (!paused && m_state.idle && !m_state.queue.empty()) {
      command({"playlist-play-index", "0"});
    }
    int flag = paused ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
  }

  void AudioPlayer::seekTo(double seconds) {
    command({"seek", std::format("{:.3f}", std::max(0.0, seconds)), "absolute+exact"});
  }

  void AudioPlayer::seekBy(double seconds) { command({"seek", std::format("{:.3f}", seconds), "relative+exact"}); }

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
        }
        break;
      }
      default:
        break;
      }
    }
    if (changed && m_onChange) {
      m_onChange(structural);
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
          m_state.queue.emplace_back(file != nullptr && file->format == MPV_FORMAT_STRING ? file->u.string : "");
        }
      }
    }
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
