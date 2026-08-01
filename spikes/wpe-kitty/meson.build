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

if host_machine.system() != 'linux'
  error('mux-view currently requires Linux: WPEPlatform and POSIX shared memory are Linux targets')
endif

cc = meson.get_compiler('c')
glib_dep = dependency('glib-2.0', version: '>=2.74')
gio_dep = dependency('gio-2.0', version: '>=2.74')

mux_runtime_install_dir = get_option('libexecdir') / 'mux'

view_deps = [
  glib_dep,
  dependency('wpe-webkit-2.0', version: '>=2.52.5'),
  dependency('wpe-platform-2.0', version: '>=2.52.5'),
]

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
    'src/mux-session-state.c',
    'src/mux-protocol.c',
    'muxd-clipboard.c',
    'mux-clipboard.c',
    'mux-clipboard-history.c',
    'mux-clipboard-broker.c',
    'mux-clipboard-control.c',
    'mux-clipboard-wire.c',
    'mux-extension-protocol.c',
    'mux-clipboard-broker-client.c',
    'mux-clipboard-broker-peer.c',
    'mux-clipboard-broker-transport.c',
    'mux-clipboard-broker-bindings.c',
    'mux-local-transport.c',
    'mux-local-source.c',
    'mux-local-endpoint.c',
  ],
  dependencies: [glib_dep, gio_dep],
  install: true,
  install_dir: mux_runtime_install_dir,
)

muxctl_executable = executable(
  'muxctl',
  [
    'src/muxctl.c',
    'src/mux-protocol.c',
  ],
  dependencies: [glib_dep],
  install: true,
  install_dir: mux_runtime_install_dir,
)

executable(
  'mux-bar',
  [
    'src/mux-bar.c',
    'src/mux-protocol.c',
    'src/mux-shortcuts.c',
  ],
  dependencies: [glib_dep],
  install: true,
  install_dir: mux_runtime_install_dir,
)

executable(
  'mux-layer',
  'src/mux-layer.c',
  dependencies: [glib_dep],
  install: true,
  install_dir: mux_runtime_install_dir,
)

engine_dependencies = [
  dependency('glib-2.0'),
  dependency('gio-2.0'),
  dependency('wpe-webkit-2.0', version: '>=2.52.5'),
  dependency('wpe-platform-2.0', version: '>=2.52.5'),
]

executable(
  'mux-engine',
  [
    'src/mux-engine.c',
    'src/mux-engine-protocol.c',
    'src/mux-shortcuts.c',
    'mux-clipboard.c',
    'mux-clipboard-wire.c',
    'mux-wpe-clipboard.c',
    'mux-clipboard-engine-link.c',
    'mux-ui-protocol.c',
    'mux-ui-engine.c',
    'mux-permission-store.c',
    'mux-download-engine.c',
    'mux-file-chooser-engine.c',
    'mux-popup-engine.c',
    'mux-browser-affordance-engine.c',
    'mux-browser-store.c',
    'mux-input-method.c',
    'mux-notification-engine.c',
    'mux-navigation-policy.c',
    'mux-uri.c',
  ],
  dependencies: engine_dependencies,
  install: true,
  install_dir: mux_runtime_install_dir,
)

pane_dependencies = [
  dependency('glib-2.0'),
  dependency('gio-2.0'),
]

executable(
  'mux-pane',
  [
    'src/mux-pane.c',
    'src/mux-engine-protocol.c',
    'src/mux-shortcuts.c',
    'src/mux-protocol.c',
    'mux-pane-clipboard.c',
    'mux-clipboard.c',
    'mux-clipboard-history.c',
    'mux-clipboard-broker.c',
    'mux-clipboard-control.c',
    'mux-clipboard-wire.c',
    'mux-extension-protocol.c',
    'mux-osc5522.c',
    'mux-kitty-clipboard.c',
    'mux-clipboard-pane-link.c',
    'mux-clipboard-broker-client.c',
    'mux-clipboard-broker-peer.c',
    'mux-clipboard-broker-transport.c',
    'mux-clipboard-broker-bindings.c',
    'mux-local-transport.c',
    'mux-local-source.c',
    'mux-local-endpoint.c',
    'mux-clipboard-picker.c',
    'mux-clipboard-picker-controller.c',
    'mux-clipboard-picker-broker.c',
    'mux-ui-protocol.c',
    'mux-ui-pane.c',
    'mux-pane-overlay.c',
    'mux-notification-pane.c',
    'mux-kitty-chooser.c',
  ],
  dependencies: pane_dependencies,
  install: true,
  install_dir: mux_runtime_install_dir,
)
clipboard_history_test = executable(
  'test-clipboard-history',
  [
    'tests/test-clipboard-history.c',
    'mux-clipboard.c',
    'mux-clipboard-history.c',
  ],
  dependencies: [glib_dep, gio_dep],
)

test('clipboard-history', clipboard_history_test)

clipboard_broker_test = executable(
  'test-clipboard-broker',
  [
    'tests/test-clipboard-broker.c',
    'mux-clipboard.c',
    'mux-clipboard-history.c',
    'mux-clipboard-broker.c',
    'mux-clipboard-control.c',
    'mux-clipboard-wire.c',
    'mux-extension-protocol.c',
    'mux-clipboard-broker-client.c',
    'mux-clipboard-broker-peer.c',
  ],
  dependencies: [glib_dep, gio_dep],
)

test('clipboard-broker', clipboard_broker_test)

engine_protocol_test = executable(
  'test-engine-protocol',
    [
      'tests/test-engine-protocol.c',
      'src/mux-engine-protocol.c',
      'src/mux-engine.c',
      'src/mux-pane.c',
      'src/mux-shortcuts.c',
      'mux-uri.c',
    ],
    c_args: [
      '-DMUX_ENGINE_LOGIC_TEST',
      '-DMUX_PANE_LOGIC_TEST',
    ],
    dependencies: engine_dependencies,
  )

test('engine-protocol', engine_protocol_test)

ui_protocol_test = executable(
  'test-ui-protocol',
  [
    'tests/test-ui-protocol.c',
    'mux-ui-protocol.c',
  ],
  dependencies: [glib_dep, gio_dep],
)

test('ui-protocol', ui_protocol_test)

clipboard_wire_test = executable(
  'test-clipboard-wire',
  [
    'tests/test-clipboard-wire.c',
    'mux-clipboard-wire.c',
    'mux-clipboard.c',
  ],
  dependencies: [glib_dep, gio_dep],
)

test('clipboard-wire', clipboard_wire_test)

clipboard_engine_link_test = executable(
  'test-clipboard-engine-link',
  [
    'tests/test-clipboard-engine-link.c',
    'mux-clipboard.c',
    'mux-clipboard-wire.c',
    'mux-wpe-clipboard.c',
    'mux-clipboard-engine-link.c',
  ],
  dependencies: engine_dependencies,
)

test('clipboard-engine-link', clipboard_engine_link_test)

local_transport_test = executable(
  'test-local-transport',
  [
    'tests/test-local-transport.c',
    'mux-local-transport.c',
  ],
  dependencies: [glib_dep, gio_dep],
)

test('local-transport', local_transport_test)

extension_router_test = executable(
  'test-extension-router',
  [
    'tests/test-extension-router.c',
    'mux-extension-router.c',
    'mux-extension-protocol.c',
  ],
  dependencies: [glib_dep, gio_dep],
)

test('extension-router', extension_router_test)

uri_test = executable(
  'test-uri',
  [
    'tests/test-uri.c',
    'mux-uri.c',
  ],
  dependencies: [glib_dep],
)

test('uri', uri_test)

permission_store_test = executable(
  'test-permission-store',
  [
    'tests/test-permission-store.c',
    'mux-permission-store.c',
  ],
  dependencies: [glib_dep, gio_dep],
)

test('permission-store', permission_store_test)

session_state_test = executable(
  'test-session-state',
  [
    'tests/test-session-state.c',
    'src/mux-session-state.c',
  ],
  include_directories: include_directories('src'),
  dependencies: [glib_dep, gio_dep],
)

test('session-state', session_state_test)

muxd_integration_test = executable(
  'test-muxd-integration',
  [
    'tests/test-muxd-integration.c',
    'src/mux-protocol.c',
  ],
  include_directories: include_directories('src'),
  dependencies: [glib_dep, gio_dep],
)

test(
  'muxd-integration',
  muxd_integration_test,
  args: [muxd_executable.full_path(), muxctl_executable.full_path()],
  timeout: 20,
)

browser_store_test = executable(
  'test-browser-store',
  [
    'tests/test-browser-store.c',
    'mux-browser-store.c',
  ],
  dependencies: [glib_dep, gio_dep],
)

test('browser-store', browser_store_test)

file_chooser_engine_test = executable(
  'test-file-chooser-engine',
  [
    'tests/test-file-chooser-engine.c',
    'mux-ui-protocol.c',
  ],
  dependencies: engine_dependencies,
)

test(
  'file-chooser-engine',
  file_chooser_engine_test,
  timeout: 20,
)

configure_file(
  input: 'packaging/mux',
  output: 'mux',
  copy: true,
  install: true,
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x',
)

install_subdir(
  'packaging/kitty',
  install_dir: get_option('datadir') / 'mux',
)
