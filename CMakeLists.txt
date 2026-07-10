cmake_minimum_required(VERSION 3.18)

# ---- Ignore External Flags ----
set(ENV{CFLAGS} "")
set(ENV{CXXFLAGS} "")
set(ENV{LDFLAGS} "")
set(CMAKE_C_FLAGS "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG "" CACHE STRING "" FORCE)

project(Kaelum VERSION 0.1.0 LANGUAGES CXX)

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
option(KAELUM_ENABLE_LEVEL_ZERO "Enable Intel Level Zero support (optional)" OFF)
option(KAELUM_USE_SYCL "Enable SYCL compatibility" OFF)
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

# ---- Optional: Intel Level Zero / oneAPI ----
if(KAELUM_ENABLE_LEVEL_ZERO)
    find_package(LevelZero QUIET)
    if(LevelZero_FOUND)
        message(STATUS "Intel Level Zero found: enabling ze_api integration")
        add_definitions(-DKAELUM_WITH_LEVEL_ZERO)
    else()
        # Fallback: try pkg-config
        pkg_check_modules(ZE_API QUIET ze_api)
        if(ZE_API_FOUND)
            message(STATUS "Intel Level Zero found via pkg-config")
            add_definitions(-DKAELUM_WITH_LEVEL_ZERO)
        else()
            message(WARNING "Intel Level Zero not found. Building without Level Zero support.")
        endif()
    endif()
else()
    message(STATUS "Intel Level Zero disabled by configuration (KAELUM_ENABLE_LEVEL_ZERO=OFF)")
endif()

# ---- Optional: SYCL ----
if(KAELUM_USE_SYCL)
    find_program(SYCL_CXX sycl-cxx QUIET)
    if(SYCL_CXX)
        message(STATUS "SYCL enabled")
        add_definitions(-DKAELUM_WITH_SYCL)
    else()
        message(WARNING "SYCL compiler not found. SYCL support disabled.")
    endif()
endif()

# ---- Shader compilation ----
find_program(GLSLANGVALIDATOR glslangValidator)
if(GLSLANGVALIDATOR)
    message(STATUS "glslangValidator found: compiling shaders")

    set(SHADER_SOURCES
        shaders/test.vert
        shaders/test.frag
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

# ---- Sources ----
set(KAELUM_SOURCES
    src/main.cpp
    src/loom.cpp
    src/nexus.cpp
    src/sigil.cpp
    src/glyph_engine.cpp
    src/xdg-shell-client-protocol.cpp
)

set(KAELUM_HEADERS
    include/common.hpp
    include/logger.hpp
    include/loom.hpp
    include/nexus.hpp
    include/sigil.hpp
    include/glyph_engine.hpp
    include/wayland-protocols/xdg-shell-client-protocol.hpp
)

# ---- Executable ----
add_executable(kaelum
    ${KAELUM_SOURCES}
    ${KAELUM_HEADERS}
)

if(TARGET kaelum_shaders)
    add_dependencies(kaelum kaelum_shaders)
endif()

target_include_directories(kaelum PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
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

if(KAELUM_ENABLE_LEVEL_ZERO AND LevelZero_FOUND)
    target_link_libraries(kaelum PRIVATE LevelZero::LevelZero)
elseif(KAELUM_ENABLE_LEVEL_ZERO AND ZE_API_FOUND)
    target_link_libraries(kaelum PRIVATE ${ZE_API_LIBRARIES})
endif()

# ---- Install ----
install(TARGETS kaelum RUNTIME DESTINATION bin)

# ---- Compiler cache workaround for CI ----
if(DEFINED ENV{CCACHE_DISABLE})
    set_target_properties(kaelum PROPERTIES
        CXX_COMPILER_LAUNCHER ""
        C_COMPILER_LAUNCHER ""
    )
endif()
