// Pruebas de la biblioteca de Valandy: agrupación por artista/álbum, búsqueda y escaneo real a SQLite.
#include "apps/music/audio_player.h"
#include "apps/music/library.h"
#include "apps/music/lyrics.h"
#include "apps/music/music_state.h"
#include "core/deferred_call.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

using namespace okaru::music;
namespace fs = std::filesystem;

namespace {

  Track track(std::string title, std::string artist, std::string album, int number, std::string albumArtist = "",
              std::string dir = "/m/a", std::string genre = "") {
    Track t;
    t.title = std::move(title);
    t.artist = std::move(artist);
    t.album = std::move(album);
    t.number = number;
    t.albumArtist = std::move(albumArtist);
    t.genre = std::move(genre);
    t.path = dir + "/" + t.title + ".flac";
    t.duration = 60.0;
    return t;
  }

  // WAV PCM de 16 bits, mono, 8 kHz, `ms` milisegundos de silencio.
  void writeWav(const fs::path& path, int ms) {
    const std::uint32_t samples = 8000U * static_cast<std::uint32_t>(ms) / 1000U;
    const std::uint32_t data = samples * 2U;
    std::ofstream out(path, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + data);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(8000);
    u32(16000);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(data);
    for (std::uint32_t i = 0; i < samples; ++i) {
      u16(0);
    }
  }

  void testOrganize() {
    Model model = organize({
        track("B2", "The Band", "Second", 2, "", "/m/band/second"),
        track("B1", "The Band", "Second", 1, "", "/m/band/second"),
        track("A1", "Artist A", "Mix", 1, "", "/m/mix", "Rock; Pop"),
        track("X1", "Artist X", "Mix", 2, "", "/m/mix", "rock"),
        track("Z1", "Zeta", "Solo", 1, "", "/m/zeta"),
    });
    assert(model.tracks.size() == 5);
    // "The Band" se ordena como "band": antes que "zeta", después de "artist x".
    assert(model.artists.size() == 4);
    assert(model.artists[0].name == "Artist A");
    assert(model.artists[1].name == "Artist X");
    assert(model.artists[2].name == "The Band");
    assert(model.artists[3].name == "Zeta");

    // El recopilatorio (misma carpeta, artistas distintos, sin artista de álbum) es un solo álbum sin artista,
    // y aparece en los dos artistas.
    std::size_t mix = model.albums.size();
    for (std::size_t i = 0; i < model.albums.size(); ++i) {
      if (model.albums[i].title == "Mix") {
        mix = i;
      }
    }
    assert(mix < model.albums.size());
    assert(model.albums[mix].tracks.size() == 2);
    assert(model.albums[mix].artist.empty());
    assert(model.artists[0].albums.size() == 1 && model.artists[0].albums[0] == mix);
    assert(model.artists[1].albums.size() == 1 && model.artists[1].albums[0] == mix);

    // Pistas del álbum en orden de número.
    const Album& second = model.albums[model.artists[2].albums[0]];
    assert(model.tracks[second.tracks[0]].title == "B1");
    assert(model.tracks[second.tracks[1]].title == "B2");

    // Géneros partidos por ";" y sin distinguir mayúsculas: Pop (1) y Rock (2).
    assert(model.genres.size() == 2);
    assert(model.genres[0].name == "Pop" && model.genres[0].tracks.size() == 1);
    assert(model.genres[1].name == "Rock" && model.genres[1].tracks.size() == 2);

    // Mismo nombre de álbum, artistas de álbum distintos: dos álbumes.
    Model two = organize({
        track("1", "A", "Greatest Hits", 1, "A", "/m/x"),
        track("2", "B", "Greatest Hits", 1, "B", "/m/x"),
    });
    assert(two.albums.size() == 2);
  }

  void testSearch() {
    Model model = organize({
        track("Canción del Mar", "Ána", "Olas", 1),
        track("Night", "Bee", "Dark", 1),
    });
    assert(search(model, "cancion").size() == 1);
    assert(search(model, "ANA olas").size() == 1);
    assert(search(model, "ana night").empty());
    assert(search(model, "   ").empty());
    assert(sortKey("The Beatles") == "beatles");
    assert(sortKey("Los Ángeles") == "angeles");
  }

  void testSort() {
    std::vector<Track> tracks{
        track("Zulu", "B", "Alpha", 1, "", "/m/b"),
        track("alpha", "A", "Omega", 1, "", "/m/a"),
        track("Mike", "A", "Omega", 2, "", "/m/a"),
    };
    tracks[0].duration = 10;
    tracks[0].mtime = 300;
    tracks[0].year = 2020;
    tracks[1].mtime = 100;
    tracks[1].year = 1999;
    const Model model = organize(std::move(tracks));
    const auto titles = [&](const std::vector<std::size_t>& order) {
      std::string out;
      for (std::size_t i : order) {
        out += model.tracks[i].title.substr(0, 1);
      }
      return out;
    };
    assert(titles(sortedTracks(model, SongSort::Artist, false)) == "aMZ");
    assert(titles(sortedTracks(model, SongSort::Title, false)) == "aMZ");
    assert(titles(sortedTracks(model, SongSort::Title, true)) == "ZMa");
    assert(titles(sortedTracks(model, SongSort::Album, false)) == "ZaM");
    assert(titles(sortedTracks(model, SongSort::Duration, false)) == "ZaM");
    assert(titles(sortedTracks(model, SongSort::Added, false)).front() == 'Z');
    const auto albums = sortedAlbums(model, AlbumSort::Year, false);
    assert(model.albums[albums[0]].title == "Alpha");
    const auto artists = sortedArtists(model, NameSort::Songs, false);
    assert(model.artists[artists[0]].name == "A");
  }

  void testLyrics() {
    const Lyrics synced = parseLyrics("[ar:Someone]\n[00:12.50]Hola\n[00:05.00][00:20]Coro\n\n[00:30.10] Fin\r\n");
    assert(synced.synced);
    assert(synced.lines.size() == 4); // la etiqueta [ar:] fuera; "Coro" dos veces
    assert(synced.lines[0].text == "Coro" && synced.lines[0].time == 5.0);
    assert(synced.lines[1].text == "Hola" && synced.lines[1].time == 12.5);
    assert(synced.lines[3].text == "Fin");
    assert(currentLine(synced, 1.0) == -1);
    assert(currentLine(synced, 12.5) == 1);
    assert(currentLine(synced, 99.0) == 3);
    const Lyrics plain = parseLyrics("\nUno\n\nDos\n\n");
    assert(!plain.synced && plain.lines.size() == 3 && plain.lines[0].text == "Uno" && plain.lines[1].text.empty());
    assert(currentLine(plain, 10.0) == -1);
    assert(parseLyrics("").empty());
  }

  void testSettings() {
    const fs::path path = fs::temp_directory_path() / ("okaru-music-state-" + std::to_string(::getpid()) + ".toml");
    MusicState state;
    state.folders = {"/m"};
    state.settings.crossfade = 6;
    state.settings.replayGain = ReplayGain::Album;
    state.settings.lyrics.move(2, -1); // internet antes que el .lrc
    state.settings.covers.enabled[static_cast<std::size_t>(Source::Online)] = false;
    assert(saveState(state, path));
    const MusicState loaded = loadState(path);
    assert(loaded.settings.crossfade == 6);
    assert(loaded.settings.replayGain == ReplayGain::Album);
    assert(loaded.settings.lyrics.order[1] == Source::Online && loaded.settings.lyrics.order[2] == Source::Local);
    assert(!loaded.settings.covers.on(Source::Online) && loaded.settings.covers.on(Source::Embedded));
    // Un orden inválido (repetido) se ignora y queda el de fábrica.
    std::ofstream(path) << "[settings.lyrics]\norder = [0, 0, 1]\n";
    assert(loadState(path).settings.lyrics.order[2] == Source::Online);
    fs::remove(path);
  }

  void testShuffle() {
    const std::vector<std::string> items{"a", "b", "c", "d", "e"};
    const auto out = shuffledWithFirst(items, 3);
    assert(out.size() == 5 && out[0] == "d");
    assert(std::is_permutation(out.begin(), out.end(), items.begin(), items.end()));
    assert(formatTime(65) == "1:05");
    assert(formatTime(3723) == "1:02:03");
  }

  void testScan() {
    const fs::path root = fs::temp_directory_path() / ("okaru-music-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "music" / "album");
    fs::create_directories(root / "music" / ".hidden");
    writeWav(root / "music" / "album" / "one.wav", 500);
    writeWav(root / "music" / "album" / "two.wav", 250);
    writeWav(root / "music" / ".hidden" / "skip.wav", 100);
    std::ofstream(root / "music" / "album" / "notes.txt") << "no";
    assert(isAudioFile("x.FLAC") && !isAudioFile("notes.txt"));

    Library library(root / "music.db", root / "covers");
    const auto runScan = [&](std::vector<fs::path> folders) {
      bool done = false;
      Model result;
      library.scan(std::move(folders), {}, [](Library::Progress) {}, [&](Model model) {
        result = std::move(model);
        done = true;
      });
      for (int i = 0; i < 500 && !done; ++i) {
        for (auto& fn : DeferredCall::takePending()) {
          fn();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      assert(done);
      return result;
    };

    Model first = runScan({root / "music"});
    assert(first.tracks.size() == 2); // la carpeta oculta y el .txt no cuentan
    assert(first.tracks[0].title == "one" || first.tracks[0].title == "two");
    for (const auto& t : first.tracks) {
      assert(t.duration > 0.2 && t.duration < 0.6);
    }
    assert(library.load().tracks.size() == 2); // quedó en la base

    // Un archivo borrado desaparece; una carpeta que no existe (disco sin montar) conserva lo suyo.
    fs::remove(root / "music" / "album" / "two.wav");
    assert(runScan({root / "music"}).tracks.size() == 1);
    assert(runScan({root / "music", root / "unmounted"}).tracks.size() == 1);
    // Sin la carpeta en la lista, sus canciones salen de la biblioteca.
    assert(runScan({}).tracks.empty());
    fs::remove_all(root);
  }

} // namespace

int main() {
  testOrganize();
  testSearch();
  testSort();
  testLyrics();
  testSettings();
  testShuffle();
  testScan();
  return 0;
}
