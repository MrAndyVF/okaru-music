# okaru-music (Valandy): el reproductor de música de OKARU. Se compila contra el núcleo instalado (paquete okaru-core).
project('okaru-music', 'cpp',
  version: files('VERSION'),
  meson_version: '>=1.1',
  default_options: ['cpp_std=c++23', 'cpp_extensions=false', 'warning_level=0', 'b_ndebug=if-release'],
)

# El núcleo y sus bibliotecas son privados (/usr/lib/okaru): se enlazan con su ruta en el binario.
_okaru_libdir = get_option('prefix') / get_option('libdir') / 'okaru'
_deps = [dependency('okaru-core'), dependency('mpv'), dependency('taglib'), dependency('sqlite3'), dependency('glib-2.0'), dependency('libsecret-1')]
_inc = include_directories('src')
_args = ['-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow']
_library = files('src/apps/music/library.cpp')

executable('okaru-music',
  sources: files(
    'src/apps/music/main.cpp',
    'src/apps/music/audio_player.cpp',
    'src/apps/music/lyrics.cpp',
    'src/apps/music/online.cpp',
    'src/apps/music/playlists.cpp',
    'src/apps/music/servers.cpp',
    'src/apps/music/mpris_server.cpp',
    'src/apps/music/music_state.cpp',
    'src/apps/music/music_window.cpp',
  ) + _library,
  include_directories: _inc,
  dependencies: _deps,
  cpp_args: _args,
  install_rpath: _okaru_libdir,
  install: true,
)

# Sus textos, que el núcleo busca en /usr/share/okaru/apps/music/translations.
install_subdir('apps/music/translations', install_dir: get_option('datadir') / 'okaru' / 'apps' / 'music')
# Fuente del nombre «Valandy» (Great Vibes, SIL OFL 1.1: su licencia va al lado).
install_subdir('apps/music/fonts', install_dir: get_option('datadir') / 'okaru' / 'apps' / 'music')
install_data('assets/dev.okaru.Music.desktop', install_dir: get_option('datadir') / 'applications')

if get_option('tests').allowed()
  test('okaru_music_library', executable('okaru_music_library_test',
    sources: files('tests/okaru_music_library_test.cpp', 'src/apps/music/audio_player.cpp', 'src/apps/music/lyrics.cpp', 'src/apps/music/music_state.cpp', 'src/apps/music/playlists.cpp', 'src/apps/music/servers.cpp') + _library,
    include_directories: _inc,
    dependencies: _deps,
    cpp_args: _args,
    build_rpath: _okaru_libdir,
    override_options: ['b_ndebug=false'],
  ))
endif
