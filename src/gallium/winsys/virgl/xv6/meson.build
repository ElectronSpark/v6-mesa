# SPDX-License-Identifier: MIT

libvirglxv6 = static_library(
  'virglxv6',
  'virgl_xv6_winsys.c',
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_gallium_drivers, inc_virtio],
  dependencies : [dep_libvirglcommon, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)
