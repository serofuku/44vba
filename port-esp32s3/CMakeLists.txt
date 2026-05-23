cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Maximum performance flags for GBA emulator
add_compile_options(
  -Wno-error
  -Wno-unused-but-set-variable
  -Wno-unused-function
  -Wno-implicit-fallthrough
  -Wno-format
  -O3
  -ffast-math
  -funroll-loops
  -fomit-frame-pointer
  -finline-functions
  -fno-strict-aliasing
)

project(main)
