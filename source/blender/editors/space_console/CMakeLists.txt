# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  ../include
  ../../makesrna

)

set(INC_SYS
)

set(SRC
  console_draw.cc
  console_ops.cc
  space_console.cc

  console_intern.hh
)

set(LIB
  PRIVATE bf::blenfont
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader
  PRIVATE bf::dna
  PRIVATE bf::gpu
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::windowmanager
)

if(WITH_PYTHON)
  add_definitions(-DWITH_PYTHON)
endif()


blender_add_lib(bf_editor_space_console "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
