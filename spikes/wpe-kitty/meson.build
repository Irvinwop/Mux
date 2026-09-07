project(
  'mux-wpe-kitty',
  'c',
  version: '0.1.0',
  default_options: [
    'c_std=c17',
    'warning_level=3',
    'werror=false',
  ],
)

cc = meson.get_compiler('c')
glib_dep = dependency('glib-2.0', version: '>=2.74')
gio_dep = dependency('gio-2.0', version: '>=2.74')
runtime_enabled = get_option('runtime')

if runtime_enabled and host_machine.system() != 'linux'
  error('The Mux browser runtime requires Linux. Use -Druntime=false to build the portable core tests.')
endif

# Keep platform-independent models and protocols in one production library.
# Both runtime processes and tests link these objects, with no test-only mode.
core_library = static_library(
  'mux-core',
  [
    'mux-clipboard.c',
    'mux-clipboard-history.c',
    'mux-clipboard-broker.c',
    'mux-clipboard-control.c',
    'mux-clipboard-wire.c',
    'mux-extension-protocol.c',
    'mux-clipboard-broker-client.c',
    'mux-clipboard-broker-peer.c',
    'mux-extension-router.c',
    'mux-ui-protocol.c',
    'mux-permission-store.c',
    'mux-browser-store.c',
    'mux-uri.c',
    'src/mux-session-state.c',
  ],
  dependencies: [glib_dep, gio_dep],
  install: false,
)
core_dep = declare_dependency(
  link_with: core_library,
  dependencies: [glib_dep, gio_dep],
  include_directories: include_directories('.', 'src'),
)

core_tests = [
  'clipboard-history',
  'clipboard-broker',
  'ui-protocol',
  'clipboard-wire',
  'extension-router',
  'uri',
  'permission-store',
  'session-state',
  'browser-store',
]
foreach test_name : core_tests
  core_test = executable(
    'test-' + test_name,
    'tests/test-' + test_name + '.c',
    dependencies: core_dep,
  )
  test(test_name, core_test, suite: 'core')
endforeach

pane_overlay_test = executable(
  'test-pane-overlay',
  [
    'tests/test-pane-overlay.c',
    'mux-pane-overlay.c',
    'mux-ui-pane.c',
  ],
  dependencies: core_dep,
)
test('pane-overlay', pane_overlay_test, suite: 'core')

notification_pane_test = executable(
  'test-notification-pane',
  ['tests/test-notification-pane.c', 'mux-notification-pane.c'],
  dependencies: core_dep,
)
test('notification-pane', notification_pane_test, suite: 'core')

# Unix SOCK_SEQPACKET and peer-credential behavior must be tested on Linux.
# Core-only Linux CI exercises this transport even without WPE installed.
if host_machine.system() == 'linux'
  local_transport_test = executable(
    'test-local-transport',
    [
      'tests/test-local-transport.c',
      'mux-local-transport.c',
    ],
    dependencies: [glib_dep, gio_dep],
  )
  test('local-transport', local_transport_test, suite: 'core')
endif

if runtime_enabled
  mux_runtime_install_dir = get_option('libexecdir') / 'mux'
  engine_dependencies = [
    core_dep,
    dependency('wpe-webkit-2.0', version: '>=2.52.5'),
    dependency('wpe-platform-2.0', version: '>=2.52.5'),
  ]
  view_deps = engine_dependencies
  rt = cc.find_library('rt', required: false)
  if rt.found()
    view_deps += rt
  endif

  executable(
    'mux-view',
    [
      'src/main.c',
      'src/mux-ipc.c',
      'src/mux-protocol.c',
      'src/mux-shortcuts.c',
    ],
    dependencies: view_deps,
    install: false,
  )

  muxd_executable = executable(
    'muxd',
    [
      'src/muxd.c',
      'src/mux-protocol.c',
      'muxd-clipboard.c',
      'mux-clipboard-broker-transport.c',
      'mux-clipboard-broker-bindings.c',
      'mux-local-transport.c',
      'mux-local-source.c',
      'mux-local-endpoint.c',
    ],
    dependencies: core_dep,
    install: true,
    install_dir: mux_runtime_install_dir,
  )

  muxctl_executable = executable(
    'muxctl',
    ['src/muxctl.c', 'src/mux-protocol.c'],
    dependencies: glib_dep,
    install: true,
    install_dir: mux_runtime_install_dir,
  )

  executable(
    'mux-bar',
    ['src/mux-bar.c', 'src/mux-protocol.c', 'src/mux-shortcuts.c'],
    dependencies: glib_dep,
    install: true,
    install_dir: mux_runtime_install_dir,
  )

  executable(
    'mux-layer',
    'src/mux-layer.c',
    dependencies: glib_dep,
    install: true,
    install_dir: mux_runtime_install_dir,
  )

  executable(
    'mux-engine',
    [
      'src/mux-engine.c',
      'src/mux-engine-protocol.c',
      'src/mux-shortcuts.c',
      'mux-wpe-clipboard.c',
      'mux-clipboard-engine-link.c',
      'mux-ui-engine.c',
      'mux-download-engine.c',
      'mux-file-chooser-engine.c',
      'mux-popup-engine.c',
      'mux-browser-affordance-engine.c',
      'mux-input-method.c',
      'mux-notification-engine.c',
      'mux-navigation-policy.c',
    ],
    dependencies: engine_dependencies,
    install: true,
    install_dir: mux_runtime_install_dir,
  )

  executable(
    'mux-pane',
    [
      'src/mux-pane.c',
      'src/mux-engine-protocol.c',
      'src/mux-shortcuts.c',
      'src/mux-protocol.c',
      'mux-pane-clipboard.c',
      'mux-osc5522.c',
      'mux-kitty-clipboard.c',
      'mux-clipboard-pane-link.c',
      'mux-clipboard-broker-transport.c',
      'mux-clipboard-broker-bindings.c',
      'mux-local-transport.c',
      'mux-local-source.c',
      'mux-local-endpoint.c',
      'mux-clipboard-picker.c',
      'mux-clipboard-picker-controller.c',
      'mux-clipboard-picker-broker.c',
      'mux-ui-pane.c',
      'mux-pane-overlay.c',
      'mux-notification-pane.c',
      'mux-kitty-chooser.c',
    ],
    dependencies: core_dep,
    install: true,
    install_dir: mux_runtime_install_dir,
  )

  engine_protocol_test = executable(
    'test-engine-protocol',
    [
      'tests/test-engine-protocol.c',
      'src/mux-engine-protocol.c',
      'src/mux-engine.c',
      'src/mux-pane.c',
      'src/mux-shortcuts.c',
    ],
    c_args: ['-DMUX_ENGINE_LOGIC_TEST', '-DMUX_PANE_LOGIC_TEST'],
    dependencies: engine_dependencies,
  )
  test('engine-protocol', engine_protocol_test, suite: 'runtime')

  clipboard_engine_link_test = executable(
    'test-clipboard-engine-link',
    [
      'tests/test-clipboard-engine-link.c',
      'mux-wpe-clipboard.c',
      'mux-clipboard-engine-link.c',
    ],
    dependencies: engine_dependencies,
  )
  test('clipboard-engine-link', clipboard_engine_link_test, suite: 'runtime')

  muxd_integration_test = executable(
    'test-muxd-integration',
    ['tests/test-muxd-integration.c', 'src/mux-protocol.c'],
    dependencies: core_dep,
  )
  test(
    'muxd-integration',
    muxd_integration_test,
    args: [muxd_executable.full_path(), muxctl_executable.full_path()],
    timeout: 20,
    suite: 'runtime',
  )

  file_chooser_engine_test = executable(
    'test-file-chooser-engine',
    'tests/test-file-chooser-engine.c',
    dependencies: engine_dependencies,
  )
  test('file-chooser-engine', file_chooser_engine_test, timeout: 20, suite: 'runtime')

  download_engine_test = executable(
    'test-download-engine',
    'tests/test-download-engine.c',
    dependencies: engine_dependencies,
  )
  test('download-engine', download_engine_test, suite: 'runtime')

  popup_engine_test = executable(
    'test-popup-engine',
    'tests/test-popup-engine.c',
    dependencies: engine_dependencies,
  )
  test('popup-engine', popup_engine_test, suite: 'runtime')

  configure_file(
    input: 'packaging/mux',
    output: 'mux',
    copy: true,
    install: true,
    install_dir: get_option('bindir'),
    install_mode: 'rwxr-xr-x',
  )
  install_subdir('packaging/kitty', install_dir: get_option('datadir') / 'mux')
endif

summary({
  'Browser runtime': runtime_enabled,
  'Portable core tests': true,
  'Linux transport tests': host_machine.system() == 'linux',
}, section: 'Mux components')
