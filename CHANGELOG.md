# Changelog

Each version has an English and a Spanish section. / Cada versión tiene una sección en inglés y otra en español.

## [1.1.4] - 2026-11-25

### English

#### Changes
- Sidebar rows (sections, playlists, settings) are taller and airier, like the Settings modules.
- Dialog cards get a 2 px accent border, like OKARU panels; menus follow the system border width.
- Released with OKARU 1.4.0, which brings square controls and 2 px borders to every app.
- CI: an optional `.okaru-core-ref` file builds the app against an unpublished okaru-core (branch, tag or commit).

### Español

#### Cambios
- Las filas de la barra lateral (secciones, playlists, ajustes) son más altas y con más aire, como los módulos de
  Ajustes.
- Las tarjetas de diálogo llevan un borde de acento de 2 px, como los paneles de OKARU; los menús siguen el grosor de
  borde del sistema.
- Sale con OKARU 1.4.0, que trae controles cuadrados y bordes de 2 px a todas las apps.
- CI: un archivo `.okaru-core-ref` opcional compila la app contra un okaru-core sin publicar (rama, etiqueta o
  commit).

## [1.1.3] - 2026-11-22

### English

#### Fixes
- **Covers come back when the cover cache is deleted.** If `~/.cache/okaru/music/covers` disappeared, Valandy kept
  pointing at the missing files and could not download new ones; it now recreates the folder and reads again the
  songs whose cover is gone (local covers are extracted again, online ones downloaded again). Released with OKARU
  1.3.9.

### Español

#### Correcciones
- **Las carátulas vuelven si se borra su caché.** Si desaparecía `~/.cache/okaru/music/covers`, Valandy seguía
  apuntando a los archivos que faltaban y no podía descargar otros; ahora recrea la carpeta y vuelve a leer las
  canciones cuya carátula ya no está (las locales se extraen de nuevo y las de internet se descargan otra vez). Sale
  con OKARU 1.3.9.

## [1.1.2] - 2026-11-22

### English

#### Changes
- Packaging: the package is **tied to the system libraries it was built against**. When a system update would
  rename one of them, pacman refuses it and names this package, instead of leaving the app unable to start; it goes
  through once OKARU publishes it rebuilt. Released with OKARU 1.3.8.

### Español

#### Cambios
- Empaquetado: el paquete queda **atado a las bibliotecas del sistema con las que se compiló**. Si una actualización
  del sistema le cambiaría el nombre a alguna, pacman se niega y nombra este paquete, en vez de dejar la app sin
  abrir; pasa en cuanto OKARU lo publica recompilado. Sale con OKARU 1.3.8.

## [1.1.1] - 2026-11-18

### English

#### Fixes
- **Playlists also show YouTube Music songs that are not in your library** (added from search, radio or For you),
  with their source in All sources. Their title, artist and cover are kept between sessions; before, only songs in
  the queue kept them.
- **Remove from playlist** removes the right song. Before, it could remove a different one when the playlist had
  hidden songs.
- **A damaged `music.toml` is set aside** as `music.toml.danado-<date>` instead of being overwritten with the
  defaults on close, which lost the servers, the queue and the settings.

### Español

#### Correcciones
- **Las listas también muestran las canciones de YouTube Music que no están en tu biblioteca** (agregadas desde la
  búsqueda, la radio o Para ti), con su origen en Todos los orígenes. Su título, artista y carátula se guardan entre
  sesiones; antes solo los guardaban las de la cola.
- **Quitar de la lista** quita la canción correcta. Antes podía quitar otra cuando la lista tenía canciones ocultas.
- **Un `music.toml` dañado se aparta** como `music.toml.danado-<fecha>` en vez de pisarse con los valores de fábrica
  al cerrar, lo que hacía perder los servidores, la cola y los ajustes.

## [1.1.0] - 2026-11-17

### English

#### Features
- **Playlists by source.** With a source picked, the sidebar shows only that source's playlists, and new ones belong
  to it. With «All sources» every playlist shows up with its source next to its name, and the ones created there mix
  sources, with each song labeled with where it comes from. «Add to» only offers playlists that accept the song.
- **One Favorites per source**: a like on a YouTube Music song goes to YouTube Music's Favorites, a local one to this
  computer's. The single Favorites of 1.0.0 is split by source on first start.
- The queue remembers title, artist, album, cover and length of YouTube Music songs that are not in the library.

#### Changes
- **Only YouTube Music songs**: music videos, channel uploads, episodes and unavailable (greyed out) items are left
  out of sync, search, playlists and «For you», even when they are in your likes.
- «For you» covers are downloaded at 544 px, sharp in the big player; the ones unused for 30 days are deleted.

### Español

#### Novedades
- **Listas por origen.** Con un origen elegido, el panel lateral muestra solo sus listas, y las nuevas quedan en él.
  Con «Todos» aparecen todas con su origen junto al nombre, y las que se crean ahí mezclan orígenes, con cada canción
  marcada con el suyo. «Agregar a» solo ofrece las listas que aceptan la canción.
- **Una Favoritas por origen**: el «me gusta» de una canción de YouTube Music va a la Favoritas de YouTube Music, y el
  de una local, a la de este equipo. La Favoritas única de la 1.0.0 se reparte por origen al abrir.
- La cola recuerda título, artista, álbum, carátula y duración de las canciones de YouTube Music que no están en la
  biblioteca.

#### Cambios
- **Solo canciones de YouTube Music**: los videoclips, videos de canales, episodios y lo no disponible (en gris) quedan
  fuera de la sincronización, la búsqueda, las listas y «Para ti», aunque estén en tus «me gusta».
- Las carátulas de «Para ti» se descargan a 544 px, nítidas en el reproductor grande; las que no se usan en 30 días
  se borran.

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
