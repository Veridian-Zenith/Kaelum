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

# ---- Architecture Selection ----
set(KAELUM_ARCH "native" CACHE STRING "Target architecture for compilation (default: native)")
message(STATUS "Targeting architecture: ${KAELUM_ARCH}")

# ---- Toolchain Validation ----
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Clang required. Found: ${CMAKE_CXX_COMPILER_ID}")
endif()

# ---- Compiler & Feature Flags ----
# Optimized for performance and security, mirroring Voix's professional standards
set(COMMON_FLAGS "-march=${KAELUM_ARCH} -Oz -pipe -fno-plt -fstack-protector-strong \
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
    util # for forkpty
)

target_include_directories(kaelum_lib PUBLIC include ${WAYLAND_CLIENT_INCLUDE_DIRS})

if(KAELUM_ENABLE_LEVEL_ZERO)
    # Intel Level Zero is typically provided by oneAPI
    # We search for it in standard and oneAPI paths
    find_path(LEVEL_ZERO_INCLUDE_DIR level_zero.h
        PATHS /opt/intel/oneapi/compiler/latest/include
              /opt/intel/oneapi/compiler/2026.0/include
              /usr/include
    )
    find_library(LEVEL_ZERO_LIB ze_loader)

    if(LEVEL_ZERO_INCLUDE_DIR AND LEVEL_ZERO_LIB)
        target_link_libraries(kaelum_lib PUBLIC ${LEVEL_ZERO_LIB})
        target_include_directories(kaelum_lib PUBLIC ${LEVEL_ZERO_INCLUDE_DIR})
        target_compile_definitions(kaelum_lib PUBLIC KAELUM_WITH_LEVEL_ZERO)
        message(STATUS "Intel Level Zero enabled.")
    else()
        message(WARNING "Intel Level Zero not found. Disabling.")
    endif()
endif()

target_link_libraries(kaelum PRIVATE kaelum_lib)

target_compile_options(kaelum_lib PRIVATE -Wall -Wextra)
target_compile_options(kaelum PRIVATE -Wall -Wextra)

# ---- Installation ----
include(GNUInstallDirs)
install(TARGETS kaelum RUNTIME DESTINATION bin)
