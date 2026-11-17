# Changelog

Each version has an English and a Spanish section. / Cada versión tiene una sección en inglés y otra en español.

## [1.0.0] - 2026-11-16

### English

#### Added
- First release of Valandy, OKARU's music player: the local library organized by album, artist, song and genre,
  read once into SQLite (only new or changed files are read again), with sorting and accent-insensitive search.
- Playback with libmpv for every common audio format: queue, shuffle, repeat, gapless, crossfade, ReplayGain and a
  10-band equalizer with presets. MPRIS for OKARU's bar and media keys.
- Lyrics (embedded, `.lrc` or downloaded from LRCLIB) synced and animated, and covers from the files, the folder or
  MusicBrainz / Cover Art Archive. Sources and their priority are chosen in Settings.
- Favorites and your own playlists.
- Jellyfin and Subsonic servers: their catalog joins the library and plays by streaming; passwords and tokens stay
  in the system keyring.
- YouTube Music (optional, with `yt-dlp`): sign-in through an isolated profile of the browser you pick (the password
  only goes to Google; Valandy keeps only the session cookies, in the keyring), sync of likes, playlists and saved
  albums, catalog search, song radio and a «For you» page with your YouTube Music home and recommendations from the
  library. Upcoming songs are resolved ahead of time so skipping is instant.
- A source picker in the sidebar: everything, this computer or one server.
- Full player in the YouTube Music style (queue and lyrics), Spotify-style fullscreen (F11) with a background in the
  cover's colors, Material 3 Expressive wavy seek bar and a mini player for small floating windows.

### Español

#### Agregado
- Primera versión de Valandy, el reproductor de música de OKARU: la biblioteca local organizada por álbum, artista,
  canción y género, leída una vez a SQLite (solo se vuelven a leer los archivos nuevos o modificados), con orden y
  búsqueda sin acentos.
- Reproducción con libmpv de todos los formatos de audio comunes: cola, aleatorio, repetir, sin silencios entre
  pistas, fundido, ReplayGain y ecualizador de 10 bandas con preajustes. MPRIS para la barra de OKARU y las teclas
  multimedia.
- Letras (incrustadas, `.lrc` o descargadas de LRCLIB) sincronizadas y animadas, y carátulas de los archivos, de la
  carpeta o de MusicBrainz / Cover Art Archive. Las fuentes y su prioridad se eligen en Ajustes.
- Favoritas y listas propias.
- Servidores Jellyfin y Subsonic: su catálogo se suma a la biblioteca y se reproduce por streaming; contraseñas y
  tokens quedan en el llavero del sistema.
- YouTube Music (opcional, con `yt-dlp`): inicio de sesión con un perfil aislado del navegador que elijas (la
  contraseña solo va a Google; Valandy guarda solo las cookies de la sesión, en el llavero), sincronización de «me
  gusta», listas y álbumes guardados, búsqueda en el catálogo, radio de una canción y una página «Para ti» con tu
  portada de YouTube Music y recomendaciones de la biblioteca. Las canciones siguientes se resuelven por adelantado
  para que cambiar de canción sea inmediato.
- Selector de origen en la barra lateral: todo, este equipo o un servidor.
- Reproductor completo al estilo de YouTube Music (cola y letra), pantalla completa estilo Spotify (F11) con fondo
  en los colores de la carátula, barra de tiempo ondulada Material 3 Expressive y miniventana para ventanas
  flotantes pequeñas.
