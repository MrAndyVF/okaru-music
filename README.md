# Valandy (okaru-music)

**English** · [Español](#español)

The music player of [OKARU](https://github.com/MrAndyVF/okaru) (郁): your local library organized by artist, album
and genre, with the shell's look and theme. Light and fast: tags are read once into SQLite and only new or changed
files are read again. It plays music you already have; it does not download content.

- Every common audio format (FLAC, ALAC, MP3, AAC, Opus, Vorbis, WAV, AIFF, APE, WavPack, DSD…) through libmpv,
  with gapless playback.
- Album covers (embedded or `cover.jpg`/`folder.jpg`), embedded lyrics, sorting, search without accents or case.
- Lyrics: embedded, `.lrc` next to the file or downloaded from LRCLIB, synced and animated line by line. Missing
  covers come from MusicBrainz / Cover Art Archive.
- Your own servers: Jellyfin and Subsonic (Navidrome, Gonic, Airsonic…), streamed, never downloaded.
- YouTube Music (optional, needs `yt-dlp`): sign in with an isolated profile of the browser you choose (your
  password only goes to Google), your likes, playlists and saved albums, catalog search, radio and a «For you»
  page with your recommendations. It uses YouTube Music's web API, which is not public and may change.
- Playlists and favorites, queue, equalizer, crossfade, ReplayGain, mini player and a Spotify-style fullscreen.
- MPRIS: OKARU's bar, `playerctl` and media keys control it.

## Install

It ships in OKARU's pacman repository `[okaru]` (set up by OKARU's installer):

```sh
sudo pacman -S okaru-music
```

## Build

It builds against OKARU's shared core, provided by the `okaru-core` package, plus `mpv`, `taglib`, `sqlite`, `libsecret` and `openssl`:

```sh
meson setup build && meson compile -C build
meson test -C build
# Run it with the translations from this repository:
OKARU_APPS_DIR=$PWD/apps ./build/okaru-music
```

Translations live in `apps/music/translations/` (English and Spanish).

## License

MIT.

---

## Español

El reproductor de música de [OKARU](https://github.com/MrAndyVF/okaru) (郁): tu biblioteca local organizada por
artista, álbum y género, con el aspecto y el tema de la shell. Ligero y rápido: las etiquetas se leen una vez a
SQLite y solo se vuelven a leer los archivos nuevos o modificados. Reproduce la música que ya tienes; no descarga
contenido.

- Todos los formatos de audio comunes (FLAC, ALAC, MP3, AAC, Opus, Vorbis, WAV, AIFF, APE, WavPack, DSD…) con
  libmpv, sin silencios entre pistas.
- Carátulas (incrustadas o `cover.jpg`/`folder.jpg`), letras incrustadas, orden, búsqueda sin acentos ni mayúsculas.
- Letras: incrustadas, `.lrc` junto al archivo o descargadas de LRCLIB, sincronizadas y animadas línea a línea. Las
  carátulas que faltan salen de MusicBrainz / Cover Art Archive.
- Tus servidores: Jellyfin y Subsonic (Navidrome, Gonic, Airsonic…), por streaming, sin descargar nada.
- YouTube Music (opcional, necesita `yt-dlp`): inicio de sesión con un perfil aislado del navegador que elijas (tu
  contraseña solo va a Google), tus «me gusta», listas y álbumes guardados, búsqueda en el catálogo, radio y una
  página «Para ti» con tus recomendaciones. Usa la API de la web de YouTube Music, que no es pública y puede cambiar.
- Listas y favoritas, cola, ecualizador, fundido, ReplayGain, miniventana y pantalla completa estilo Spotify.
- MPRIS: la barra de OKARU, `playerctl` y las teclas multimedia lo controlan.

### Instalar

Viene en el repositorio pacman de OKARU, `[okaru]` (lo configura el instalador de OKARU):

```sh
sudo pacman -S okaru-music
```

### Compilar

Se compila contra el núcleo compartido de OKARU, del paquete `okaru-core`, más `mpv`, `taglib`, `sqlite`, `libsecret` y `openssl`:

```sh
meson setup build && meson compile -C build
meson test -C build
# Para ejecutarlo con las traducciones de este repositorio:
OKARU_APPS_DIR=$PWD/apps ./build/okaru-music
```

Las traducciones están en `apps/music/translations/` (inglés y español).

### Licencia

MIT.
