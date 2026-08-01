project(
  'mux-kitty-transport',
  'c',
  version: '0.1.0',
  default_options: [
    'buildtype=release',
    'c_std=c17',
    'warning_level=3',
  ],
)

compiler = meson.get_compiler('c')
glib = dependency('glib-2.0', version: '>=2.74')
math = compiler.find_library('m', required: true)
realtime = compiler.find_library('rt', required: false)
dependencies = [glib, math]

if realtime.found()
  dependencies += [realtime]
endif

executable(
  'mux-kitty-transport',
  'src/main.c',
  c_args: ['-D_POSIX_C_SOURCE=200809L'],
  dependencies: dependencies,
  install: true,
)
