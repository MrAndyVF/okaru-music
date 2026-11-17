// Pruebas de la biblioteca de Valandy: agrupación por artista/álbum, búsqueda y escaneo real a SQLite.
#include "apps/music/audio_player.h"
#include "apps/music/library.h"
#include "apps/music/lyrics.h"
#include "apps/music/music_state.h"
#include "apps/music/playlists.h"
#include "apps/music/servers.h"
#include "apps/music/youtube.h"
#include "apps/music/recommendations.h"
#include "core/deferred_call.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sqlite3.h>
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
    state.settings.equalizer = true;
    state.settings.eqGains[0] = 6.0;
    state.settings.lyrics.move(2, -1); // internet antes que el .lrc
    state.settings.covers.enabled[static_cast<std::size_t>(Source::Online)] = false;
    state.extras = {{.path = "yt://ab/v1", .title = "Canción", .artist = "Alguien", .duration = 180.5}};
    assert(saveState(state, path));
    const MusicState loaded = loadState(path);
    assert(loaded.settings.crossfade == 6);
    assert(loaded.settings.replayGain == ReplayGain::Album);
    assert(loaded.settings.equalizer && loaded.settings.eqGains[0] == 6.0 && loaded.settings.eqGains[1] == 0.0);
    assert(loaded.settings.lyrics.order[1] == Source::Online && loaded.settings.lyrics.order[2] == Source::Local);
    assert(!loaded.settings.covers.on(Source::Online) && loaded.settings.covers.on(Source::Embedded));
    assert(loaded.extras.size() == 1 && loaded.extras[0].title == "Canción" && loaded.extras[0].duration == 180.5);
    // Un orden inválido (repetido) se ignora y queda el de fábrica.
    std::ofstream(path) << "[settings.lyrics]\norder = [0, 0, 1]\n";
    assert(loadState(path).settings.lyrics.order[2] == Source::Online);
    fs::remove(path);
  }

  void testPlaylists() {
    const fs::path path = fs::temp_directory_path() / ("okaru-music-playlists-" + std::to_string(::getpid()) + ".db");
    fs::remove(path);
    {
      Playlists lists(path);
      assert(lists.all().empty());
      assert(lists.toggleLike("/m/a.flac", "local")); // nace «Favoritas» de este equipo
      assert(lists.all().size() == 1 && lists.all()[0].favorites && lists.liked("/m/a.flac"));
      assert(lists.toggleLike("/m/b.flac", "local"));
      assert(lists.all()[0].items.front() == "/m/b.flac"); // la más reciente arriba
      assert(!lists.toggleLike("/m/a.flac", "local") && !lists.liked("/m/a.flac"));
      assert(lists.toggleLike("yt://ab/v1", "ab")); // otro origen: otra «Favoritas»
      assert(lists.all().size() == 2 && lists.all()[0].origin == "ab" && lists.all()[0].items.size() == 1);
      assert(!lists.toggleLike("yt://ab/v1", "ab") && lists.all()[0].items.empty());
      const auto id = lists.create("Viaje", "");
      lists.add(id, {"/m/x.flac", "/m/y.flac", "/m/x.flac"}); // sin repetidas
      assert(lists.find(id)->items.size() == 2);
      lists.removeItem(id, 0);
      lists.rename(id, "Ruta");
      assert(lists.find(id)->items == std::vector<std::string>{"/m/y.flac"});
      lists.create("Rock", "ab");
    }
    Playlists reopened(path); // todo quedó guardado, con su origen
    assert(reopened.all().size() == 4 && reopened.all()[0].favorites && reopened.all()[1].favorites);
    assert(reopened.all()[2].name == "Ruta" && reopened.all()[2].origin.empty() && reopened.all()[3].origin == "ab");
    assert(reopened.liked("/m/b.flac") && !reopened.liked("/m/a.flac"));
    reopened.remove(reopened.all()[3].id);
    reopened.remove(reopened.all()[2].id);
    assert(reopened.all().size() == 2 && reopened.liked("/m/b.flac"));
    for (const char* suffix : {"", "-wal", "-shm"}) {
      fs::remove(path.string() + suffix);
    }
  }

  void testRemote() {
    Server jf{.id = "ab12", .kind = ServerKind::Jellyfin};
    Server sub{.id = "cd34", .kind = ServerKind::Subsonic};
    assert(remotePrefix(jf) == "jf://ab12/" && remotePrefix(sub) == "sub://cd34/");
    assert(isRemote("jf://ab12/x/y") && isRemote("sub://a/b/c") && !isRemote("/home/m/a.flac"));
    RemoteRef ref;
    assert(parseRemote("jf://ab12/album9/item7", ref) && ref.server == "ab12" && ref.album == "album9" && ref.item == "item7");
    assert(parseRemote("sub://cd34/_/song1", ref) && ref.album == "_" && ref.item == "song1");
    assert(!parseRemote("/home/a.flac", ref) && !parseRemote("jf://ab12/solo", ref));
    assert(normalizeUrl("  music.example.com/ ") == "https://music.example.com");
    assert(normalizeUrl("http://192.168.1.5:4533//") == "http://192.168.1.5:4533");
  }

  void testShuffle() {
    const std::vector<std::string> items{"a", "b", "c", "d", "e"};
    const auto out = shuffledWithFirst(items, 3);
    assert(out.size() == 5 && out[0] == "d");
    assert(std::is_permutation(out.begin(), out.end(), items.begin(), items.end()));
    assert(formatTime(65) == "1:05");
    assert(formatTime(3723) == "1:02:03");
    // Ecualizador: plano = sin filtro; con ganancia, baja el volumen lo que más sube y omite las bandas en 0.
    assert(equalizerFilter(kEqPresets[0].gains).empty());
    EqGains gains{};
    gains[0] = 6.0;
    gains[9] = -20.0; // se limita a -12
    assert(equalizerFilter(gains) == "lavfi=[volume=-6.0dB,equalizer=f=31:t=o:w=1:g=6.0,equalizer=f=16000:t=o:w=1:g=-12.0]");
  }

  void testColors() {
    // Mitad roja y mitad casi negra: salen los dos, el rojo (vivo) primero; la transparencia no cuenta.
    std::vector<std::uint8_t> rgba;
    for (int i = 0; i < 100; ++i) {
      const bool red = i < 50;
      rgba.insert(rgba.end(), {static_cast<std::uint8_t>(red ? 220 : 10), 10, 10, 255});
    }
    rgba.insert(rgba.end(), {0, 255, 0, 0});
    const auto colors = dominantColors(rgba, 5);
    assert(colors.size() == 2);
    assert(colors[0][0] > 0.8F && colors[1][0] < 0.1F);
    assert(dominantColors({}, 5).empty());
  }

  void testYouTube() {
    // Filas como las de InnerTube: búsqueda (artista • álbum • duración en una columna), «me gusta» (artista y
    // álbum en columnas, duración fija) y un artista (sin videoId: no es canción). La miniatura sale grande.
    const auto run = [](const std::string& text, const std::string& type = "", const std::string& id = "") {
      nlohmann::json r{{"text", text}};
      if (!type.empty()) {
        r["navigationEndpoint"]["browseEndpoint"] = {
            {"browseId", id},
            {"browseEndpointContextSupportedConfigs", {{"browseEndpointContextMusicConfig", {{"pageType", type}}}}}};
      }
      return r;
    };
    const auto col = [](nlohmann::json runs) {
      return nlohmann::json{{"musicResponsiveListItemFlexColumnRenderer", {{"text", {{"runs", std::move(runs)}}}}}};
    };
    const nlohmann::json response{{"contents", {
        {{"musicResponsiveListItemRenderer", {
            {"playlistItemData", {{"videoId", "217Bw85-5cw"}}},
            {"thumbnail", {{"musicThumbnailRenderer", {{"thumbnail", {{"thumbnails", {{{"url", "https://x/a=w60-h60-l90-rj"}}, {{"url", "https://x/a=w120-h120-l90-rj"}}}}}}}}}},
            {"flexColumns", {col({run("Adiós")}),
                             col({run("Gustavo Cerati", "MUSIC_PAGE_TYPE_ARTIST", "UC1"), run(" • "),
                                  run("Ahí Vamos", "MUSIC_PAGE_TYPE_ALBUM", "MPREb_1"), run(" • "), run("3:54")})}}}}},
        {{"musicResponsiveListItemRenderer", {
            {"flexColumns", {col({{{"text", "Afuera"}, {"navigationEndpoint", {{"watchEndpoint", {{"videoId", "abcdefghijk"}}}}}}}),
                             col({run("Caifanes", "MUSIC_PAGE_TYPE_ARTIST", "UC2"), run(" & "), run("Saúl", "MUSIC_PAGE_TYPE_ARTIST", "UC3")}),
                             col({run("El Nervio Del Volcán", "MUSIC_PAGE_TYPE_ALBUM", "MPREb_2")})}},
            {"fixedColumns", {{{"musicResponsiveListItemFixedColumnRenderer", {{"text", {{"runs", {run("1:02:03")}}}}}}}}}}}},
        {{"musicResponsiveListItemRenderer", {{"flexColumns", {col({run("Gustavo Cerati", "MUSIC_PAGE_TYPE_ARTIST", "UC1")})}}}}},
        {{"continuationItemRenderer", {{"continuationEndpoint", {{"continuationCommand", {{"token", "TOKEN"}}}}}}}},
    }}};
    const auto tracks = parseYtTracks(response);
    assert(tracks.size() == 2);
    assert(tracks[0].videoId == "217Bw85-5cw" && tracks[0].title == "Adiós" && tracks[0].artist == "Gustavo Cerati");
    assert(tracks[0].album == "Ahí Vamos" && tracks[0].albumId == "MPREb_1" && tracks[0].duration == 234.0);
    assert(ytCoverSize(tracks[0].thumbnail, 544) == "https://x/a=w544-h544-l90-rj");
    assert(tracks[1].videoId == "abcdefghijk" && tracks[1].artist == "Caifanes, Saúl" && tracks[1].duration == 3723.0);
    assert(parseYtContinuation(response).token == "TOKEN" && parseYtContinuation(response).inBody);
    assert(parseYtDuration("12 M reproducciones") == 0.0);

    // Cookies: lectura, sesión iniciada, firma y formato para yt-dlp.
    const std::string header = "VISITOR=v; SID=s; SAPISID=abc; __Secure-3PAPISID=def";
    assert(cookieValue(header, "SAPISID") == "abc" && cookieValue(header, "SID") == "s" && cookieValue(header, "X").empty());
    assert(hasYouTubeSession(header) && !hasYouTubeSession("VISITOR=v"));
    gchar* sha = g_compute_checksum_for_string(G_CHECKSUM_SHA1, "1700000000 abc https://music.youtube.com", -1);
    const std::string auth = sapisidAuthorization(header, 1700000000);
    assert(auth.starts_with(std::string("SAPISIDHASH 1700000000_") + sha) && auth.contains(" SAPISID3PHASH 1700000000_"));
    g_free(sha);
    assert(netscapeCookies("A=1; B=x=y").contains("\tA\t1\n") && netscapeCookies("A=1; B=x=y").contains("\tB\tx=y\n"));

    // Perfil de Firefox simulado: solo las de youtube.com, sin repetir (gana el dominio general).
    const fs::path profile = fs::temp_directory_path() / ("okaru-music-ff-" + std::to_string(::getpid()));
    fs::create_directories(profile / "Default");
    sqlite3* db = nullptr;
    sqlite3_open((profile / "cookies.sqlite").c_str(), &db);
    sqlite3_exec(db, "CREATE TABLE moz_cookies(host TEXT, name TEXT, value TEXT);"
                     "INSERT INTO moz_cookies VALUES('.youtube.com','SID','s'),('music.youtube.com','SID','other'),"
                     "('.google.com','SAPISID','g'),('.youtube.com','SAPISID','y');", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    assert(readFirefoxCookies(profile) == "SID=s; SAPISID=y");

    // Chromium con --password-store=basic: AES-128-CBC con la clave de «peanuts», prefijo v10 y (versión ≥ 24) el
    // SHA-256 del dominio delante del valor.
    unsigned char key[16];
    PKCS5_PBKDF2_HMAC_SHA1("peanuts", 7, reinterpret_cast<const unsigned char*>("saltysalt"), 9, 1, 16, key);
    unsigned char iv[16];
    std::fill(std::begin(iv), std::end(iv), ' ');
    const std::string plain = std::string(32, 'h') + "secret-sid";
    std::string cipher(plain.size() + 16, '\0');
    int n1 = 0, n2 = 0;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key, iv);
    EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(cipher.data()), &n1, reinterpret_cast<const unsigned char*>(plain.data()), static_cast<int>(plain.size()));
    EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(cipher.data()) + n1, &n2);
    EVP_CIPHER_CTX_free(ctx);
    cipher.resize(static_cast<std::size_t>(n1 + n2));
    const std::string blob = "v10" + cipher;
    sqlite3_open((profile / "Default" / "Cookies").c_str(), &db);
    sqlite3_exec(db, "CREATE TABLE meta(key TEXT, value TEXT); INSERT INTO meta VALUES('version','24');"
                     "CREATE TABLE cookies(host_key TEXT, name TEXT, value TEXT, encrypted_value BLOB);", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO cookies VALUES('.youtube.com','SID','',?)", -1, &stmt, nullptr);
    sqlite3_bind_blob(stmt, 1, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    assert(readChromiumCookies(profile) == "SID=secret-sid");
    fs::remove_all(profile);
  }

  void testRecommendations() {
    // Radio ("next"): título, artista enlazado, álbum, duración; sin repetir.
    const auto link = [](const std::string& text, const std::string& type, const std::string& id) {
      return nlohmann::json{{"text", text}, {"navigationEndpoint", {{"browseEndpoint", {{"browseId", id},
          {"browseEndpointContextSupportedConfigs", {{"browseEndpointContextMusicConfig", {{"pageType", type}}}}}}}}}};
    };
    const nlohmann::json item{{"playlistPanelVideoRenderer", {
        {"videoId", "YVXNPPdh6wE"}, {"title", {{"runs", {{{"text", "Crimen"}}}}}},
        {"longBylineText", {{"runs", {link("Gustavo Cerati", "MUSIC_PAGE_TYPE_ARTIST", "UC1"), {{"text", " • "}},
                                      link("Ahí Vamos", "MUSIC_PAGE_TYPE_ALBUM", "MPREb_1"), {{"text", " • "}}, {{"text", "2006"}}}}}},
        {"lengthText", {{"runs", {{{"text", "3:53"}}}}}}}}};
    const auto radio = parseYtPanel(nlohmann::json{{"items", {item, item}}});
    assert(radio.size() == 1 && radio[0].title == "Crimen" && radio[0].artist == "Gustavo Cerati");
    assert(radio[0].album == "Ahí Vamos" && radio[0].duration == 233.0);
    assert(isVideoId("YVXNPPdh6wE") && !isVideoId("x; rm -rf /") && !isVideoId("short"));
    // Con sesión, cada canción trae su «counterpart» (el video): no se repite. Y un videoclip suelto se salta.
    nlohmann::json video = item;
    video["playlistPanelVideoRenderer"]["videoId"] = "VIDEOxxxxx1";
    video["playlistPanelVideoRenderer"]["navigationEndpoint"]["watchEndpoint"]["watchEndpointMusicSupportedConfigs"]
         ["watchEndpointMusicConfig"]["musicVideoType"] = "MUSIC_VIDEO_TYPE_OMV";
    const nlohmann::json wrapped{{"playlistPanelVideoWrapperRenderer", {{"primaryRenderer", item}, {"counterpart", {{{"counterpartRenderer", video}}}}}}};
    const auto deduped = parseYtPanel(nlohmann::json{{"items", {wrapped, video}}});
    assert(deduped.size() == 1 && deduped[0].videoId == "YVXNPPdh6wE");

    // Portada: canción, álbum y lista; los artistas (ni canción ni lista) se saltan.
    const auto two = [](const std::string& title, nlohmann::json nav) {
      return nlohmann::json{{"musicTwoRowItemRenderer", {{"title", {{"runs", {{{"text", title}}}}}},
          {"subtitle", {{"runs", {{{"text", "Canción"}}, {{"text", " • "}}, {{"text", "Artista"}}}}}}, {"navigationEndpoint", std::move(nav)}}}};
    };
    const auto browse = [](const std::string& id, const std::string& type) {
      return nlohmann::json{{"browseEndpoint", {{"browseId", id},
          {"browseEndpointContextSupportedConfigs", {{"browseEndpointContextMusicConfig", {{"pageType", type}}}}}}}};
    };
    const nlohmann::json home{{"contents", {{{"musicCarouselShelfRenderer", {
        {"header", {{"musicCarouselShelfBasicHeaderRenderer", {{"title", {{"runs", {{{"text", "Vuelve a escucharlo"}}}}}}}}}},
        {"contents", {two("Adiós", {{"watchEndpoint", {{"videoId", "217Bw85-5cw"}}}}),
                      two("Ahí Vamos", browse("MPREb_1", "MUSIC_PAGE_TYPE_ALBUM")),
                      two("Mi supermix", browse("VLRDTMAK5", "MUSIC_PAGE_TYPE_PLAYLIST")),
                      two("Cerati", browse("UC1", "MUSIC_PAGE_TYPE_ARTIST"))}}}}}}}};
    const auto shelves = parseYtHome(home);
    assert(shelves.size() == 1 && shelves[0].title == "Vuelve a escucharlo" && shelves[0].cards.size() == 3);
    assert(shelves[0].cards[0].kind == YtCard::Kind::Song && shelves[0].cards[0].videoId == "217Bw85-5cw");
    assert(shelves[0].cards[0].subtitle == "Canción • Artista");
    assert(shelves[0].cards[0].artist == "Artista"); // sin enlace: lo último del subtítulo
    assert(shelves[0].cards[1].kind == YtCard::Kind::Album && shelves[0].cards[2].kind == YtCard::Kind::Playlist);

    // Biblioteca: recientes por fecha del archivo (los de servidores no cuentan); redescubre estable por día.
    Model model;
    for (int i = 0; i < 4; ++i) {
      Track t;
      t.path = i < 3 ? "/m/" + std::to_string(i) : "yt://s/_/abcdefghijk";
      t.album = "A" + std::to_string(i);
      t.mtime = i < 3 ? 100 + i : 0;
      model.tracks.push_back(t);
      model.albums.push_back({.title = t.album, .tracks = {static_cast<std::size_t>(i)}});
    }
    assert((recentAlbums(model, 2) == std::vector<std::size_t>{2, 1}));
    assert(recentAlbums(model, 10).size() == 3);
    assert(rediscoverAlbums(model, 3, 7) == rediscoverAlbums(model, 3, 7) && rediscoverAlbums(model, 10, 1).size() == 4);
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
    // Las de un servidor no son de las carpetas: el escaneo local no las toca; su sincronización las reemplaza.
    Track remote;
    remote.path = "jf://ab12/al1/it1";
    remote.title = "Remota";
    library.replaceRemote("jf://ab12/", {remote});
    assert(runScan({root / "music"}).tracks.size() == 2);
    remote.path = "jf://ab12/al1/it2";
    library.replaceRemote("jf://ab12/", {remote});
    const Model synced = library.load();
    assert(synced.tracks.size() == 2);
    assert(std::ranges::any_of(synced.tracks, [](const Track& t) { return t.path == "jf://ab12/al1/it2"; }));
    library.replaceRemote("jf://ab12/", {});
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
  testPlaylists();
  testRemote();
  testShuffle();
  testColors();
  testYouTube();
  testRecommendations();
  testScan();
  return 0;
}
