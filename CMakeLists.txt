cmake_minimum_required(VERSION 3.18)

# ---- Ignore External Flags ----
set(ENV{CFLAGS} "")
set(ENV{CXXFLAGS} "")
set(ENV{LDFLAGS} "")
set(CMAKE_C_FLAGS "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG "" CACHE STRING "" FORCE)

project(Kaelum VERSION 0.1.0 LANGUAGES CXX C)

# ---- Cache Acceleration ----
if(NOT DEFINED ENV{CCACHE_DISABLE})
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        message(STATUS "ccache found: ${CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    else()
        message(WARNING "ccache not found. Proceeding with raw compilation.")
    endif()
endif()

# ---- Project Standards ----
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Options ----
option(KAELUM_USE_MOLD "Use MOLD linker instead of LLD" ON)
option(KAELUM_DEBUG "Enable debug layers" OFF)

# ---- Architecture Selection ----
set(KAELUM_ARCH "native" CACHE STRING "Target architecture for compilation (default: native)")
message(STATUS "Targeting architecture: ${KAELUM_ARCH}")

# ---- Toolchain Validation ----
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Clang required. Found: ${CMAKE_CXX_COMPILER_ID}")
endif()

# ---- Compiler & Feature Flags ----
set(COMMON_FLAGS "-march=${KAELUM_ARCH} -pipe -fno-plt -fstack-protector-strong \
-fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -flto=thin -fvisibility=hidden \
-fexperimental-library -std=c++26")

set(DEBUG_FLAGS "-g -O1 -fstandalone-debug -fno-omit-frame-pointer")
set(RELEASE_FLAGS "-O3 -DNDEBUG")

if(KAELUM_DEBUG)
    set(CMAKE_CXX_FLAGS "${COMMON_FLAGS} ${DEBUG_FLAGS}")
    add_definitions(-DKAELUM_DEBUG)
else()
    set(CMAKE_CXX_FLAGS "${COMMON_FLAGS} ${RELEASE_FLAGS}")
endif()

# ---- Linker Setup ----
if(KAELUM_USE_MOLD)
    find_program(MOLD_LINKER mold)
    if(MOLD_LINKER)
        message(STATUS "Using MOLD linker: ${MOLD_LINKER}")
        set(CMAKE_LINKER_TYPE "MOLD")
        set(CMAKE_EXE_LINKER_FLAGS "-fuse-ld=mold")
    else()
        find_program(LLD_LINKER lld)
        if(LLD_LINKER)
            message(STATUS "MOLD not found, using LLD")
            set(CMAKE_EXE_LINKER_FLAGS "-fuse-ld=lld")
        else()
            message(WARNING "Neither MOLD nor LLD found, using system default linker")
        endif()
    endif()
endif()

# ---- Dependencies ----
find_package(PkgConfig REQUIRED)

# Wayland
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-protocols)
pkg_check_modules(WAYLAND_EGL REQUIRED wayland-egl)

# Vulkan
find_package(Vulkan REQUIRED)

# FreeType
pkg_check_modules(FREETYPE REQUIRED freetype2)

# FontConfig
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)

# liburing (io_uring)
pkg_check_modules(LIBURING REQUIRED liburing)

# xkbcommon
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)

# ---- Shader compilation ----
find_program(GLSLANGVALIDATOR glslangValidator)
if(GLSLANGVALIDATOR)
    message(STATUS "glslangValidator found: compiling shaders")

    set(SHADER_SOURCES
        shaders/glyph.vert
        shaders/glyph.frag
    )

    foreach(SHADER ${SHADER_SOURCES})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPV_FILE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_NAME}.spv")

        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND ${GLSLANGVALIDATOR} -V "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}" -o ${SPV_FILE}
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}"
            COMMENT "Compiling ${SHADER_NAME} to SPIR-V"
        )

        list(APPEND SHADER_SPV_FILES ${SPV_FILE})
    endforeach()

    add_custom_target(kaelum_shaders ALL DEPENDS ${SHADER_SPV_FILES})
else()
    message(WARNING "glslangValidator not found. Using pre-compiled SPIR-V shaders. "
                    "Install glslang-tools to compile shaders from source.")
endif()

# ---- xdg-shell protocol generation ----
find_program(WAYLAND_SCANNER wayland-scanner)
if(WAYLAND_SCANNER)
    set(XDG_SHELL_XML "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
    set(XDG_SHELL_CLIENT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
    set(XDG_SHELL_CLIENT_C "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.c")

    add_custom_command(
        OUTPUT ${XDG_SHELL_CLIENT_HEADER} ${XDG_SHELL_CLIENT_C}
        COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_XML} ${XDG_SHELL_CLIENT_HEADER}
        COMMAND ${WAYLAND_SCANNER} public-code ${XDG_SHELL_XML} ${XDG_SHELL_CLIENT_C}
        DEPENDS ${XDG_SHELL_XML}
        COMMENT "Generating xdg-shell client protocol"
    )

    add_custom_target(kaelum_xdg_shell ALL DEPENDS ${XDG_SHELL_CLIENT_HEADER} ${XDG_SHELL_CLIENT_C})

    set(KAELUM_GENERATED_SOURCES ${XDG_SHELL_CLIENT_C})
    set(KAELUM_GENERATED_HEADERS ${XDG_SHELL_CLIENT_HEADER})
else()
    message(WARNING "wayland-scanner not found. xdg-shell protocol will not be generated.")
    set(KAELUM_GENERATED_SOURCES "")
    set(KAELUM_GENERATED_HEADERS "")
endif()

# ---- Sources ----
set(KAELUM_SOURCES
    src/main.cpp
    src/subprocess.cpp
    src/termio.cpp
    src/terminal.cpp
    src/renderer.cpp
    src/glyph_engine.cpp
    src/surface.cpp
    ${KAELUM_GENERATED_SOURCES}
)

set(KAELUM_HEADERS
    include/subprocess.hpp
    include/termio.hpp
    include/terminal.hpp
    include/renderer.hpp
    include/glyph_engine.hpp
    include/surface.hpp
    include/common.hpp
    include/logger.hpp
)

# ---- Executable ----
add_executable(kaelum
    ${KAELUM_SOURCES}
)

# Ensure generated C file is compiled as C
if(KAELUM_GENERATED_SOURCES)
    set_source_files_properties(${KAELUM_GENERATED_SOURCES} PROPERTIES LANGUAGE C)
endif()

if(TARGET kaelum_shaders)
    add_dependencies(kaelum kaelum_shaders)
endif()

if(TARGET kaelum_xdg_shell)
    add_dependencies(kaelum kaelum_xdg_shell)
endif()

target_include_directories(kaelum PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
    ${WAYLAND_INCLUDE_DIRS}
    ${Vulkan_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
    ${LIBURING_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
)

target_link_libraries(kaelum PRIVATE
    ${WAYLAND_LIBRARIES}
    ${Vulkan_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    ${LIBURING_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
)

# ---- Install ----
install(TARGETS kaelum RUNTIME DESTINATION bin)

# ---- Compiler cache workaround for CI ----
if(DEFINED ENV{CCACHE_DISABLE})
    set_target_properties(kaelum PROPERTIES
        CXX_COMPILER_LAUNCHER ""
        C_COMPILER_LAUNCHER ""
    )
endif()