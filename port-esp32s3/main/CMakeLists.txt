idf_component_register(
    SRCS
        "main.cpp"
        "os.c"
    INCLUDE_DIRS
        "."
        "../../src"
        "../../libretro"
        "../../libretro-common/include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -ffast-math
    -funroll-loops
    -fomit-frame-pointer
    -finline-functions
    -fno-strict-aliasing
    -Wno-error
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-implicit-fallthrough
    -Wno-format
    -Wno-volatile
    -DFRONTEND_SUPPORTS_RGB565
    -DHAVE_STDINT_H
    "-DINLINE=inline __attribute__((always_inline))"
    -DLSB_FIRST
    -DUSE_FRAME_SKIP
    -DDISABLE_SOUND=1
    -DUSE_TWEAKS=1
)
