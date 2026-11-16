#include "apps/music/mpris_server.h"

#include "apps/music/audio_player.h"
#include "apps/music/library.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace okaru::music {

  namespace {

    constexpr Logger kLog("mpris");
    const sdbus::ObjectPath kObjectPath{"/org/mpris/MediaPlayer2"};
    constexpr auto kRootInterface = "org.mpris.MediaPlayer2";
    constexpr auto kPlayerInterface = "org.mpris.MediaPlayer2.Player";
    constexpr std::string_view kBusName = "org.mpris.MediaPlayer2.okaru_music";
    constexpr double kSeekThreshold = 1.0; // segundos de diferencia con lo esperado para avisar Seeked
    constexpr double kUs = 1'000'000.0;

    std::int64_t toUs(double seconds) { return static_cast<std::int64_t>(std::llround(seconds * kUs)); }

    std::string trackPath(std::int64_t index) {
      return "/dev/okaru/Music/track/" + std::to_string(std::max<std::int64_t>(0, index));
    }

    std::string loopStatus(Repeat repeat) {
      return repeat == Repeat::One ? "Track" : repeat == Repeat::All ? "Playlist" : "None";
    }

    std::string asUri(const std::string& item) { return item.contains("://") ? item : "file://" + item; }

  } // namespace

  MprisServer::MprisServer(SessionBus& bus, AudioPlayer& player, Hooks hooks)
      : m_bus(bus), m_player(player), m_hooks(std::move(hooks)) {
    auto& connection = m_bus.connection();
    m_object = sdbus::createObject(connection, kObjectPath);

    m_object
        ->addVTable(
            sdbus::registerMethod("Raise").implementedAs([this] { m_hooks.raise(); }),
            sdbus::registerMethod("Quit").implementedAs([this] { m_hooks.quit(); }),
            sdbus::registerProperty("CanQuit").withGetter([] { return true; }),
            sdbus::registerProperty("CanRaise").withGetter([] { return false; }),
            sdbus::registerProperty("HasTrackList").withGetter([] { return false; }),
            sdbus::registerProperty("Identity").withGetter([] { return i18n::tr("apps.music.title"); }),
            sdbus::registerProperty("DesktopEntry").withGetter([] { return std::string("dev.okaru.Music"); }),
            sdbus::registerProperty("SupportedUriSchemes").withGetter([] { return std::vector<std::string>{"file"}; }),
            sdbus::registerProperty("SupportedMimeTypes").withGetter([] { return std::vector<std::string>{"audio/*"}; })
        )
        .forInterface(kRootInterface);

    m_object
        ->addVTable(
            sdbus::registerMethod("Next").implementedAs([this] { m_player.next(); }),
            sdbus::registerMethod("Previous").implementedAs([this] { m_player.previous(); }),
            sdbus::registerMethod("Pause").implementedAs([this] { m_player.setPaused(true); }),
            sdbus::registerMethod("Play").implementedAs([this] { m_player.setPaused(false); }),
            sdbus::registerMethod("PlayPause").implementedAs([this] { m_player.togglePause(); }),
            sdbus::registerMethod("Stop").implementedAs([this] {
              m_player.setPaused(true);
              m_player.seekTo(0.0);
            }),
            sdbus::registerMethod("Seek").withInputParamNames("Offset").implementedAs([this](std::int64_t offset) {
              m_player.seekBy(static_cast<double>(offset) / kUs);
            }),
            sdbus::registerMethod("SetPosition")
                .withInputParamNames("TrackId", "Position")
                .implementedAs([this](const sdbus::ObjectPath& track, std::int64_t position) {
                  const auto& state = m_player.state();
                  if (track == trackPath(state.current) && position >= 0 && position <= toUs(state.duration)) {
                    m_player.seekTo(static_cast<double>(position) / kUs);
                  }
                }),
            sdbus::registerMethod("OpenUri").withInputParamNames("Uri").implementedAs([this](const std::string& uri) {
              if (uri.starts_with("file://")) {
                m_player.playQueue({uri.substr(7)}, 0);
              }
            }),
            sdbus::registerProperty("PlaybackStatus").withGetter([this] { return snapshot().status; }),
            sdbus::registerProperty("Rate").withGetter([] { return 1.0; }).withSetter([](const double&) {}),
            sdbus::registerProperty("MinimumRate").withGetter([] { return 1.0; }),
            sdbus::registerProperty("MaximumRate").withGetter([] { return 1.0; }),
            sdbus::registerProperty("Shuffle").withGetter([this] { return m_player.shuffled(); }).withSetter(
                [this](const bool& shuffle) { m_player.setShuffle(shuffle); }
            ),
            sdbus::registerProperty("LoopStatus").withGetter([this] { return loopStatus(m_player.repeat()); }).withSetter(
                [this](const std::string& loop) {
                  m_player.setRepeat(loop == "Track" ? Repeat::One : loop == "Playlist" ? Repeat::All : Repeat::Off);
                }
            ),
            sdbus::registerProperty("Volume").withGetter(
                                                 [this] { return m_player.state().volume / 100.0; }
            ).withSetter([this](const double& volume) { m_player.setVolume(std::clamp(volume, 0.0, 1.0) * 100.0); }),
            sdbus::registerProperty("Position").withGetter([this] { return toUs(m_player.state().position); }),
            sdbus::registerProperty("Metadata").withGetter([this] {
              std::map<std::string, sdbus::Variant> metadata;
              const auto& state = m_player.state();
              metadata["mpris:trackid"] = sdbus::Variant(sdbus::ObjectPath{trackPath(state.current)});
              const std::string& item = m_player.currentItem();
              if (item.empty()) {
                return metadata;
              }
              if (state.duration > 0.0) {
                metadata["mpris:length"] = sdbus::Variant(toUs(state.duration));
              }
              metadata["xesam:url"] = sdbus::Variant(asUri(item));
              const Track* track = m_hooks.current();
              if (track == nullptr) {
                return metadata;
              }
              metadata["xesam:title"] = sdbus::Variant(track->title);
              if (!track->artist.empty()) {
                metadata["xesam:artist"] = sdbus::Variant(std::vector<std::string>{track->artist});
              }
              if (!track->albumArtist.empty()) {
                metadata["xesam:albumArtist"] = sdbus::Variant(std::vector<std::string>{track->albumArtist});
              }
              if (!track->album.empty()) {
                metadata["xesam:album"] = sdbus::Variant(track->album);
              }
              if (track->number > 0) {
                metadata["xesam:trackNumber"] = sdbus::Variant(static_cast<std::int32_t>(track->number));
              }
              if (!track->cover.empty()) {
                metadata["mpris:artUrl"] = sdbus::Variant(asUri(track->cover));
              }
              return metadata;
            }),
            sdbus::registerProperty("CanGoNext").withGetter([this] { return snapshot().canNext; }),
            sdbus::registerProperty("CanGoPrevious").withGetter([this] { return snapshot().canPrevious; }),
            sdbus::registerProperty("CanPlay").withGetter([this] { return !m_player.state().queue.empty(); }),
            sdbus::registerProperty("CanPause").withGetter([this] { return !m_player.state().queue.empty(); }),
            sdbus::registerProperty("CanSeek").withGetter([this] { return !m_player.state().idle; }),
            sdbus::registerProperty("CanControl").withGetter([] { return true; }),
            sdbus::registerSignal("Seeked").withParameters<std::int64_t>("Position")
        )
        .forInterface(kPlayerInterface);

    // Con otra ventana abierta, el nombre corto está ocupado: se usa uno por proceso.
    try {
      connection.requestName(sdbus::ServiceName{std::string(kBusName)});
    } catch (const sdbus::Error&) {
      connection.requestName(sdbus::ServiceName{std::string(kBusName) + ".instance" + std::to_string(::getpid())});
    }
    m_last = snapshot();
    m_lastPositionAt = std::chrono::steady_clock::now();
  }

  MprisServer::~MprisServer() = default;

  MprisServer::Snapshot MprisServer::snapshot() const {
    const auto& state = m_player.state();
    const bool loaded = !state.idle && state.current >= 0;
    const auto count = static_cast<std::int64_t>(state.queue.size());
    const bool wraps = m_player.repeat() == Repeat::All && count > 0;
    return {
        .status = !loaded ? "Stopped" : (state.paused ? "Paused" : "Playing"),
        .index = state.current,
        .item = m_player.currentItem(),
        .duration = state.duration,
        .volume = state.volume,
        .shuffle = m_player.shuffled(),
        .loop = loopStatus(m_player.repeat()),
        .canPrevious = loaded,
        .canNext = loaded && (wraps || state.current + 1 < count),
        .loaded = loaded,
    };
  }

  void MprisServer::update() {
    const Snapshot now = snapshot();
    std::vector<sdbus::PropertyName> changed;
    const auto check = [&](bool differs, const char* name) {
      if (differs) {
        changed.emplace_back(name);
      }
    };
    check(now.status != m_last.status, "PlaybackStatus");
    check(now.item != m_last.item || now.index != m_last.index || now.duration != m_last.duration, "Metadata");
    check(now.volume != m_last.volume, "Volume");
    check(now.shuffle != m_last.shuffle, "Shuffle");
    check(now.loop != m_last.loop, "LoopStatus");
    check(now.canNext != m_last.canNext, "CanGoNext");
    check(now.canPrevious != m_last.canPrevious, "CanGoPrevious");
    if (now.loaded != m_last.loaded) {
      changed.insert(
          changed.end(),
          {sdbus::PropertyName{"CanPlay"}, sdbus::PropertyName{"CanPause"}, sdbus::PropertyName{"CanSeek"}}
      );
    }

    // Position no avisa por PropertiesChanged: los clientes la extrapolan y solo esperan Seeked en los saltos.
    const auto clock = std::chrono::steady_clock::now();
    const double position = m_player.state().position;
    const double elapsed = std::chrono::duration<double>(clock - m_lastPositionAt).count();
    const double expected = m_lastPosition + (m_last.status == "Playing" ? elapsed : 0.0);
    const bool seeked = now.item == m_last.item && std::abs(position - expected) > kSeekThreshold;
    m_lastPosition = position;
    m_lastPositionAt = clock;
    m_last = now;

    try {
      if (!changed.empty()) {
        m_object->emitPropertiesChangedSignal(kPlayerInterface, changed);
      }
      if (seeked) {
        m_object->emitSignal("Seeked").onInterface(kPlayerInterface).withArguments(toUs(position));
      }
    } catch (const sdbus::Error& e) {
      kLog.debug("MPRIS signal failed: {}", e.what());
    }
  }

} // namespace okaru::music
