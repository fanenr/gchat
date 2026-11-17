# SPDX-FileCopyrightText: 2026 Arthur <arthur63@163.com>
# SPDX-License-Identifier: GPL-3.0-or-later

project('gchat', 'c', version: '0.1.0',
  default_options: ['c_std=gnu11', 'warning_level=2'],
)

config = configuration_data()
config.set_quoted('GETTEXT_PACKAGE', 'gchat')
config.set_quoted('GCHAT_VERSION', meson.project_version())
config.set_quoted('LOCALEDIR', meson.current_build_dir() / 'po')
configure_file(output: 'config.h', configuration: config)
subdir('po')

gnome = import('gnome')
blueprint = find_program('blueprint-compiler')
ui_files = []
foreach name : ['shell', 'sidebar', 'content', 'settings', 'thought']
  ui_files += custom_target(name + '-ui',
    input: 'ui' / name + '.blp',
    output: name + '.ui',
    command: [blueprint, 'compile', '--output', '@OUTPUT@', '@INPUT@'],
  )
endforeach
resources = gnome.compile_resources('gchat-resources',
  'ui/gchat.gresource.xml',
  source_dir: 'ui',
  dependencies: ui_files,
)

i18n_dep = declare_dependency(
  sources: 'i18n.c',
  dependencies: dependency('glib-2.0', version: '>=2.66'),
)
store_deps = [dependency('json-glib-1.0', version: '>=1.6')]
md_deps = [
  dependency('gtk4', version: '>=4.12'),
  dependency('libadwaita-1', version: '>=1.5'),
  dependency('md4c'),
  dependency('gtksourceview-5'),
  dependency('libsoup-3.0'),
]
app_deps = store_deps + md_deps

executable('gchat', ['main.c', 'md.c', 'store.c', resources],
  dependencies: app_deps + [i18n_dep],
  link_depends: translations,
  install: false,
)

store_tests = executable('test-store', ['tests/test-store.c', 'store.c'],
  dependencies: store_deps + [i18n_dep],
  link_depends: translations,
  install: false,
)
app_tests = executable('test-app',
  ['tests/test-app.c', 'md.c', 'store.c', resources],
  dependencies: app_deps + [i18n_dep],
  link_depends: translations,
  install: false,
)
md_tests = executable('test-md', 'tests/test-md.c',
  dependencies: md_deps + [i18n_dep],
  link_depends: translations,
  install: false,
)
i18n_tests = executable('test-i18n', 'tests/test-i18n.c',
  dependencies: i18n_dep,
  link_depends: translations,
  install: false,
)
test('store', store_tests, protocol: 'tap')
test('app', app_tests, protocol: 'tap', env: ['LANGUAGE=en'])
test('app-zh_CN', app_tests, protocol: 'tap', env: ['LANGUAGE=zh_CN'])
test('markdown', md_tests, protocol: 'tap')
test('i18n', i18n_tests, protocol: 'tap')
test('translations', find_program('msgfmt'), args: [
  '--check', '--check-format', '-o', meson.current_build_dir() / 'test-zh_CN.mo',
  files('po/zh_CN.po'),
])
