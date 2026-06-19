cmake_minimum_required(VERSION 3.18)

# ---- Ignore External Flags ----
# Ensuring build consistency by ignoring environment-inherited flags
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
option(KAELUM_ENABLE_LEVEL_ZERO "Enable Intel Level Zero support" ON)
option(KAELUM_USE_SYCL "Enable SYCL compatibility" ON)

# ---- Architecture Selection ----
set(KAELUM_ARCH "native" CACHE STRING "Target architecture for compilation (default: native)")
message(STATUS "Targeting architecture: ${KAELUM_ARCH}")

# ---- Toolchain Validation ----
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Clang required. Found: ${CMAKE_CXX_COMPILER_ID}")
endif()

# ---- Compiler & Feature Flags ----
# Optimized for performance and security, mirroring Voix's professional standards
set(COMMON_FLAGS "-march=${KAELUM_ARCH} -pipe -fno-plt -fstack-protector-strong \
-fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -flto=thin -fvisibility=hidden \
-fexperimental-library -std=c++26")

set(DEBUG_FLAGS "-g -fstandalone-debug -fno-omit-frame-pointer")
set(RELEASE_FLAGS "-O3 -DNDEBUG")

set(CMAKE_CXX_FLAGS "${COMMON_FLAGS} ${DEBUG_FLAGS}" CACHE STRING "" FORCE)
string(APPEND CMAKE_CXX_FLAGS_DEBUG " ${COMMON_FLAGS} ${DEBUG_FLAGS}")
string(APPEND CMAKE_CXX_FLAGS_RELEASE " ${COMMON_FLAGS} ${RELEASE_FLAGS}")

# ---- Linker Flags ----
set(LINKER_FLAGS "-Wl,-O3 -Wl,--as-needed -Wl,-z,relro -Wl,-z,now \
-Wl,-z,noexecstack -Wl,--icf=all \
-pie -flto=thin")

if(KAELUM_USE_MOLD)
    set(LINKER_FLAGS "-fuse-ld=mold ${LINKER_FLAGS}")
else()
    set(LINKER_FLAGS "-fuse-ld=lld ${LINKER_FLAGS}")
endif()

set(CMAKE_EXE_LINKER_FLAGS "" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${LINKER_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${LINKER_FLAGS} -Wl,--strip-all" CACHE STRING "" FORCE)

# ---- Dependencies ----
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)

# liburing (io_uring)
find_library(URING_LIB uring REQUIRED)

# Wayland
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(WAYLAND_CURSOR REQUIRED wayland-cursor)

# Typography
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)

# ---- Source & Executable ----
# Split into library and executable for better organization
file(GLOB_RECURSE KAELUM_SOURCES src/*.cpp)
list(REMOVE_ITEM KAELUM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
add_library(kaelum_lib STATIC ${KAELUM_SOURCES})

add_executable(kaelum src/main.cpp)

target_link_libraries(kaelum_lib PUBLIC 
    Threads::Threads 
    ${URING_LIB} 
    Vulkan::Vulkan 
    ${WAYLAND_CLIENT_LIBRARIES}
    ${WAYLAND_CURSOR_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    util # for forkpty
)

target_include_directories(kaelum_lib PUBLIC include ${WAYLAND_CLIENT_INCLUDE_DIRS} ${WAYLAND_CURSOR_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS})

if(KAELUM_ENABLE_LEVEL_ZERO)
    # Intel Level Zero is typically provided by oneAPI
    find_path(LEVEL_ZERO_INCLUDE_DIR ze_api.h
        PATHS /usr/include/level_zero
              /opt/intel/oneapi/compiler/latest/include/level_zero
              /opt/intel/oneapi/compiler/2026.0/include/level_zero
    )
    find_library(LEVEL_ZERO_LIB ze_loader)

    if(LEVEL_ZERO_INCLUDE_DIR AND LEVEL_ZERO_LIB)
        target_link_libraries(kaelum_lib PUBLIC ${LEVEL_ZERO_LIB})
        target_include_directories(kaelum_lib PUBLIC ${LEVEL_ZERO_INCLUDE_DIR})
        target_compile_definitions(kaelum_lib PUBLIC KAELUM_WITH_LEVEL_ZERO)
        message(STATUS "Intel Level Zero enabled: ${LEVEL_ZERO_INCLUDE_DIR}")
    else()
        message(WARNING "Intel Level Zero not found. Disabling.")
    endif()
endif()

if(KAELUM_USE_SYCL)
    # SYCL typically requires the -fsycl flag and specific libraries
    find_path(SYCL_INCLUDE_DIR sycl/sycl.hpp
        PATHS /opt/intel/oneapi/compiler/latest/include
              /opt/intel/oneapi/compiler/2026.0/include
              /usr/include
    )
    
    if(SYCL_INCLUDE_DIR)
        target_compile_options(kaelum_lib PUBLIC -fsycl)
        target_include_directories(kaelum_lib PUBLIC ${SYCL_INCLUDE_DIR})
        target_compile_definitions(kaelum_lib PUBLIC KAELUM_WITH_SYCL)
        message(STATUS "SYCL compatibility enabled: ${SYCL_INCLUDE_DIR}")
    else()
        message(WARNING "SYCL headers not found. Disabling SYCL compatibility.")
    endif()
endif()

target_link_libraries(kaelum PRIVATE kaelum_lib)

target_compile_options(kaelum_lib PRIVATE -Wall -Wextra)
target_compile_options(kaelum PRIVATE -Wall -Wextra)

# ---- Shader Compilation ----
find_program(GLSLC glslc)
if(GLSLC)
    set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
    set(SHADER_SOURCES ${SHADER_DIR}/test.vert ${SHADER_DIR}/test.frag)
    foreach(SHADER_SRC ${SHADER_SOURCES})
        get_filename_component(SHADER_NAME ${SHADER_SRC} NAME)
        set(SHADER_SPV ${SHADER_DIR}/${SHADER_NAME}.spv)
        add_custom_command(
            OUTPUT ${SHADER_SPV}
            COMMAND ${GLSLC} ${SHADER_SRC} -o ${SHADER_SPV}
            DEPENDS ${SHADER_SRC}
            COMMENT "Compiling shader ${SHADER_NAME}"
        )
        list(APPEND SHADER_SPV_FILES ${SHADER_SPV})
    endforeach()
    add_custom_target(shaders ALL DEPENDS ${SHADER_SPV_FILES})
    add_dependencies(kaelum shaders)
else()
    message(WARNING "glslc not found. Shaders will not be compiled automatically.")
endif()

# ---- Installation ----
include(GNUInstallDirs)
install(TARGETS kaelum RUNTIME DESTINATION bin)
