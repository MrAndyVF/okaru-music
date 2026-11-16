# Valandy (okaru-music)

**English** · [Español](#español)

The music player of [OKARU](https://github.com/MrAndyVF/okaru) (郁): your local library organized by artist, album
and genre, with the shell's look and theme. Light and fast: tags are read once into SQLite and only new or changed
files are read again. It plays music you already have; it does not download content.

- Every common audio format (FLAC, ALAC, MP3, AAC, Opus, Vorbis, WAV, AIFF, APE, WavPack, DSD…) through libmpv,
  with gapless playback.
- Album covers (embedded or `cover.jpg`/`folder.jpg`), embedded lyrics, sorting, search without accents or case.
- MPRIS: OKARU's bar, `playerctl` and media keys control it.

## Install

It ships in OKARU's pacman repository `[okaru]` (set up by OKARU's installer):

```sh
sudo pacman -S okaru-music
```

## Build

It builds against OKARU's shared core, provided by the `okaru-core` package, plus `mpv`, `taglib` and `sqlite`:

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
- MPRIS: la barra de OKARU, `playerctl` y las teclas multimedia lo controlan.

### Instalar

Viene en el repositorio pacman de OKARU, `[okaru]` (lo configura el instalador de OKARU):

```sh
sudo pacman -S okaru-music
```

### Compilar

Se compila contra el núcleo compartido de OKARU, del paquete `okaru-core`, más `mpv`, `taglib` y `sqlite`:

```sh
meson setup build && meson compile -C build
meson test -C build
# Para ejecutarlo con las traducciones de este repositorio:
OKARU_APPS_DIR=$PWD/apps ./build/okaru-music
```

Las traducciones están en `apps/music/translations/` (inglés y español).

### Licencia

MIT.
