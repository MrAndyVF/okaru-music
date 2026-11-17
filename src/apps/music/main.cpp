// OKARU: Valandy (okaru-music), el reproductor de música de OKARU.
#include "apps/common/okaru_app.h"
#include "apps/music/music_window.h"

#include <clocale>

int main() {
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C"); // mpv exige LC_NUMERIC=C

  okaru::apps::OkaruApp app({
      .appId = "dev.okaru.Music",
      .dataName = "music",
      .titleKey = "apps.music.title",
      .width = 1180.0F,
      .height = 760.0F,
      .minWidth = 300.0F,
      .minHeight = 140.0F,
  });
  okaru::music::MusicWindow window(app);
  return app.run();
}
