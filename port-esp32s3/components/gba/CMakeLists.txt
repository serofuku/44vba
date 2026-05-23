idf_component_register(
  SRCS
    "../../../src/gba.cpp"
    "../../../src/memory.cpp"
    "../../../src/sound.cpp"
    "../../../src/system.cpp"
  INCLUDE_DIRS
    "."
    "../../../src"
    "../../../libretro"
    "../../../libretro-common/include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
  -O3
  -ffast-math
  -funroll-loops
  -fomit-frame-pointer
  -finline-functions
  -finline-limit=1000
  -fno-strict-aliasing
  -fno-exceptions
  -fno-rtti
  -Wno-volatile
  -DFRONTEND_SUPPORTS_RGB565
  -DHAVE_STDINT_H
  "-DINLINE=inline __attribute__((always_inline))"
  -DLSB_FIRST
  -DUSE_FRAME_SKIP
  -DUSE_TWEAKS=1
  -DDISABLE_SOUND=1
)
